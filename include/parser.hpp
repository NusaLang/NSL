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
    // End of a statement: `;` is optional -- a line break, `}` or end of file also ends it.
    void expectEnd(const std::string& message);

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
    StmtPtr forInStmt(Span start);  // Python-style `for x in <iterable>`
    // Loop over `iter` binding `vars` (several = unpacking each element); shared with comprehensions.
    StmtPtr buildForIn(const std::vector<std::string>& vars, ExprPtr iter, std::unique_ptr<BlockStmt> body, Span sp);
    std::vector<std::string> forTargets();
    ExprPtr comprehension(ExprPtr element, ExprPtr valueOrNull, bool isDict, Span sp);
    ExprPtr power();
    StmtPtr importStmt();           // `import a.b [as c]`
    StmtPtr fromImportStmt();       // `from a.b import x [as y], ...`
    bool isWord(const Token& t, const char* a, const char* b = nullptr) const;
    bool matchWord(const char* a, const char* b = nullptr);
    int hiddenCounter_ = 0;
    // Extra statements a single source statement expands into (`from m import a, b`);
    // parse() and block() splice them in right after the statement that made them.
    std::vector<StmtPtr> pendingStmts_;
    StmtPtr returnStmt();
    StmtPtr breakStmt();
    StmtPtr continueStmt();
    std::unique_ptr<BlockStmt> block();
    StmtPtr exprStmt();
    StmtPtr tupleAssign(ExprPtr first, Span sp);
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
