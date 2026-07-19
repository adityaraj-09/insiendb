#pragma once
#include "token.h"
#include <string>
#include <vector>


class Lexer{

    public:
     Lexer(const std::string& source);
     std::vector<Token> tokens;
     std::vector<Token> tokenize();

    private:
    const std::string& src;
    size_t pos=0;
    int line=1;

    char peek(int offset = 0) const;

    char advance();                    // consume and return current char
    bool isAtEnd() const;
 
    void skipWhitespaceAndComments();  // spaces, tabs, newlines, "-- comments"
 
    Token makeToken(TokenType type, const std::string& lexeme);
    Token identifierOrKeyword();       // handles both `users` and `SELECT`
    Token number();                    // 123 or 3.14
    Token stringLiteral();             // 'hello world'
    Token errorToken(const std::string& msg);
    


};