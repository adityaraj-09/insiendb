#include "semantic_analyzer.h"
#include "storage_engine/index_key.h"
#include <cctype>
#include <algorithm>

SemanticAnalyzer::SemanticAnalyzer(Catalog& catalog) : catalog(catalog) {}

void SemanticAnalyzer::error(const std::string& msg, int line) {
    throw SemanticError(msg, line);
}

// ---------------- dispatcher ----------------

void SemanticAnalyzer::analyze(Statement* stmt) {
    if (auto* ct = dynamic_cast<CreateTableStmt*>(stmt)) { analyzeCreateTable(ct); return; }
    if (auto* ci = dynamic_cast<CreateIndexStmt*>(stmt)) { analyzeCreateIndex(ci); return; }
    if (auto* ins = dynamic_cast<InsertStmt*>(stmt))     { analyzeInsert(ins); return; }
    if (auto* upd = dynamic_cast<UpdateStmt*>(stmt))     { analyzeUpdate(upd); return; }
    if (auto* del = dynamic_cast<DeleteStmt*>(stmt))   { analyzeDelete(del); return; }
    if (auto* sel = dynamic_cast<SelectStmt*>(stmt))     { analyzeSelect(sel); return; }
    error("Unknown statement type", stmt->line);
}

// ---------------- CREATE TABLE ----------------
// Checks: no duplicate table, no duplicate column names, every declared
// type is one this engine actually understands. On success, registers
// the table into the catalog — this is the one place semantic analysis
// has a *side effect* rather than just validating.

void SemanticAnalyzer::analyzeCreateTable(CreateTableStmt* stmt) {
    if (catalog.hasTable(stmt->tableName))
        error("Table '" + stmt->tableName + "' already exists", stmt->line);

    TableSchema schema;
    schema.name = stmt->tableName;

    for (size_t i = 0; i < stmt->columns.size(); i++) {
        const auto& col = stmt->columns[i];

        for (size_t j = 0; j < i; j++) {
            if (stmt->columns[j].name == col.name)
                error("Duplicate column name '" + col.name + "'", stmt->line);
        }

        std::string upperType = col.type;
        std::transform(upperType.begin(), upperType.end(), upperType.begin(),
                        [](unsigned char c) { return std::toupper(c); });

        Type t;
        if (upperType == "INT") t = Type::INT;
        else if (upperType == "FLOAT") t = Type::FLOAT;
        else if (upperType == "TEXT") t = Type::TEXT;
        else if (upperType == "BOOL") t = Type::BOOL;
        else { error("Unknown column type '" + col.type + "'", stmt->line); }

        schema.columns.push_back(ColumnSchema{col.name, t, static_cast<int>(i)});
    }

    catalog.createTable(schema);
}

// ---------------- CREATE INDEX ----------------

void SemanticAnalyzer::analyzeCreateIndex(CreateIndexStmt* stmt) {
    if (catalog.getIndex(stmt->indexName))
        error("Index '" + stmt->indexName + "' already exists", stmt->line);

    const TableSchema* schema = catalog.getTable(stmt->tableName);
    if (!schema)
        error("Table '" + stmt->tableName + "' does not exist", stmt->line);

    const ColumnSchema* col = schema->findColumn(stmt->columnName);
    if (!col)
        error("Column '" + stmt->columnName + "' does not exist on table '" + stmt->tableName + "'", stmt->line);
    if (!IndexKey::isIndexableType(col->type))
        error("Column type cannot be indexed", stmt->line);
    if (catalog.findIndexOnColumn(stmt->tableName, stmt->columnName))
        error("Index on " + stmt->tableName + "." + stmt->columnName + " already exists", stmt->line);

    stmt->resolvedColumnIndex = col->index;

    IndexSchema idx;
    idx.name = stmt->indexName;
    idx.tableName = stmt->tableName;
    idx.columnName = stmt->columnName;
    idx.columnIndex = col->index;
    idx.columnType = col->type;
    catalog.createIndex(idx);
}

