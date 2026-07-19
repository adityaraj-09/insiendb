#include "executor.h"
#include "storage_engine/index_catalog.h"
#include "storage_engine/index_key.h"
#include <stdexcept>
#include <cmath>

Executor::Executor(Catalog& catalog, Storage& storage) : catalog(catalog), storage(storage) {}

// ---------------- small numeric helpers ----------------
// The analyzer already guaranteed these values are numeric wherever these
// get called; get<>() would throw std::bad_variant_access if that promise
// were ever broken, which is exactly the "fail loudly" behavior we want
// for a bug in the analyzer rather than silent wrong answers.

static double asDouble(const Value& v) {
    if (v.type == Type::INT) return static_cast<double>(std::get<long long>(v.data));
    if (v.type == Type::FLOAT) return std::get<double>(v.data);
    throw std::runtime_error("asDouble: value is not numeric (" + typeToString(v.type) + ")");
}

static long long asInt(const Value& v) {
    if (v.type != Type::INT)
        throw std::runtime_error("asInt: value is not INT (" + typeToString(v.type) + ")");
    return std::get<long long>(v.data);
}

// (asBool was here previously — every call site now goes through toTri()/
// fromTri() instead, since a "boolean" result in this engine can also
// legitimately be NULL/UNKNOWN, which plain bool can't represent.)

// ---------------- three-valued logic ----------------
// SQL's NULL means "unknown", so any comparison touching NULL is neither
// true nor false — it's UNKNOWN. AND/OR/NOT all need proper truth tables
// for this third state, not just C++ bool. A Value carrying this state is
// represented as Type::NUL; Value::makeBool(...) covers the other two.
enum class Tri { TRUE_, FALSE_, UNKNOWN };

static Tri toTri(const Value& v) {
    if (v.type == Type::NUL) return Tri::UNKNOWN;
    return std::get<bool>(v.data) ? Tri::TRUE_ : Tri::FALSE_;
}

static Value fromTri(Tri t) {
    switch (t) {
        case Tri::TRUE_:  return Value::makeBool(true);
        case Tri::FALSE_: return Value::makeBool(false);
        default:          return Value::makeNull(); // UNKNOWN
    }
}

static Tri triAnd(Tri a, Tri b) {
    if (a == Tri::FALSE_ || b == Tri::FALSE_) return Tri::FALSE_;   // FALSE dominates
    if (a == Tri::UNKNOWN || b == Tri::UNKNOWN) return Tri::UNKNOWN;
    return Tri::TRUE_;
}

static Tri triOr(Tri a, Tri b) {
    if (a == Tri::TRUE_ || b == Tri::TRUE_) return Tri::TRUE_;      // TRUE dominates
    if (a == Tri::UNKNOWN || b == Tri::UNKNOWN) return Tri::UNKNOWN;
    return Tri::FALSE_;
}

static Tri triNot(Tri a) {
    if (a == Tri::UNKNOWN) return Tri::UNKNOWN;
    return a == Tri::TRUE_ ? Tri::FALSE_ : Tri::TRUE_;
}

// ---------------- expression evaluation ----------------
// Same dynamic_cast dispatch shape as SemanticAnalyzer::resolveExpr — this
// is the third question we ask of the same tree ("what value, for THIS
// row", vs. earlier "what type, in general"). Post-order: evaluate
// children first, then combine.

