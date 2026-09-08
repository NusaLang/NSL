#pragma once

#include <string>
#include <vector>
#include <stdexcept>

enum class TokenType {
    Number, String, Ident,
    Let, Fn, If, Else, While, For, Return, Break, Continue, True_, False_, Null_,
    Class, This, Super, Extends,
    Struct, EnumKw, Try, Catch, Finally, Throw,
    Plus, Minus, Star, Slash, Percent,
    Eq, EqEq, Neq, Lt, Lte, Gt, Gte,
    PlusEq, MinusEq, StarEq, SlashEq,
    And, Or, Not,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semi, Dot, Colon,
    Eof
};

// Position tracking for error messages.
struct Span {
    int start = 0;
    int end = 0;
    int line = 1;
    int column = 1;
};

struct Token {
    TokenType type;
    std::string text;    // raw source text (identifiers, operators)
    double number = 0.0; // decoded value when type == Number
    std::string str;     // decoded value when type == String
    Span span{};
};

class LexError : public std::runtime_error {
public:
    LexError(const std::string& msg, int line, int column);
    int line;
    int column;
};

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string source_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    char peek(int offset = 0) const;
    char advance();
    void skipWhitespaceAndComments();

    Token readNumber(size_t start, int line, int col);
    Token readIdent(size_t start, int line, int col);
    Token readString(size_t start, int line, int col);
    Token readSymbol(size_t start, int line, int col);
    // Unlike other read* methods, produces several tokens (a desugared
    // `+`/`ke_teks(...)` chain) -- see lexer.cpp.
    std::vector<Token> readBacktickTemplate(size_t start, int line, int col);
    // One token, except for a template string. Shared by tokenize()'s
    // main loop and readBacktickTemplate()'s ${...} scan (nested
    // templates work via ordinary recursion).
    std::vector<Token> nextTokens();
};