// ---------------- INSERT ----------------
// Checks: table exists; explicit column list (if given) names real,
// non-duplicate columns; value count matches column count; each value's
// type is compatible with its target column's type.

void SemanticAnalyzer::analyzeInsert(InsertStmt* stmt) {
    const TableSchema* schema = catalog.getTable(stmt->tableName);
    if (!schema) error("Table '" + stmt->tableName + "' does not exist", stmt->line);

    // Figure out which column each value targets, in order.
    std::vector<const ColumnSchema*> targets;
    if (stmt->columns.empty()) {
        // No explicit list -> values must line up with the schema, in order.
        for (auto& c : schema->columns) targets.push_back(&c);
    } else {
        for (size_t i = 0; i < stmt->columns.size(); i++) {
            const std::string& name = stmt->columns[i];
            for (size_t j = 0; j < i; j++) {
                if (stmt->columns[j] == name)
                    error("Column '" + name + "' specified twice in INSERT", stmt->line);
            }
            const ColumnSchema* col = schema->findColumn(name);
            if (!col) error("Column '" + name + "' does not exist on table '" + stmt->tableName + "'", stmt->line);
            targets.push_back(col);
        }
    }

    if (targets.size() != stmt->values.size())
        error("Column count (" + std::to_string(targets.size()) + ") does not match value count (" +
              std::to_string(stmt->values.size()) + ")", stmt->line);

    stmt->resolvedColumnIndices.clear();
    for (auto* col : targets) stmt->resolvedColumnIndices.push_back(col->index);

    // Values don't reference table columns, but resolveExpr needs a scope
    // to resolve literals' types uniformly; an empty/self scope is fine
    // since INSERT ... VALUES never legally contains a column reference.
    Scope scope;
    for (size_t i = 0; i < stmt->values.size(); i++) {
        Type valueType = resolveExpr(stmt->values[i].get(), scope);
        Type targetType = targets[i]->type;

        bool compatible = (valueType == targetType) || (isNumeric(valueType) && isNumeric(targetType));
        if (!compatible)
            error("Value " + std::to_string(i + 1) + " is " + typeToString(valueType) +
                  " but column '" + targets[i]->name + "' is " + typeToString(targetType),
                  stmt->values[i]->line);
    }
}

// ---------------- UPDATE ----------------

void SemanticAnalyzer::analyzeUpdate(UpdateStmt* stmt) {
    const TableSchema* schema = catalog.getTable(stmt->tableName);
    if (!schema) error("Table '" + stmt->tableName + "' does not exist", stmt->line);
    if (stmt->assignments.empty())
        error("UPDATE requires at least one SET assignment", stmt->line);

    Scope scope;
    int rowOffset = 0;
    addTableToScope(scope, rowOffset, stmt->tableName, stmt->line);

    for (auto& assign : stmt->assignments) {
        int colIndex;
        const ColumnSchema* col = resolveColumn(scope, "", assign.column, stmt->line, colIndex);
        assign.resolvedColumnIndex = colIndex;

        Type valueType = resolveExpr(assign.value.get(), scope);
        bool compatible = (valueType == col->type) || (isNumeric(valueType) && isNumeric(col->type));
        if (!compatible)
            error("Cannot assign " + typeToString(valueType) + " to column '" + col->name +
                  "' of type " + typeToString(col->type), assign.value->line);
    }

    if (stmt->whereClause) {
        Type whereType = resolveExpr(stmt->whereClause.get(), scope);
        if (whereType != Type::BOOL)
            error("WHERE clause must evaluate to BOOL, got " + typeToString(whereType),
                  stmt->whereClause->line);
    }
}

// ---------------- DELETE ----------------

void SemanticAnalyzer::analyzeDelete(DeleteStmt* stmt) {
    const TableSchema* schema = catalog.getTable(stmt->tableName);
    if (!schema) error("Table '" + stmt->tableName + "' does not exist", stmt->line);

    Scope scope;
    int rowOffset = 0;
    addTableToScope(scope, rowOffset, stmt->tableName, stmt->line);

    if (stmt->whereClause) {
        Type whereType = resolveExpr(stmt->whereClause.get(), scope);
        if (whereType != Type::BOOL)
            error("WHERE clause must evaluate to BOOL, got " + typeToString(whereType),
                  stmt->whereClause->line);
    }
}

