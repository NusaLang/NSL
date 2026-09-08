#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"

class TypeError : public std::runtime_error {
public:
    explicit TypeError(const std::string& msg) : std::runtime_error(msg) {}
};

// Gradual, TypeScript-`any`-style checker: annotations are 100%
// optional, unannotated/unprovable types are "any" and never flagged,
// only provable mismatches error. No generics/union types/flow
// narrowing; doesn't check inside impor()-ed modules.
class TypeChecker {
public:
    // Throws TypeError listing every error found (not just the first).
    void check(const Program& program);

private:
    struct StaticType {
        std::string name;  // canonical type name; "" = unknown/any
        bool isFunction = false;
        std::vector<std::string> paramTypes;  // meaningful only if isFunction
        std::string returnType;               // meaningful only if isFunction

        StaticType() = default;
        StaticType(std::string n) : name(std::move(n)) {}  // NOLINT(*-explicit-constructor)
    };

    void checkStmt(const Stmt* stmt);
    StaticType inferExpr(const Expr* expr);
    void checkComparable(const StaticType& l, const StaticType& r, const std::string& op, Span span);

    void pushScope();
    void popScope();
    void define(const std::string& name, StaticType t);
    const StaticType* lookup(const std::string& name) const;
    // Indonesian/English alias -> canonical type name. False on an
    // unrecognized spelling (reported as an error, not silently "any").
    bool canonicalizeType(const std::string& raw, std::string& out) const;
    void error(const std::string& msg, Span span);

    std::vector<std::unordered_map<std::string, StaticType>> scopes_;
    std::vector<std::string> returnTypeStack_;
    std::vector<std::string> errors_;
};
