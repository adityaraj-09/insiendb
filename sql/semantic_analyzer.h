#pragma once
#include "ast.h"
#include "catalog.h"
#include "semantic_error.h"
#include <vector>
#include <string>

// One entry per table visible while analyzing the current statement.
// This is a *vector*, not a single TableSchema, even though today only
// one table is ever in scope — the day you add JOINs, scope grows to
// multiple entries and unqualified column lookup has to search all of
// them (and detect ambiguity). Building the abstraction now avoids a
// rewrite later.
struct ScopeEntry {
    std::string alias;       // table name for now; would hold "AS" aliases later
    const TableSchema* schema;
    int rowOffset = 0;       // where this table's columns start in a joined composite row
};
using Scope = std::vector<ScopeEntry>;

// Walks a parsed AST and checks it against the Catalog: do the referenced
// tables/columns exist, do the types agree, is the WHERE clause actually
// boolean. On success, it also *annotates* the AST in place — every Expr's
// resolvedType gets filled in, and every ColumnRefExpr's resolvedIndex —
// so the executor never has to re-derive that information per row.
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(Catalog& catalog);

    // Throws SemanticError on the first problem found.
    void analyze(Statement* stmt);

private:
    Catalog& catalog;

    void analyzeCreateTable(CreateTableStmt* stmt);
    void analyzeCreateIndex(CreateIndexStmt* stmt);
    void analyzeInsert(InsertStmt* stmt);
    void analyzeUpdate(UpdateStmt* stmt);
    void analyzeDelete(DeleteStmt* stmt);
    void analyzeSelect(SelectStmt* stmt);

    // Builds a scope entry for one table and advances rowOffset for the next.
    void addTableToScope(Scope& scope, int& rowOffset, const std::string& tableName, int line);

    // Post-order walk: resolves every node bottom-up, filling in
    // resolvedType, and returns the type of the expression as a whole.
    Type resolveExpr(Expr* expr, const Scope& scope);

    // Column name -> ColumnSchema*, given an optional "table." qualifier.
    // Also writes the column's index within a composite (joined) row.
    const ColumnSchema* resolveColumn(const Scope& scope, const std::string& tableQualifier,
                                       const std::string& columnName, int line, int& outRowIndex);

    [[noreturn]] void error(const std::string& msg, int line);
};