// ---------------- SELECT ----------------
// Checks: table exists; every selected column name resolves; the WHERE
// expression tree type-checks and its root type is BOOL.

void SemanticAnalyzer::addTableToScope(Scope& scope, int& rowOffset, const std::string& tableName, int line) {
    const TableSchema* schema = catalog.getTable(tableName);
    if (!schema) error("Table '" + tableName + "' does not exist", line);

    for (auto& entry : scope) {
        if (entry.alias == tableName)
            error("Table '" + tableName + "' appears more than once in FROM clause", line);
    }

    scope.push_back(ScopeEntry{tableName, schema, rowOffset});
    rowOffset += static_cast<int>(schema->columns.size());
}

void SemanticAnalyzer::analyzeSelect(SelectStmt* stmt) {
    Scope scope;
    int rowOffset = 0;
    addTableToScope(scope, rowOffset, stmt->tableName, stmt->line);

    for (auto& join : stmt->joins) {
        addTableToScope(scope, rowOffset, join.tableName, stmt->line);
        Type onType = resolveExpr(join.onCondition.get(), scope);
        if (onType != Type::BOOL)
            error("JOIN ON clause must evaluate to BOOL, got " + typeToString(onType),
                  join.onCondition->line);
    }

    bool isStar = stmt->columns.size() == 1 && stmt->columns[0] == "*";
    stmt->resolvedColumnIndices.clear();
    stmt->resolvedColumnNames.clear();
    if (isStar) {
        for (auto& entry : scope) {
            for (auto& col : entry.schema->columns) {
                stmt->resolvedColumnIndices.push_back(entry.rowOffset + col.index);
                stmt->resolvedColumnNames.push_back(
                    scope.size() > 1 ? entry.alias + "." + col.name : col.name);
            }
        }
    } else {
        for (auto& colName : stmt->columns) {
            std::string tableQ, column;
            size_t dot = colName.find('.');
            if (dot != std::string::npos) {
                tableQ = colName.substr(0, dot);
                column = colName.substr(dot + 1);
            } else {
                column = colName;
            }

            int globalIndex;
            const ColumnSchema* col = resolveColumn(scope, tableQ, column, stmt->line, globalIndex);
            stmt->resolvedColumnIndices.push_back(globalIndex);
            stmt->resolvedColumnNames.push_back(
                tableQ.empty() && scope.size() > 1 ? colName : col->name);
        }
    }

    if (stmt->whereClause) {
        Type whereType = resolveExpr(stmt->whereClause.get(), scope);
        if (whereType != Type::BOOL)
            error("WHERE clause must evaluate to BOOL, got " + typeToString(whereType),
                  stmt->whereClause->line);
    }
}

// ---------------- column resolution ----------------

const ColumnSchema* SemanticAnalyzer::resolveColumn(const Scope& scope, const std::string& tableQualifier,
                                                     const std::string& columnName, int line, int& outRowIndex) {
    if (!tableQualifier.empty()) {
        for (auto& entry : scope) {
            if (entry.alias == tableQualifier) {
                const ColumnSchema* col = entry.schema->findColumn(columnName);
                if (!col) error("Column '" + columnName + "' not found in table '" + tableQualifier + "'", line);
                outRowIndex = entry.rowOffset + col->index;
                return col;
            }
        }
        error("Table '" + tableQualifier + "' not found in FROM clause", line);
    }

    const ColumnSchema* found = nullptr;
    int foundRowIndex = -1;
    for (auto& entry : scope) {
        const ColumnSchema* col = entry.schema->findColumn(columnName);
        if (col) {
            if (found) error("Column '" + columnName + "' is ambiguous", line);
            found = col;
            foundRowIndex = entry.rowOffset + col->index;
        }
    }
    if (!found) error("Column '" + columnName + "' not found", line);
    outRowIndex = foundRowIndex;
    return found;
}

