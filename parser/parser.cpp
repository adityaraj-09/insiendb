#include "parser.h"

Parser::Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}

// ---------------- token stream helpers ----------------

const Token& Parser::peek(int offset) const {
    size_t p = pos + offset;
    if (p >= tokens.size()) return tokens.back(); // EOF
    return tokens[p];
}

const Token& Parser::previous() const { return tokens[pos - 1]; }

bool Parser::isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }

bool Parser::check(TokenType type) const {
    if (isAtEnd() && type != TokenType::END_OF_FILE) return false;
    return peek().type == type;
}

const Token& Parser::advance() {
    if (!isAtEnd()) pos++;
    return previous();
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (auto t : types) {
        if (check(t)) { advance(); return true; }
    }
    return false;
}

const Token& Parser::expect(TokenType type, const std::string& errMsg) {
    if (check(type)) return advance();
    error(errMsg + " (got '" + peek().lexeme + "')");
}

void Parser::error(const std::string& msg) const {
    throw ParseError(msg, peek().line);
}

// ---------------- entry point ----------------

std::vector<StmtPtr> Parser::parseProgram() {
    std::vector<StmtPtr> statements;
    while (!isAtEnd()) {
        statements.push_back(parseStatement());
    }
    return statements;
}

StmtPtr Parser::parseStatement() {
    if (check(TokenType::CREATE)) {
        if (peek(1).type == TokenType::INDEX)
            return parseCreateIndex();
        return parseCreateTable();
    }
    if (check(TokenType::INSERT)) return parseInsert();
    if (check(TokenType::UPDATE)) return parseUpdate();
    if (check(TokenType::DELETE)) return parseDelete();
    if (check(TokenType::SELECT)) return parseSelect();
    error("Expected a statement (CREATE, INSERT, UPDATE, DELETE, or SELECT)");
}

// ---------------- CREATE TABLE ----------------

StmtPtr Parser::parseCreateTable() {
    int startLine = peek().line;
    expect(TokenType::CREATE, "Expected CREATE");
    expect(TokenType::TABLE, "Expected TABLE after CREATE");

    auto stmt = std::make_unique<CreateTableStmt>();
    stmt->line = startLine;
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;

    expect(TokenType::LPAREN, "Expected '(' after table name");
    do {
        ColumnDef col;
        col.name = expect(TokenType::IDENTIFIER, "Expected column name").lexeme;
        col.type = expect(TokenType::IDENTIFIER, "Expected column type").lexeme;
        stmt->columns.push_back(std::move(col));
    } while (match({TokenType::COMMA}));
    expect(TokenType::RPAREN, "Expected ')' after column list");
    expect(TokenType::SEMICOLON, "Expected ';' after statement");

    return stmt;
}

// ---------------- CREATE INDEX ----------------

StmtPtr Parser::parseCreateIndex() {
    int startLine = peek().line;
    expect(TokenType::CREATE, "Expected CREATE");
    expect(TokenType::INDEX, "Expected INDEX after CREATE");

    auto stmt = std::make_unique<CreateIndexStmt>();
    stmt->line = startLine;
    stmt->indexName = expect(TokenType::IDENTIFIER, "Expected index name").lexeme;
    expect(TokenType::ON, "Expected ON after index name");
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;
    expect(TokenType::LPAREN, "Expected '(' before column name");
    stmt->columnName = expect(TokenType::IDENTIFIER, "Expected column name").lexeme;
    expect(TokenType::RPAREN, "Expected ')' after column name");
    expect(TokenType::SEMICOLON, "Expected ';' after statement");
    return stmt;
}

// ---------------- INSERT ----------------

StmtPtr Parser::parseInsert() {
    int startLine = peek().line;
    expect(TokenType::INSERT, "Expected INSERT");
    expect(TokenType::INTO, "Expected INTO after INSERT");

    auto stmt = std::make_unique<InsertStmt>();
    stmt->line = startLine;
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;

    // optional explicit column list: INSERT INTO t (a, b) VALUES (...)
    if (match({TokenType::LPAREN})) {
        do {
            stmt->columns.push_back(expect(TokenType::IDENTIFIER, "Expected column name").lexeme);
        } while (match({TokenType::COMMA}));
        expect(TokenType::RPAREN, "Expected ')' after column list");
    }

    expect(TokenType::VALUES, "Expected VALUES");
    expect(TokenType::LPAREN, "Expected '(' after VALUES");
    do {
        stmt->values.push_back(parseExpr());
    } while (match({TokenType::COMMA}));
    expect(TokenType::RPAREN, "Expected ')' after value list");
    expect(TokenType::SEMICOLON, "Expected ';' after statement");

    return stmt;
}