Value evalExpr(const Expr* expr, const Row& row) {
    if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
        switch (lit->resolvedType) {
            case Type::INT:   return Value::makeInt(std::stoll(lit->value));
            case Type::FLOAT: return Value::makeFloat(std::stod(lit->value));
            case Type::TEXT:  return Value::makeText(lit->value);
            case Type::BOOL:  return Value::makeBool(lit->value == "true");
            case Type::NUL:   return Value::makeNull();
            default:
                throw std::runtime_error("evalExpr: literal has unresolved type — was this AST analyzed?");
        }
    }

    if (auto* col = dynamic_cast<const ColumnRefExpr*>(expr)) {
        if (col->resolvedIndex < 0 || static_cast<size_t>(col->resolvedIndex) >= row.size())
            throw std::runtime_error("evalExpr: column '" + col->column + "' has no resolved index");
        return row[col->resolvedIndex]; // <-- direct index, no name comparison at all
    }

    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        if (un->op == "IS_NULL" || un->op == "IS_NOT_NULL") {
            Value operand = evalExpr(un->operand.get(), row);
            bool isNull = (operand.type == Type::NUL);
            return Value::makeBool(un->op == "IS_NULL" ? isNull : !isNull);
        }
        Value operand = evalExpr(un->operand.get(), row);
        if (un->op == "NOT") return fromTri(triNot(toTri(operand)));
        // unary '-': NULL propagates (NULL negated is still "unknown value")
        if (operand.type == Type::NUL) return Value::makeNull();
        if (un->resolvedType == Type::FLOAT) return Value::makeFloat(-asDouble(operand));
        return Value::makeInt(-asInt(operand));
    }

    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        const std::string& op = bin->op;

        // AND/OR short-circuit on the value that DETERMINES the result
        // (FALSE for AND, TRUE for OR) — not just "the left side has a
        // definite value", since UNKNOWN doesn't determine anything.
        if (op == "AND") {
            Tri l = toTri(evalExpr(bin->left.get(), row));
            if (l == Tri::FALSE_) return Value::makeBool(false);
            return fromTri(triAnd(l, toTri(evalExpr(bin->right.get(), row))));
        }
        if (op == "OR") {
            Tri l = toTri(evalExpr(bin->left.get(), row));
            if (l == Tri::TRUE_) return Value::makeBool(true);
            return fromTri(triOr(l, toTri(evalExpr(bin->right.get(), row))));
        }

        Value l = evalExpr(bin->left.get(), row);
        Value r = evalExpr(bin->right.get(), row);

        if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            // Comparing anything with NULL is UNKNOWN, full stop — this is
            // exactly why "col = NULL" can never match a row; you need
            // "col IS NULL" instead.
            if (l.type == Type::NUL || r.type == Type::NUL) return Value::makeNull();

            bool result;
            if (l.type == Type::TEXT && r.type == Type::TEXT) {
                const std::string& a = std::get<std::string>(l.data);
                const std::string& b = std::get<std::string>(r.data);
                if (op == "=") result = (a == b);
                else if (op == "!=") result = (a != b);
                else if (op == "<") result = (a < b);
                else if (op == "<=") result = (a <= b);
                else if (op == ">") result = (a > b);
                else result = (a >= b);
            } else if (l.type == Type::BOOL && r.type == Type::BOOL) {
                bool a = std::get<bool>(l.data);
                bool b = std::get<bool>(r.data);
                if (op == "=") result = (a == b);
                else if (op == "!=") result = (a != b);
                else if (op == "<") result = (a < b);
                else if (op == "<=") result = (a <= b);
                else if (op == ">") result = (a > b);
                else result = (a >= b);
            } else {
                double a = asDouble(l), b = asDouble(r);
                if (op == "=") result = (a == b);
                else if (op == "!=") result = (a != b);
                else if (op == "<") result = (a < b);
                else if (op == "<=") result = (a <= b);
                else if (op == ">") result = (a > b);
                else result = (a >= b);
            }
            return Value::makeBool(result);
        }

        // arithmetic: bin->resolvedType (set by the analyzer) tells us
        // whether to do integer or floating-point math.
        if (l.type == Type::NUL || r.type == Type::NUL) return Value::makeNull();

        if (bin->resolvedType == Type::FLOAT) {
            double a = asDouble(l), b = asDouble(r);
            if (op == "+") return Value::makeFloat(a + b);
            if (op == "-") return Value::makeFloat(a - b);
            if (op == "*") return Value::makeFloat(a * b);
            return Value::makeFloat(a / b); // "/"
        } else {
            long long a = asInt(l), b = asInt(r);
            if (op == "+") return Value::makeInt(a + b);
            if (op == "-") return Value::makeInt(a - b);
            if (op == "*") return Value::makeInt(a * b);
            if (b == 0) throw std::runtime_error("Division by zero");
            return Value::makeInt(a / b); // integer division
        }
    }

    throw std::runtime_error("evalExpr: unknown expression node");
}

static bool rowMatchesWhere(const Expr* whereClause, const Row& row) {
    if (!whereClause) return true;
    Value cond = evalExpr(whereClause, row);
    return (cond.type == Type::BOOL) && std::get<bool>(cond.data);
}

// ---------------- statement execution ----------------

void Executor::executeCreateTable(CreateTableStmt* stmt) {
    const TableSchema* schema = catalog.getTable(stmt->tableName);
    if (!schema)
        throw std::runtime_error("executeCreateTable: table not in catalog");
    storage.createTable(*schema);
}

void Executor::executeCreateIndex(CreateIndexStmt* stmt) {
    const IndexSchema* idx = catalog.getIndex(stmt->indexName);
    if (!idx)
        throw std::runtime_error("executeCreateIndex: index not in catalog");
    storage.createIndex(idx->name, idx->tableName, idx->columnName);
}

struct ColumnComparison {
    std::string table;
    std::string column;
    std::string op;
    Value bound;
    bool valid = false;
};

struct JoinEquality {
    int leftIndex = -1;
    int rightIndex = -1;
    std::string leftTable;
    std::string rightTable;
    std::string leftColumn;
    std::string rightColumn;
    bool valid = false;
};

static Value literalToValue(const LiteralExpr* lit) {
    static const Row emptyRow;
    return evalExpr(lit, emptyRow);
}

