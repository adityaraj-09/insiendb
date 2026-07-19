#include "lexer.h"
#include <cctype>
#include <unordered_map>

// SQL keywords are case-insensitive by convention (SELECT == select == Select).
// We upper-case the lexeme before this lookup so "select" and "SELECT" both hit.
static const std::unordered_map<std::string, TokenType> keywords = {
    {"SELECT", TokenType::SELECT}, {"FROM", TokenType::FROM},
    {"WHERE", TokenType::WHERE},   {"INSERT", TokenType::INSERT},
    {"INTO", TokenType::INTO},     {"VALUES", TokenType::VALUES},
    {"CREATE", TokenType::CREATE}, {"TABLE", TokenType::TABLE}, {"INDEX", TokenType::INDEX},
    {"UPDATE", TokenType::UPDATE}, {"DELETE", TokenType::DELETE},
    {"JOIN", TokenType::JOIN},     {"ON", TokenType::ON},
    {"SET", TokenType::SET},
    {"AND", TokenType::AND},       {"OR", TokenType::OR},
    {"NOT", TokenType::NOT},       {"NULL", TokenType::NULL_LIT},
    {"TRUE", TokenType::TRUE_LIT}, {"FALSE", TokenType::FALSE_LIT},
    {"IS", TokenType::IS},
};

Lexer::Lexer(const std::string& source) : src(source) {}

bool Lexer::isAtEnd() const { return pos >= src.size(); }

char Lexer::peek(int offset) const {
    size_t p = pos + offset;
    if (p >= src.size()) return '\0';
    return src[p];
}

char Lexer::advance() {
    char c = src[pos++];
    if (c == '\n') line++;
    return c;
}

Token Lexer::makeToken(TokenType type, const std::string& lexeme) {
    return Token{type, lexeme, line};
}

Token Lexer::errorToken(const std::string& msg) {
    return Token{TokenType::ILLEGAL, msg, line};
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && peek(1) == '-') {
            // "-- comment until end of line", same as real SQL
            while (!isAtEnd() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::identifierOrKeyword() {
    size_t start = pos;
    while (!isAtEnd() && (isalnum((unsigned char)peek()) || peek() == '_')) advance();
    std::string text = src.substr(start, pos - start);

    std::string upper = text;
    for (auto& ch : upper) ch = toupper((unsigned char)ch);

    auto it = keywords.find(upper);
    if (it != keywords.end()) return makeToken(it->second, upper);
    return makeToken(TokenType::IDENTIFIER, text);
}

Token Lexer::number() {
    size_t start = pos;
    while (!isAtEnd() && isdigit((unsigned char)peek())) advance();
    if (peek() == '.' && isdigit((unsigned char)peek(1))) {
        advance(); // consume '.'
        while (!isAtEnd() && isdigit((unsigned char)peek())) advance();
    }
    return makeToken(TokenType::NUMBER, src.substr(start, pos - start));
}


Token Lexer::stringLiteral() {
    advance(); // consume opening quote
    size_t start = pos;
    while (!isAtEnd() && peek() != '\'') advance();
    if (isAtEnd()) return errorToken("Unterminated string literal");
    std::string text = src.substr(start, pos - start);
    advance(); // consume closing quote
    return makeToken(TokenType::STRING, text);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();
        if (isAtEnd()) {
            tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
            break;
        }

        char c = peek();

        if (isalpha((unsigned char)c) || c == '_') {
            tokens.push_back(identifierOrKeyword());
            continue;
        }
        if (isdigit((unsigned char)c)) {
            tokens.push_back(number());
            continue;
        }
        if (c == '\'') {
            tokens.push_back(stringLiteral());
            continue;
        }

        // single/double-char operators and punctuation
        advance();
        switch (c) {
            case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(")); break;
            case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")")); break;
            case ',': tokens.push_back(makeToken(TokenType::COMMA, ",")); break;
            case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";")); break;
            case '.': tokens.push_back(makeToken(TokenType::DOT, ".")); break;
            case '+': tokens.push_back(makeToken(TokenType::PLUS, "+")); break;
            case '-': tokens.push_back(makeToken(TokenType::MINUS, "-")); break;
            case '*': tokens.push_back(makeToken(TokenType::STAR, "*")); break;
            case '/': tokens.push_back(makeToken(TokenType::SLASH, "/")); break;
            case '=': tokens.push_back(makeToken(TokenType::EQ, "=")); break;
            case '!':
                if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::NEQ, "!=")); }
                else tokens.push_back(errorToken("Unexpected '!'"));
                break;
            case '<':
                if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::LTE, "<=")); }
                else tokens.push_back(makeToken(TokenType::LT, "<"));
                break;
            case '>':
                if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::GTE, ">=")); }
                else tokens.push_back(makeToken(TokenType::GT, ">"));
                break;
            default:
                tokens.push_back(errorToken(std::string("Unexpected character: ") + c));
        }
    }

    return tokens;
}

std::string tokenTypeToString(TokenType t) {
    switch (t) {
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::NUMBER: return "NUMBER";
        case TokenType::STRING: return "STRING";
        case TokenType::SELECT: return "SELECT";
        case TokenType::FROM: return "FROM";
        case TokenType::WHERE: return "WHERE";
        case TokenType::INSERT: return "INSERT";
        case TokenType::INTO: return "INTO";
        case TokenType::VALUES: return "VALUES";
        case TokenType::CREATE: return "CREATE";
        case TokenType::TABLE: return "TABLE";
        case TokenType::INDEX: return "INDEX";
        case TokenType::UPDATE: return "UPDATE";
        case TokenType::DELETE: return "DELETE";
        case TokenType::JOIN: return "JOIN";
        case TokenType::ON: return "ON";
        case TokenType::SET: return "SET";
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::NULL_LIT: return "NULL";
        case TokenType::TRUE_LIT: return "TRUE";
        case TokenType::FALSE_LIT: return "FALSE";
        case TokenType::IS: return "IS";
        case TokenType::EQ: return "EQ";
        case TokenType::NEQ: return "NEQ";
        case TokenType::LT: return "LT";
        case TokenType::LTE: return "LTE";
        case TokenType::GT: return "GT";
        case TokenType::GTE: return "GTE";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::COMMA: return "COMMA";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::DOT: return "DOT";
        case TokenType::END_OF_FILE: return "EOF";
        case TokenType::ILLEGAL: return "ILLEGAL";
    }
    return "UNKNOWN";
}