// ---------------- UPDATE ----------------

StmtPtr Parser::parseUpdate() {
    int startLine = peek().line;
    expect(TokenType::UPDATE, "Expected UPDATE");

    auto stmt = std::make_unique<UpdateStmt>();
    stmt->line = startLine;
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;

    expect(TokenType::SET, "Expected SET after table name");
    do {
        UpdateAssignment assign;
        assign.column = expect(TokenType::IDENTIFIER, "Expected column name").lexeme;
        expect(TokenType::EQ, "Expected '=' after column name in SET");
        assign.value = parseExpr();
        stmt->assignments.push_back(std::move(assign));
    } while (match({TokenType::COMMA}));

    if (match({TokenType::WHERE})) {
        stmt->whereClause = parseExpr();
    }

    expect(TokenType::SEMICOLON, "Expected ';' after statement");
    return stmt;
}

// ---------------- DELETE ----------------

StmtPtr Parser::parseDelete() {
    int startLine = peek().line;
    expect(TokenType::DELETE, "Expected DELETE");
    expect(TokenType::FROM, "Expected FROM after DELETE");

    auto stmt = std::make_unique<DeleteStmt>();
    stmt->line = startLine;
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;

    if (match({TokenType::WHERE})) {
        stmt->whereClause = parseExpr();
    }

    expect(TokenType::SEMICOLON, "Expected ';' after statement");
    return stmt;
}

// ---------------- SELECT ----------------

std::string Parser::parseQualifiedName() {
    std::string first = expect(TokenType::IDENTIFIER, "Expected column name").lexeme;
    if (match({TokenType::DOT})) {
        return first + "." + expect(TokenType::IDENTIFIER, "Expected column name after '.'").lexeme;
    }
    return first;
}

StmtPtr Parser::parseSelect() {
    int startLine = peek().line;
    expect(TokenType::SELECT, "Expected SELECT");

    auto stmt = std::make_unique<SelectStmt>();
    stmt->line = startLine;

    if (match({TokenType::STAR})) {
        stmt->columns.push_back("*");
    } else {
        do {
            stmt->columns.push_back(parseQualifiedName());
        } while (match({TokenType::COMMA}));
    }

    expect(TokenType::FROM, "Expected FROM");
    stmt->tableName = expect(TokenType::IDENTIFIER, "Expected table name").lexeme;

    while (match({TokenType::JOIN})) {
        JoinClause join;
        join.tableName = expect(TokenType::IDENTIFIER, "Expected table name after JOIN").lexeme;
        expect(TokenType::ON, "Expected ON after JOIN table");
        join.onCondition = parseExpr();
        stmt->joins.push_back(std::move(join));
    }

    if (match({TokenType::WHERE})) {
        stmt->whereClause = parseExpr();
    }

    expect(TokenType::SEMICOLON, "Expected ';' after statement");
    return stmt;
}

// ---------------- expressions ----------------
// Each function below handles one precedence level and delegates to the
// next-tighter level for its operands. This is "precedence climbing"
// written out longhand instead of as a loop over a precedence table —
// easier to read when you're learning it for the first time.

ExprPtr Parser::parseExpr() { return parseOr(); }

ExprPtr Parser::parseOr() {
    ExprPtr expr = parseAnd();
    while (match({TokenType::OR})) {
        int opLine = previous().line;
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = "OR";
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseAnd();
        expr = std::move(bin);
    }
    return expr;
}

ExprPtr Parser::parseAnd() {
    ExprPtr expr = parseEquality();
    while (match({TokenType::AND})) {
        int opLine = previous().line;
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = "AND";
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseEquality();
        expr = std::move(bin);
    }
    return expr;
}

