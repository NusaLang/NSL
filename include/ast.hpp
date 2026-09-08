#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lexer.hpp"  // for Span

// Plain data-holding nodes, one struct per grammar production, tagged
// with a Kind enum instead of a visitor.

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind { Literal, Identifier, Unary, Binary, Assign, Call, ArrayLit, Index, IndexAssign, FnExpr };

struct Expr {
    ExprKind kind;
    // Stamped by the parser (see parser.cpp) -- lets a RuntimeError
    // thrown while evaluating this node report "line N, col M" instead
    // of nothing at all. See Interpreter::eval's catch-and-attach.
    Span span{};
    virtual ~Expr() = default;
protected:
    explicit Expr(ExprKind k) : kind(k) {}
};

struct LiteralExpr : Expr {
    enum class Kind { Number, String, Bool, Null } litKind;
    double number = 0.0;
    std::string str;
    bool boolean = false;

    static ExprPtr makeNumber(double n) {
        auto e = std::make_unique<LiteralExpr>();
        e->litKind = Kind::Number;
        e->number = n;
        return e;
    }
    static ExprPtr makeString(std::string s) {
        auto e = std::make_unique<LiteralExpr>();
        e->litKind = Kind::String;
        e->str = std::move(s);
        return e;
    }
    static ExprPtr makeBool(bool b) {
        auto e = std::make_unique<LiteralExpr>();
        e->litKind = Kind::Bool;
        e->boolean = b;
        return e;
    }
    static ExprPtr makeNull() {
        auto e = std::make_unique<LiteralExpr>();
        e->litKind = Kind::Null;
        return e;
    }
    LiteralExpr() : Expr(ExprKind::Literal) {}
};

struct IdentifierExpr : Expr {
    std::string name;
    explicit IdentifierExpr(std::string n) : Expr(ExprKind::Identifier), name(std::move(n)) {}
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr operand;
    UnaryExpr(std::string o, ExprPtr e)
        : Expr(ExprKind::Unary), op(std::move(o)), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    std::string op;
    ExprPtr left, right;
    BinaryExpr(std::string o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Binary), op(std::move(o)), left(std::move(l)), right(std::move(r)) {}
};

struct AssignExpr : Expr {
    std::string name;
    ExprPtr value;
    AssignExpr(std::string n, ExprPtr v)
        : Expr(ExprKind::Assign), name(std::move(n)), value(std::move(v)) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a)
        : Expr(ExprKind::Call), callee(std::move(c)), args(std::move(a)) {}
};

// `[1, 2, 3]`
struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elements;
    explicit ArrayLitExpr(std::vector<ExprPtr> e)
        : Expr(ExprKind::ArrayLit), elements(std::move(e)) {}
};

// `target[index]` as an rvalue (read).
struct IndexExpr : Expr {
    ExprPtr target;
    ExprPtr index;
    IndexExpr(ExprPtr t, ExprPtr i)
        : Expr(ExprKind::Index), target(std::move(t)), index(std::move(i)) {}
};

// `target[index] = value` (write). Kept as its own node instead of
// generalizing AssignExpr's target, so plain identifier assignment stays
// a simple string-keyed env write with no extra indirection.
struct IndexAssignExpr : Expr {
    ExprPtr target;
    ExprPtr index;
    ExprPtr value;
    IndexAssignExpr(ExprPtr t, ExprPtr i, ExprPtr v)
        : Expr(ExprKind::IndexAssign), target(std::move(t)), index(std::move(i)), value(std::move(v)) {}
};

struct FnDeclStmt;

struct FnExprNode : Expr {
    std::unique_ptr<FnDeclStmt> decl;
    explicit FnExprNode(std::unique_ptr<FnDeclStmt> d) : Expr(ExprKind::FnExpr), decl(std::move(d)) {}
};

// ---- statements ----

enum class StmtKind {
    Let, FnDecl, ClassDecl, Block, If, While, For, Return, Break, Continue, ExprStmt,
    StructDecl, EnumDecl, Try, Throw
};

struct Stmt {
    StmtKind kind;
    // Stamped uniformly by Parser::statement() -- see Expr::span.
    Span span{};
    virtual ~Stmt() = default;
protected:
    explicit Stmt(StmtKind k) : kind(k) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> s)
        : Stmt(StmtKind::Block), statements(std::move(s)) {}
};

