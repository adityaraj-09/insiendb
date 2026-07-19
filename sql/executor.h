#pragma once
#include "ast.h"
#include "catalog.h"
#include "storage.h"
#include <vector>
#include <string>

// The result of a SELECT: column names (for printing a header) plus the
// matching rows. INSERT/CREATE TABLE don't produce a QueryResult — they
// just mutate Storage/Catalog and return nothing.
struct QueryResult {
    std::vector<std::string> columnNames;
    std::vector<Row> rows;
};

// Runs an AST that has ALREADY been through SemanticAnalyzer — it trusts
// resolvedType/resolvedIndex/resolvedColumnIndices completely and does
// zero name lookups of its own. This split (analyze once, execute many
// times trusting the annotations) is exactly how real query engines
// separate "planning" from "execution".
class Executor {
public:
    Executor(Catalog& catalog, Storage& storage);

    void executeCreateTable(CreateTableStmt* stmt);
    void executeCreateIndex(CreateIndexStmt* stmt);
    void executeInsert(InsertStmt* stmt);
    int executeUpdate(UpdateStmt* stmt);
    int executeDelete(DeleteStmt* stmt);
    QueryResult executeSelect(SelectStmt* stmt);

private:
    Catalog& catalog;
    Storage& storage;
};

// Evaluates an already-analyzed expression tree against one row, producing
// an actual Value. Column references pull straight from row[resolvedIndex] —
// no name comparison happens anywhere in this function.
Value evalExpr(const Expr* expr, const Row& row);