static Value coerceBoundForIndex(const Value& bound, Type columnType) {
    if (columnType == Type::FLOAT && bound.type == Type::INT)
        return Value::makeFloat(static_cast<double>(std::get<long long>(bound.data)));
    return bound;
}

static bool isComparisonOp(const std::string& op) {
    return op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

static bool tryExtractColumnComparison(const Expr* expr, ColumnComparison& out) {
    auto* bin = dynamic_cast<const BinaryExpr*>(expr);
    if (!bin || !isComparisonOp(bin->op)) return false;

    const ColumnRefExpr* col = nullptr;
    const LiteralExpr* lit = nullptr;

    if (auto* leftCol = dynamic_cast<const ColumnRefExpr*>(bin->left.get())) {
        lit = dynamic_cast<const LiteralExpr*>(bin->right.get());
        col = leftCol;
    } else if (auto* rightCol = dynamic_cast<const ColumnRefExpr*>(bin->right.get())) {
        lit = dynamic_cast<const LiteralExpr*>(bin->left.get());
        col = rightCol;
    }
    if (!col || !lit) return false;

    out.table = col->table;
    out.column = col->column;
    out.op = bin->op;
    out.bound = literalToValue(lit);
    out.valid = true;
    return true;
}

static bool tryExtractJoinEquality(const Expr* on, JoinEquality& out) {
    auto* bin = dynamic_cast<const BinaryExpr*>(on);
    if (!bin || bin->op != "=") return false;

    auto* leftCol = dynamic_cast<const ColumnRefExpr*>(bin->left.get());
    auto* rightCol = dynamic_cast<const ColumnRefExpr*>(bin->right.get());
    if (!leftCol || !rightCol) return false;
    if (leftCol->table.empty() || rightCol->table.empty()) return false;
    if (leftCol->resolvedIndex < 0 || rightCol->resolvedIndex < 0) return false;

    out.leftIndex = leftCol->resolvedIndex;
    out.rightIndex = rightCol->resolvedIndex;
    out.leftTable = leftCol->table;
    out.rightTable = rightCol->table;
    out.leftColumn = leftCol->column;
    out.rightColumn = rightCol->column;
    out.valid = true;
    return true;
}

static IndexCompareOp comparisonToIndexOp(const std::string& op) {
    if (op == "=") return IndexCompareOp::Eq;
    if (op == "!=") return IndexCompareOp::Ne;
    if (op == "<") return IndexCompareOp::Lt;
    if (op == "<=") return IndexCompareOp::Le;
    if (op == ">") return IndexCompareOp::Gt;
    if (op == ">=") return IndexCompareOp::Ge;
    throw std::runtime_error("comparisonToIndexOp: unsupported op");
}

static const IndexSchema* findIndexForComparison(const Catalog& catalog, const std::string& baseTable,
                                                 const ColumnComparison& cmp) {
    std::string table = cmp.table.empty() ? baseTable : cmp.table;
    return catalog.findIndexOnColumn(table, cmp.column);
}

static bool planWhereIndex(const Catalog& catalog, const std::string& baseTable, const Expr* where,
                           ColumnComparison& cmp, const IndexSchema*& index, bool& fullyIndexed) {
    index = nullptr;
    fullyIndexed = false;
    if (!where) return false;

    if (tryExtractColumnComparison(where, cmp)) {
        index = findIndexForComparison(catalog, baseTable, cmp);
        fullyIndexed = index != nullptr;
        return index != nullptr;
    }

    auto* andExpr = dynamic_cast<const BinaryExpr*>(where);
    if (andExpr && andExpr->op == "AND") {
        ColumnComparison side;
        if (tryExtractColumnComparison(andExpr->left.get(), side)) {
            index = findIndexForComparison(catalog, baseTable, side);
            if (index) {
                cmp = side;
                return true;
            }
        }
        if (tryExtractColumnComparison(andExpr->right.get(), side)) {
            index = findIndexForComparison(catalog, baseTable, side);
            if (index) {
                cmp = side;
                return true;
            }
        }
    }
    return false;
}

void Executor::executeInsert(InsertStmt* stmt) {
    const TableSchema* schema = catalog.getTable(stmt->tableName);
    Row row(schema->columns.size(), Value::makeNull()); // unset columns default to NULL

    // INSERT ... VALUES never references table columns, so evalExpr gets
    // an empty row — any ColumnRefExpr here would be a bug the analyzer
    // should already have caught.
    static const Row emptyRow;
    for (size_t i = 0; i < stmt->values.size(); i++) {
        Value v = evalExpr(stmt->values[i].get(), emptyRow);
        int targetIndex = stmt->resolvedColumnIndices[i];
        row[targetIndex] = std::move(v);
    }

    storage.insertRow(stmt->tableName, std::move(row));
}

int Executor::executeUpdate(UpdateStmt* stmt) {
    int updated = 0;

    storage.scanTableWithRids(stmt->tableName, [&](RowId rid, const Row& row) {
        Row mutableRow = row;
        if (!rowMatchesWhere(stmt->whereClause.get(), mutableRow)) return;

        for (auto& assign : stmt->assignments) {
            Value v = evalExpr(assign.value.get(), mutableRow);
            mutableRow[assign.resolvedColumnIndex] = std::move(v);
        }

        if (storage.updateRow(stmt->tableName, rid, mutableRow))
            updated++;
    });

    return updated;
}

int Executor::executeDelete(DeleteStmt* stmt) {
    int deleted = 0;

    storage.scanTableWithRids(stmt->tableName, [&](RowId rid, const Row& row) {
        if (!rowMatchesWhere(stmt->whereClause.get(), row)) return;
        if (storage.deleteRow(stmt->tableName, rid))
            deleted++;
    });

    return deleted;
}

static std::vector<Row> fetchRowsByIndex(Storage& storage, const IndexSchema* index,
                                         IndexCompareOp op, const Value& bound) {
    Value coerced = coerceBoundForIndex(bound, index->columnType);
    if (!valueIsIndexable(coerced, index->columnType))
        return {};
    return storage.indexSearchRows(index->name, op, coerced);
}

QueryResult Executor::executeSelect(SelectStmt* stmt) {
    std::vector<Row> compositeRows;

    ColumnComparison whereCmp;
    const IndexSchema* whereIndex = nullptr;
    bool whereFullyIndexed = false;
    bool useWhereIndex = planWhereIndex(catalog, stmt->tableName, stmt->whereClause.get(),
                                        whereCmp, whereIndex, whereFullyIndexed);

    if (useWhereIndex) {
        compositeRows = fetchRowsByIndex(storage, whereIndex,
                                         comparisonToIndexOp(whereCmp.op), whereCmp.bound);
    } else {
        compositeRows = storage.scanTable(stmt->tableName);
    }

    for (auto& join : stmt->joins) {
        JoinEquality eq;
        tryExtractJoinEquality(join.onCondition.get(), eq);

        const IndexSchema* joinIndex = nullptr;
        int probeIndex = -1;
        if (eq.valid) {
            if (eq.rightTable == join.tableName) {
                joinIndex = catalog.findIndexOnColumn(join.tableName, eq.rightColumn);
                probeIndex = eq.leftIndex;
            } else if (eq.leftTable == join.tableName) {
                joinIndex = catalog.findIndexOnColumn(join.tableName, eq.leftColumn);
                probeIndex = eq.rightIndex;
            }
        }

        std::vector<Row> nextComposite;
        if (joinIndex && probeIndex >= 0) {
            for (const Row& leftComposite : compositeRows) {
                if (probeIndex >= static_cast<int>(leftComposite.size())) continue;
                const Value& probe = leftComposite[probeIndex];
                std::vector<Row> rightRows = fetchRowsByIndex(storage, joinIndex, IndexCompareOp::Eq, probe);
                if (rightRows.empty() && !valueIsIndexable(probe, joinIndex->columnType)) {
                    rightRows = storage.scanTable(join.tableName);
                    for (const Row& rightRow : rightRows) {
                        Row combined = leftComposite;
                        combined.insert(combined.end(), rightRow.begin(), rightRow.end());
                        if (rowMatchesWhere(join.onCondition.get(), combined))
                            nextComposite.push_back(std::move(combined));
                    }
                    continue;
                }
                for (const Row& rightRow : rightRows) {
                    Row combined = leftComposite;
                    combined.insert(combined.end(), rightRow.begin(), rightRow.end());
                    if (rowMatchesWhere(join.onCondition.get(), combined))
                        nextComposite.push_back(std::move(combined));
                }
            }
        } else {
            std::vector<Row> rightRows = storage.scanTable(join.tableName);
            for (const Row& leftComposite : compositeRows) {
                for (const Row& rightRow : rightRows) {
                    Row combined = leftComposite;
                    combined.insert(combined.end(), rightRow.begin(), rightRow.end());
                    if (rowMatchesWhere(join.onCondition.get(), combined))
                        nextComposite.push_back(std::move(combined));
                }
            }
        }
        compositeRows = std::move(nextComposite);
    }

    QueryResult result;
    result.columnNames = stmt->resolvedColumnNames;

    for (const Row& row : compositeRows) {
        if (!whereFullyIndexed && !rowMatchesWhere(stmt->whereClause.get(), row)) continue;

        Row projected;
        projected.reserve(stmt->resolvedColumnIndices.size());
        for (int idx : stmt->resolvedColumnIndices) projected.push_back(row[idx]);
        result.rows.push_back(std::move(projected));
    }

    return result;
}