struct LetStmt : Stmt {
    std::string name;
    ExprPtr value;
    // Optional `: <type>` annotation, e.g. `buat x: angka = 5;`. Empty
    // string means "no annotation" -- purely gradual/optional, checked
    // by TypeChecker (typechecker.hpp), completely ignored at runtime.
    std::string typeAnnotation;
    LetStmt(std::string n, ExprPtr v, std::string t = "")
        : Stmt(StmtKind::Let), name(std::move(n)), value(std::move(v)), typeAnnotation(std::move(t)) {}
};

struct FnDeclStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    // Parallel to `params`: optional per-parameter `: <type>` annotation
    // (empty = unannotated). Same for `returnType` (empty = unannotated).
    std::vector<std::string> paramTypes;
    std::string returnType;
    std::unique_ptr<BlockStmt> body;
    FnDeclStmt(std::string n, std::vector<std::string> p, std::unique_ptr<BlockStmt> b,
               std::vector<std::string> pt = {}, std::string rt = "")
        : Stmt(StmtKind::FnDecl), name(std::move(n)), params(std::move(p)),
          paramTypes(std::move(pt)), returnType(std::move(rt)), body(std::move(b)) {
        if (paramTypes.empty()) paramTypes.assign(params.size(), "");
    }
};

struct ClassDeclStmt : Stmt {
    std::string name;
    std::string parentName;
    std::vector<std::unique_ptr<FnDeclStmt>> methods;
    ClassDeclStmt(std::string n, std::string p, std::vector<std::unique_ptr<FnDeclStmt>> m)
        : Stmt(StmtKind::ClassDecl), name(std::move(n)), parentName(std::move(p)), methods(std::move(m)) {}
};

struct StructDeclStmt : Stmt {
    std::string name;
    std::vector<std::string> fields;
    StructDeclStmt(std::string n, std::vector<std::string> f)
        : Stmt(StmtKind::StructDecl), name(std::move(n)), fields(std::move(f)) {}
};

struct EnumDeclStmt : Stmt {
    std::string name;
    std::vector<std::string> variants;
    EnumDeclStmt(std::string n, std::vector<std::string> v)
        : Stmt(StmtKind::EnumDecl), name(std::move(n)), variants(std::move(v)) {}
};

struct TryStmt : Stmt {
    std::unique_ptr<BlockStmt> tryBlock;
    std::string catchVar;
    std::unique_ptr<BlockStmt> catchBlock;
    std::unique_ptr<BlockStmt> finallyBlock;  // nullable
    TryStmt(std::unique_ptr<BlockStmt> t, std::string cv, std::unique_ptr<BlockStmt> c,
            std::unique_ptr<BlockStmt> f)
        : Stmt(StmtKind::Try), tryBlock(std::move(t)), catchVar(std::move(cv)),
          catchBlock(std::move(c)), finallyBlock(std::move(f)) {}
};

struct ThrowStmt : Stmt {
    ExprPtr value;
    explicit ThrowStmt(ExprPtr v) : Stmt(StmtKind::Throw), value(std::move(v)) {}
};

struct IfStmt : Stmt {
    ExprPtr condition;
    std::unique_ptr<BlockStmt> thenBranch;
    std::unique_ptr<BlockStmt> elseBranch;  // nullable
    IfStmt(ExprPtr c, std::unique_ptr<BlockStmt> t, std::unique_ptr<BlockStmt> e)
        : Stmt(StmtKind::If), condition(std::move(c)), thenBranch(std::move(t)), elseBranch(std::move(e)) {}
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    std::unique_ptr<BlockStmt> body;
    WhileStmt(ExprPtr c, std::unique_ptr<BlockStmt> b)
        : Stmt(StmtKind::While), condition(std::move(c)), body(std::move(b)) {}
};

// `untuk (init; cond; post) { body }` -- classic C-style for. Each part is
// nullable (`untuk (;;) { ... }` is a valid infinite loop, same as C).
struct ForStmt : Stmt {
    StmtPtr init;         // nullable: LetStmt or ExprStmtNode
    ExprPtr condition;     // nullable
    ExprPtr post;          // nullable
    std::unique_ptr<BlockStmt> body;
    ForStmt(StmtPtr i, ExprPtr c, ExprPtr p, std::unique_ptr<BlockStmt> b)
        : Stmt(StmtKind::For), init(std::move(i)), condition(std::move(c)), post(std::move(p)), body(std::move(b)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // nullable
    explicit ReturnStmt(ExprPtr v) : Stmt(StmtKind::Return), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ExprStmtNode : Stmt {
    ExprPtr expr;
    explicit ExprStmtNode(ExprPtr e) : Stmt(StmtKind::ExprStmt), expr(std::move(e)) {}
};

struct Program {
    std::vector<StmtPtr> statements;
};
