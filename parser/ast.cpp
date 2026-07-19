#include "ast.h"

std::string LiteralExpr::toString() const {
    switch (kind) {
        case Kind::STRING: return "\"" + value + "\"";
        default: return value;
    }
}

std::string ColumnRefExpr::toString() const {
    return table.empty() ? column : (table + "." + column);
}

std::string BinaryExpr::toString() const {
    return "(" + op + " " + left->toString() + " " + right->toString() + ")";
}

std::string UnaryExpr::toString() const {
    return "(" + op + " " + operand->toString() + ")";
}

std::string CreateTableStmt::toString() const {
    std::string out = "(CREATE_TABLE " + tableName + " [";
    for (size_t i = 0; i < columns.size(); i++) {
        if (i) out += ", ";
        out += columns[i].name + ":" + columns[i].type;
    }
    out += "])";
    return out;
}

std::string CreateIndexStmt::toString() const {
    return "(CREATE_INDEX " + indexName + " ON " + tableName + " (" + columnName + "))";
}

std::string InsertStmt::toString() const {
    std::string out = "(INSERT " + tableName;
    if (!columns.empty()) {
        out += " (";
        for (size_t i = 0; i < columns.size(); i++) {
            if (i) out += ", ";
            out += columns[i];
        }
        out += ")";
    }
    out += " VALUES [";
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ", ";
        out += values[i]->toString();
    }
    out += "])";
    return out;
}

std::string UpdateStmt::toString() const {
    std::string out = "(UPDATE " + tableName + " SET [";
    for (size_t i = 0; i < assignments.size(); i++) {
        if (i) out += ", ";
        out += assignments[i].column + "=" + assignments[i].value->toString();
    }
    out += "]";
    if (whereClause) out += " WHERE " + whereClause->toString();
    out += ")";
    return out;
}

std::string DeleteStmt::toString() const {
    std::string out = "(DELETE FROM " + tableName;
    if (whereClause) out += " WHERE " + whereClause->toString();
    out += ")";
    return out;
}

std::string SelectStmt::toString() const {
    std::string out = "(SELECT [";
    for (size_t i = 0; i < columns.size(); i++) {
        if (i) out += ", ";
        out += columns[i];
    }
    out += "] FROM " + tableName;
    for (auto& join : joins) {
        out += " JOIN " + join.tableName + " ON " + join.onCondition->toString();
    }
    if (whereClause) out += " WHERE " + whereClause->toString();
    out += ")";
    return out;
}
