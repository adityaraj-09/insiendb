#pragma once
#include "value.h"
#include <string>
#include <vector>
#include <memory>

// ============ EXPRESSIONS ============
// Anything that evaluates to a value: literals, column refs, `a = 5`, `x AND y`.
// This is a classic tagged tree via virtual dispatch — later, your executor
// will do the same trick (virtual eval()) to actually compute values.

struct Expr {
    int line = 0;                          // set by the parser, used in error messages
    Type resolvedType = Type::UNKNOWN;     // filled in by SemanticAnalyzer, unused until then
    virtual ~Expr() = default;
    virtual std::string toString() const = 0; // s-expr style, for debugging
};
using ExprPtr = std::unique_ptr<Expr>;

struct LiteralExpr : Expr {
    enum class Kind { NUMBER, STRING, BOOL, NUL };
    Kind kind;
    std::string value; // raw text; a real engine would store a typed Value here
    std::string toString() const override;
};

struct ColumnRefExpr : Expr {
    std::string table;   // optional qualifier, e.g. "users" in users.id — empty if unqualified
    std::string column;
    int resolvedIndex = -1; // filled in by SemanticAnalyzer: position within the row
    std::string toString() const override;
};

struct BinaryExpr : Expr {
    std::string op;     // "=", "AND", "+", etc.
    ExprPtr left;
    ExprPtr right;
    std::string toString() const override;
};

struct UnaryExpr : Expr {
    std::string op;      // "NOT", "-"
    ExprPtr operand;
    std::string toString() const override;
};

// ============ STATEMENTS ============
// A statement is a full command: CREATE TABLE, INSERT, SELECT.

struct Statement {
    int line = 0; // set by the parser
    virtual ~Statement() = default;
    virtual std::string toString() const = 0;
};
using StmtPtr = std::unique_ptr<Statement>;

struct ColumnDef {
    std::string name;
    std::string type; // kept as raw text (INT, TEXT, VARCHAR...) — no type-checking yet
};

struct CreateTableStmt : Statement {
    std::string tableName;
    std::vector<ColumnDef> columns;
    std::string toString() const override;
};

struct CreateIndexStmt : Statement {
    std::string indexName;
    std::string tableName;
    std::string columnName;
    int resolvedColumnIndex = -1;
    std::string toString() const override;
};

struct InsertStmt : Statement {
    std::string tableName;
    std::vector<std::string> columns; // may be empty -> means "all columns, in order"
    std::vector<ExprPtr> values;
    std::vector<int> resolvedColumnIndices; // filled by SemanticAnalyzer: values[i] -> row[resolvedColumnIndices[i]]
    std::string toString() const override;
};

struct JoinClause {
    std::string tableName;
    ExprPtr onCondition;
};

struct UpdateAssignment {
    std::string column;
    ExprPtr value;
    int resolvedColumnIndex = -1; // filled by SemanticAnalyzer
};

struct UpdateStmt : Statement {
    std::string tableName;
    std::vector<UpdateAssignment> assignments;
    ExprPtr whereClause; // nullptr if no WHERE
    std::string toString() const override;
};

struct DeleteStmt : Statement {
    std::string tableName;
    ExprPtr whereClause; // nullptr if no WHERE
    std::string toString() const override;
};

struct SelectStmt : Statement {
    std::vector<std::string> columns; // {"*"} or explicit list (may be "table.col")
    std::string tableName;
    std::vector<JoinClause> joins;
    ExprPtr whereClause; // nullptr if no WHERE
    std::vector<int> resolvedColumnIndices; // position in the (possibly joined) composite row
    std::vector<std::string> resolvedColumnNames; // filled by SemanticAnalyzer for result headers
    std::string toString() const override;
};
