#pragma once
#include "token.h"
#include "ast.h"
#include <vector>
#include <stdexcept>

// A parse error carries the line number so the caller can point the user
// at exactly where their SQL broke.
struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& msg, int line)
        : std::runtime_error(msg), line(line) {}
};

// Recursive-descent parser. Each grammar rule below has a matching method.
// Grammar (EBNF):
//
//   statement    := createStmt | insertStmt | updateStmt | deleteStmt | selectStmt
//   createStmt   := "CREATE" "TABLE" IDENT "(" columnDef ("," columnDef)* ")" ";"
//   columnDef    := IDENT IDENT
//   insertStmt   := "INSERT" "INTO" IDENT ["(" IDENT ("," IDENT)* ")"]
//                   "VALUES" "(" expr ("," expr)* ")" ";"
//   updateStmt   := "UPDATE" IDENT "SET" IDENT "=" expr ("," IDENT "=" expr)* ["WHERE" expr] ";"
//   deleteStmt   := "DELETE" "FROM" IDENT ["WHERE" expr] ";"
//   selectStmt   := "SELECT" selectList "FROM" IDENT (joinClause)* ["WHERE" expr] ";"
//   joinClause   := "JOIN" IDENT "ON" expr
//   selectList   := "*" | qualifiedName ("," qualifiedName)*
//   qualifiedName:= IDENT ["." IDENT]
//
//   expr         := orExpr
//   orExpr       := andExpr ("OR" andExpr)*
//   andExpr      := equality ("AND" equality)*
//   equality     := isNullExpr (("=" | "!=") isNullExpr)*
//   isNullExpr   := comparison ["IS" ["NOT"] "NULL"]
//   comparison   := term ((">" | ">=" | "<" | "<=") term)*
//   term         := factor (("+" | "-") factor)*
//   factor       := unary (("*" | "/") unary)*
//   unary        := ("NOT" | "-") unary | primary
//   primary      := NUMBER | STRING | TRUE | FALSE | NULL
//                  | IDENT ["." IDENT] | "(" expr ")"
//
// Each level in the expression grammar encodes precedence: OR binds loosest,
// unary/primary bind tightest. That's what lets `a = 1 AND b = 2 OR c = 3`
// parse into the right tree without an explicit precedence table.
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parses one or more semicolon-terminated statements until EOF.
    std::vector<StmtPtr> parseProgram();

private:
    std::vector<Token> tokens;
    size_t pos = 0;

    // --- token stream helpers ---
    const Token& peek(int offset = 0) const;
    const Token& previous() const;
    bool check(TokenType type) const;
    bool isAtEnd() const;
    const Token& advance();
    bool match(std::initializer_list<TokenType> types);
    const Token& expect(TokenType type, const std::string& errMsg);
    [[noreturn]] void error(const std::string& msg) const;

    // --- statements ---
    StmtPtr parseStatement();
    StmtPtr parseCreateTable();
    StmtPtr parseCreateIndex();
    StmtPtr parseInsert();
    StmtPtr parseUpdate();
    StmtPtr parseDelete();
    StmtPtr parseSelect();
    std::string parseQualifiedName();

    // --- expressions (precedence climbing via recursive descent) ---
    ExprPtr parseExpr();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseEquality();
    ExprPtr parseIsNull();
    ExprPtr parseComparison();
    ExprPtr parseTerm();
    ExprPtr parseFactor();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();
};