// ---------------- expression type-checking ----------------
// Post-order walk: resolve children first, then decide this node's type
// from the operator + children's types. Every branch also writes the
// result into expr->resolvedType (and, for columns, resolvedIndex) so
// the executor reads pre-resolved info instead of re-deriving it.

Type SemanticAnalyzer::resolveExpr(Expr* expr, const Scope& scope) {
    if (auto* lit = dynamic_cast<LiteralExpr*>(expr)) {
        switch (lit->kind) {
            case LiteralExpr::Kind::NUMBER:
                // No decimal point -> INT, otherwise FLOAT. This is the
                // "permissive" coercion call from the design discussion:
                // literals get a concrete type here, and INT/FLOAT are
                // both treated as the same "numeric" family everywhere else.
                lit->resolvedType = (lit->value.find('.') != std::string::npos) ? Type::FLOAT : Type::INT;
                break;
            case LiteralExpr::Kind::STRING:
                lit->resolvedType = Type::TEXT;
                break;
            case LiteralExpr::Kind::BOOL:
                lit->resolvedType = Type::BOOL;
                break;
            case LiteralExpr::Kind::NUL:
                lit->resolvedType = Type::NUL;
                break;
        }
        return lit->resolvedType;
    }

    if (auto* col = dynamic_cast<ColumnRefExpr*>(expr)) {
        int rowIndex;
        const ColumnSchema* schema = resolveColumn(scope, col->table, col->column, col->line, rowIndex);
        col->resolvedIndex = rowIndex;
        col->resolvedType = schema->type;
        return col->resolvedType;
    }

    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        Type operandType = resolveExpr(un->operand.get(), scope);
        if (un->op == "IS_NULL" || un->op == "IS_NOT_NULL") {
            // Any operand type is legal here — that's the whole point of
            // IS NULL: it's the one check that's well-defined no matter
            // what the operand's type or value is.
            un->resolvedType = Type::BOOL;
        } else if (un->op == "NOT") {
            if (operandType != Type::BOOL)
                error("NOT requires a boolean operand, got " + typeToString(operandType), un->line);
            un->resolvedType = Type::BOOL;
        } else { // unary '-'
            if (!isNumeric(operandType))
                error("Unary '-' requires a numeric operand, got " + typeToString(operandType), un->line);
            un->resolvedType = operandType;
        }
        return un->resolvedType;
    }

    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        Type leftType = resolveExpr(bin->left.get(), scope);
        Type rightType = resolveExpr(bin->right.get(), scope);
        const std::string& op = bin->op;

        if (op == "AND" || op == "OR") {
            if (leftType != Type::BOOL || rightType != Type::BOOL)
                error("'" + op + "' requires boolean operands, got " +
                      typeToString(leftType) + " and " + typeToString(rightType), bin->line);
            bin->resolvedType = Type::BOOL;
        } else if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            // Comparing against NULL is always legal at the TYPE level —
            // it's just never USEFUL, because it always evaluates to
            // UNKNOWN at runtime (see evalExpr). That's exactly why SQL
            // gives you IS NULL as a separate, always-decidable check.
            bool eitherNull = (leftType == Type::NUL || rightType == Type::NUL);
            bool bothNumeric = isNumeric(leftType) && isNumeric(rightType);
            bool sameType = leftType == rightType;
            if (!eitherNull && !bothNumeric && !sameType)
                error("Cannot compare " + typeToString(leftType) + " with " + typeToString(rightType), bin->line);
            bin->resolvedType = Type::BOOL;
        } else { // + - * /
            if (!isNumeric(leftType) || !isNumeric(rightType))
                error("Arithmetic operator '" + op + "' requires numeric operands, got " +
                      typeToString(leftType) + " and " + typeToString(rightType), bin->line);
            bin->resolvedType = (leftType == Type::FLOAT || rightType == Type::FLOAT) ? Type::FLOAT : Type::INT;
        }
        return bin->resolvedType;
    }

    error("Unknown expression node", expr->line);
}
