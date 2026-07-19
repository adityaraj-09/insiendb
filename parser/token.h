#pragma once
#include <string>

// Every distinct "kind of thing" the lexer can produce.
// Keeping keywords as their own enum values (rather than just IDENTIFIER)
// means the parser can switch on token.type instead of doing string
// comparisons everywhere — cheaper and less error-prone.
enum class TokenType {
    // literals & identifiers
    IDENTIFIER, NUMBER, STRING,

    // keywords
    SELECT, FROM, WHERE, INSERT, INTO, VALUES, CREATE, TABLE, INDEX,
    UPDATE, DELETE, JOIN, ON, SET,
    AND, OR, NOT, NULL_LIT, TRUE_LIT, FALSE_LIT, IS,

    // operators
    EQ, NEQ, LT, LTE, GT, GTE,
    PLUS, MINUS, STAR, SLASH,

    // punctuation
    LPAREN, RPAREN, COMMA, SEMICOLON, DOT,

    END_OF_FILE, ILLEGAL
};

struct Token {
    TokenType type;
    std::string lexeme;  // the raw text, e.g. "42", "users", "="
    int line;
};

std::string tokenTypeToString(TokenType t);