ExprPtr Parser::parseEquality() {
    ExprPtr expr = parseIsNull();
    while (match({TokenType::EQ, TokenType::NEQ})) {
        int opLine = previous().line;
        std::string op = previous().type == TokenType::EQ ? "=" : "!=";
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseIsNull();
        expr = std::move(bin);
    }
    return expr;
}

// "IS NULL" / "IS NOT NULL" is a postfix check, not a binary comparison —
// it never itself produces UNKNOWN, which is exactly why it's the tool SQL
// gives you for testing NULL-ness (unlike "= NULL", which always evaluates
// to UNKNOWN and therefore never matches anything in a WHERE clause).
ExprPtr Parser::parseIsNull() {
    ExprPtr expr = parseComparison();
    if (match({TokenType::IS})) {
        int opLine = previous().line;
        bool negate = match({TokenType::NOT});
        expect(TokenType::NULL_LIT, "Expected NULL after IS" + std::string(negate ? " NOT" : ""));
        auto un = std::make_unique<UnaryExpr>();
        un->op = negate ? "IS_NOT_NULL" : "IS_NULL";
        un->line = opLine;
        un->operand = std::move(expr);
        expr = std::move(un);
    }
    return expr;
}

ExprPtr Parser::parseComparison() {
    ExprPtr expr = parseTerm();
    while (match({TokenType::LT, TokenType::LTE, TokenType::GT, TokenType::GTE})) {
        int opLine = previous().line;
        TokenType t = previous().type;
        std::string op = t == TokenType::LT ? "<" : t == TokenType::LTE ? "<=" :
                          t == TokenType::GT ? ">" : ">=";
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseTerm();
        expr = std::move(bin);
    }
    return expr;
}

ExprPtr Parser::parseTerm() {
    ExprPtr expr = parseFactor();
    while (match({TokenType::PLUS, TokenType::MINUS})) {
        int opLine = previous().line;
        std::string op = previous().type == TokenType::PLUS ? "+" : "-";
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseFactor();
        expr = std::move(bin);
    }
    return expr;
}

ExprPtr Parser::parseFactor() {
    ExprPtr expr = parseUnary();
    while (match({TokenType::STAR, TokenType::SLASH})) {
        int opLine = previous().line;
        std::string op = previous().type == TokenType::STAR ? "*" : "/";
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = op;
        bin->line = opLine;
        bin->left = std::move(expr);
        bin->right = parseUnary();
        expr = std::move(bin);
    }
    return expr;
}

ExprPtr Parser::parseUnary() {
    if (match({TokenType::NOT, TokenType::MINUS})) {
        int opLine = previous().line;
        std::string op = previous().type == TokenType::NOT ? "NOT" : "-";
        auto un = std::make_unique<UnaryExpr>();
        un->op = op;
        un->line = opLine;
        un->operand = parseUnary();
        return un;
    }
    return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
    if (match({TokenType::NUMBER})) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralExpr::Kind::NUMBER;
        lit->value = previous().lexeme;
        lit->line = previous().line;
        return lit;
    }
    if (match({TokenType::STRING})) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralExpr::Kind::STRING;
        lit->value = previous().lexeme;
        lit->line = previous().line;
        return lit;
    }
    if (match({TokenType::TRUE_LIT, TokenType::FALSE_LIT})) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralExpr::Kind::BOOL;
        lit->value = previous().type == TokenType::TRUE_LIT ? "true" : "false";
        lit->line = previous().line;
        return lit;
    }
    if (match({TokenType::NULL_LIT})) {
        auto lit = std::make_unique<LiteralExpr>();
        lit->kind = LiteralExpr::Kind::NUL;
        lit->value = "null";
        lit->line = previous().line;
        return lit;
    }
    if (match({TokenType::IDENTIFIER})) {
        int idLine = previous().line;
        auto ref = std::make_unique<ColumnRefExpr>();
        std::string first = previous().lexeme;
        if (match({TokenType::DOT})) {
            ref->table = first;
            ref->column = expect(TokenType::IDENTIFIER, "Expected column name after '.'").lexeme;
        } else {
            ref->column = first;
        }
        ref->line = idLine;
        return ref;
    }
    if (match({TokenType::LPAREN})) {
        ExprPtr expr = parseExpr();
        expect(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }

    error("Expected an expression");
}
