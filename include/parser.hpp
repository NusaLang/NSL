#pragma once

#include <memory>
#include <stdexcept>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, const Token& token);
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    std::unique_ptr<Program> parse();
    // For REPL use: parse exactly one expression, nothing more.
    ExprPtr parseSingleExpression();

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek() const;
    const Token& peekAt(size_t offset) const;
    bool atEnd() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const std::string& message);

    StmtPtr statement();
    StmtPtr letStmt();
    StmtPtr fnDecl();
    std::unique_ptr<FnDeclStmt> fnDeclBody(std::string name);
    StmtPtr classDecl();
    StmtPtr structDecl();
    StmtPtr enumDecl();
    StmtPtr tryStmt();
    StmtPtr throwStmt();
    StmtPtr ifStmt();
    StmtPtr whileStmt();
    StmtPtr forStmt();
    StmtPtr returnStmt();
    StmtPtr breakStmt();
    StmtPtr continueStmt();
    std::unique_ptr<BlockStmt> block();
    StmtPtr exprStmt();
    // Shared by `untuk (init; ...` and plain statements: a let or bare
    // expression, without consuming the trailing ';' itself.
    StmtPtr forClauseInit();

    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr logicOr();
    ExprPtr logicAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr term();
    ExprPtr factor();
    ExprPtr unary();
    ExprPtr call();
    ExprPtr primary();
    ExprPtr jsxElement();

    // span '>' penutup JSX terakhir yang dikonsumsi (anchor spasi antar anak)
    Span lastJsxCloseSpan_{0, 0, 1, 1};
};
