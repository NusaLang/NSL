#include "typechecker.hpp"

#include <unordered_map>

#include "i18n.hpp"

namespace {
// Type spelling -> canonical name. One English alias per Indonesian
// spelling, matching the keyword aliases (lexer.cpp) 1:1. `any`/`apa`
// canonicalize to "" -- same as leaving the annotation off entirely.
const std::unordered_map<std::string, std::string>& typeAliasTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {"angka", "angka"}, {"num", "angka"},
        {"teks", "teks"}, {"str", "teks"},
        {"boolean", "boolean"}, {"bool", "boolean"},
        {"larik", "larik"}, {"array", "larik"},
        {"peta", "peta"}, {"map", "peta"},
        {"fungsi", "fungsi"}, {"func", "fungsi"},
        {"kosong", "kosong"}, {"null", "kosong"},
        {"any", ""}, {"apa", ""},
    };
    return table;
}
}  // namespace

bool TypeChecker::canonicalizeType(const std::string& raw, std::string& out) const {
    const auto& table = typeAliasTable();
    auto it = table.find(raw);
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

void TypeChecker::pushScope() { scopes_.emplace_back(); }
void TypeChecker::popScope() { scopes_.pop_back(); }

void TypeChecker::define(const std::string& name, StaticType t) {
    scopes_.back()[name] = std::move(t);
}

const TypeChecker::StaticType* TypeChecker::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

void TypeChecker::error(const std::string& msg, Span span) {
    errors_.push_back(msg + " (line " + std::to_string(span.line) + ", col " + std::to_string(span.column) + ")");
}

void TypeChecker::checkComparable(const StaticType& l, const StaticType& r, const std::string& op, Span span) {
    if (l.name.empty() || r.name.empty()) return;  // one side unknown -- gradual, don't flag it
    bool bothNum = l.name == "angka" && r.name == "angka";
    bool bothStr = l.name == "teks" && r.name == "teks";
    if (!bothNum && !bothStr) {
        error("operator '" + op +
                  i18n::tr("' butuh dua angka atau dua teks, dapat ", "' needs two numbers or two strings, got ") +
                  l.name + i18n::tr(" dan ", " and ") + r.name,
              span);
    }
}

TypeChecker::StaticType TypeChecker::inferExpr(const Expr* expr) {
    switch (expr->kind) {
        case ExprKind::Literal: {
            auto* n = static_cast<const LiteralExpr*>(expr);
            switch (n->litKind) {
                case LiteralExpr::Kind::Number: return {"angka"};
                case LiteralExpr::Kind::String: return {"teks"};
                case LiteralExpr::Kind::Bool: return {"boolean"};
                case LiteralExpr::Kind::Null: return {"kosong"};
            }
            return {};
        }
        case ExprKind::Identifier: {
            auto* n = static_cast<const IdentifierExpr*>(expr);
            const StaticType* t = lookup(n->name);
            return t ? *t : StaticType{};
        }
        case ExprKind::Unary: {
            auto* n = static_cast<const UnaryExpr*>(expr);
            StaticType operand = inferExpr(n->operand.get());
            if (n->op == "-") {
                if (!operand.name.empty() && operand.name != "angka") {
                    error(i18n::tr("operator unary '-' butuh angka, dapat ", "unary operator '-' needs a number, got ") +
                              operand.name,
                          n->span);
                }
                return {"angka"};
            }
            return {"boolean"};  // '!' always yields boolean
        }
        case ExprKind::Binary: {
            auto* n = static_cast<const BinaryExpr*>(expr);
            StaticType l = inferExpr(n->left.get());
            StaticType r = inferExpr(n->right.get());
            const std::string& op = n->op;

            if (op == "&&" || op == "||") return {};  // result mirrors whichever operand short-circuits to
            if (op == "==" || op == "!=") return {"boolean"};
            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                checkComparable(l, r, op, n->span);
                return {"boolean"};
            }
            if (op == "+") {
                if (l.name.empty() || r.name.empty()) return {};
                bool bothNum = l.name == "angka" && r.name == "angka";
                bool bothStr = l.name == "teks" && r.name == "teks";
                if (!bothNum && !bothStr) {
                    error(i18n::tr("operator '+' butuh dua angka atau dua teks, dapat ",
                                    "operator '+' needs two numbers or two strings, got ") +
                              l.name + i18n::tr(" dan ", " and ") + r.name,
                          n->span);
                    return {};
                }
                return {bothNum ? "angka" : "teks"};
            }
            // - * / %
            if (!l.name.empty() && l.name != "angka") {
                error("operator '" + op + i18n::tr("' butuh angka, dapat ", "' needs a number, got ") + l.name +
                          i18n::tr(" di sisi kiri", " on the left side"),
                      n->span);
            }
            if (!r.name.empty() && r.name != "angka") {
                error("operator '" + op + i18n::tr("' butuh angka, dapat ", "' needs a number, got ") + r.name +
                          i18n::tr(" di sisi kanan", " on the right side"),
                      n->span);
            }
            return {"angka"};
        }
        case ExprKind::Assign: {
            auto* n = static_cast<const AssignExpr*>(expr);
            StaticType value = inferExpr(n->value.get());
            const StaticType* declared = lookup(n->name);
            if (declared && !declared->name.empty() && !value.name.empty() && declared->name != value.name) {
                error(i18n::tr("nggak bisa isi variabel '", "can't assign to variable '") + n->name +
                          i18n::tr("' (tipe ", "' (type ") + declared->name +
                          i18n::tr(") dengan nilai bertipe ", ") with a value of type ") + value.name,
                      n->span);
            }
            return declared ? *declared : value;
        }
        case ExprKind::Call: {
            auto* n = static_cast<const CallExpr*>(expr);
            if (n->callee->kind == ExprKind::Identifier) {
                const std::string& calleeName = static_cast<const IdentifierExpr*>(n->callee.get())->name;
                const StaticType* t = lookup(calleeName);
                if (t && t->isFunction) {
                    if (n->args.size() != t->paramTypes.size()) {
                        error(i18n::tr("fungsi '", "function '") + calleeName +
                                  i18n::tr("' butuh ", "' needs ") + std::to_string(t->paramTypes.size()) +
                                  i18n::tr(" argumen, dipanggil dengan ", " arg(s), called with ") +
                                  std::to_string(n->args.size()),
                              n->span);
                    }
                    std::string returnType = t->returnType;  // copy: `t` may dangle after inferExpr recurses
                    for (size_t i = 0; i < n->args.size(); i++) {
                        StaticType argT = inferExpr(n->args[i].get());
                        if (i >= t->paramTypes.size()) continue;
                        const std::string& want = t->paramTypes[i];
                        if (!want.empty() && !argT.name.empty() && want != argT.name) {
                            error(i18n::tr("fungsi '", "function '") + calleeName +
                                      i18n::tr("' argumen ke-", "' argument #") + std::to_string(i + 1) +
                                      i18n::tr(" diharap tipe ", " expected type ") + want +
                                      i18n::tr(", dapat ", ", got ") + argT.name,
                                  n->args[i]->span);
                        }
                    }
                    return {returnType};
                }
            }
            // Unknown callee (builtin, a value passed through a
            // variable/parameter, an unannotated function, ...) --
            // still walk the callee/args so errors *inside* them
            // surface, just don't check the call itself.
            inferExpr(n->callee.get());
            for (const auto& a : n->args) inferExpr(a.get());
            return {};
        }
        case ExprKind::ArrayLit: {
            auto* n = static_cast<const ArrayLitExpr*>(expr);
            for (const auto& el : n->elements) inferExpr(el.get());
            return {"larik"};
        }
        case ExprKind::Index: {
            auto* n = static_cast<const IndexExpr*>(expr);
            inferExpr(n->target.get());
            inferExpr(n->index.get());
            return {};  // element/value type not tracked -- keeps this pass simple
        }
        case ExprKind::IndexAssign: {
            auto* n = static_cast<const IndexAssignExpr*>(expr);
            inferExpr(n->target.get());
            inferExpr(n->index.get());
            return inferExpr(n->value.get());
        }
        case ExprKind::FnExpr: {
            auto* n = static_cast<const FnExprNode*>(expr);
            const FnDeclStmt* decl = n->decl.get();
            StaticType sig;
            sig.isFunction = true;
            sig.name = "fungsi";
            sig.paramTypes.resize(decl->paramTypes.size());
            for (size_t i = 0; i < decl->paramTypes.size(); i++) {
                if (decl->paramTypes[i].empty()) continue;
                std::string canon;
                if (canonicalizeType(decl->paramTypes[i], canon)) sig.paramTypes[i] = canon;
            }
            if (!decl->returnType.empty()) {
                std::string canon;
                if (canonicalizeType(decl->returnType, canon)) sig.returnType = canon;
            }
            pushScope();
            for (size_t i = 0; i < decl->params.size(); i++) {
                define(decl->params[i], StaticType(sig.paramTypes[i]));
            }
            returnTypeStack_.push_back(sig.returnType);
            for (const auto& s : decl->body->statements) checkStmt(s.get());
            returnTypeStack_.pop_back();
            popScope();
            return sig;
        }
    }
    return {};
}

void TypeChecker::checkStmt(const Stmt* stmt) {
    switch (stmt->kind) {
        case StmtKind::Let: {
            auto* n = static_cast<const LetStmt*>(stmt);
            // Always check the initializer expression itself (catches
            // e.g. `buat x = 5 - "tiga";` regardless of annotation).
            StaticType valueType = inferExpr(n->value.get());
            // Tracked type defaults to any, NOT valueType -- an
            // unannotated `buat` must stay reassignable to any type
            // later (`buat x = 0; ... x = "selesai";` is normal and
            // must never be flagged). Only an explicit annotation locks it.
            StaticType tracked;
            if (!n->typeAnnotation.empty()) {
                std::string canon;
                if (!canonicalizeType(n->typeAnnotation, canon)) {
                    error(i18n::tr("tipe nggak dikenal: '", "unknown type: '") + n->typeAnnotation +
                              i18n::tr("' (variabel '", "' (variable '") + n->name + "')",
                          n->span);
                } else {
                    if (!canon.empty() && !valueType.name.empty() && canon != valueType.name) {
                        error(i18n::tr("variabel '", "variable '") + n->name +
                                  i18n::tr("' dianotasi tipe ", "' is annotated type ") + canon +
                                  i18n::tr(", tapi diisi nilai bertipe ", ", but assigned a value of type ") +
                                  valueType.name,
                              n->span);
                    }
                    tracked.name = canon;  // trust the annotation going forward, TS-style
                }
            }
            define(n->name, tracked);
            return;
        }
        case StmtKind::FnDecl: {
            auto* n = static_cast<const FnDeclStmt*>(stmt);
            StaticType sig;
            sig.isFunction = true;
            sig.name = "fungsi";
            sig.paramTypes.resize(n->paramTypes.size());
            for (size_t i = 0; i < n->paramTypes.size(); i++) {
                if (n->paramTypes[i].empty()) continue;
                std::string canon;
                if (!canonicalizeType(n->paramTypes[i], canon)) {
                    error(i18n::tr("tipe nggak dikenal: '", "unknown type: '") + n->paramTypes[i] +
                              i18n::tr("' (parameter '", "' (parameter '") + n->params[i] +
                              i18n::tr("' di fungsi '", "' in function '") + n->name + "')",
                          n->span);
                } else {
                    sig.paramTypes[i] = canon;
                }
            }
            if (!n->returnType.empty()) {
                std::string canon;
                if (!canonicalizeType(n->returnType, canon)) {
                    error(i18n::tr("tipe nggak dikenal: '", "unknown type: '") + n->returnType +
                              i18n::tr("' (return type fungsi '", "' (return type of function '") + n->name + "')",
                          n->span);
                } else {
                    sig.returnType = canon;
                }
            }
            // Defined in the *enclosing* scope (so callers and, via
            // this same define() before the body is walked, recursive
            // calls from inside its own body can both resolve it) --
            // mirrors Interpreter::exec's StmtKind::FnDecl exactly.
            define(n->name, sig);

            pushScope();
            for (size_t i = 0; i < n->params.size(); i++) {
                define(n->params[i], StaticType(sig.paramTypes[i]));
            }
            returnTypeStack_.push_back(sig.returnType);
            for (const auto& s : n->body->statements) checkStmt(s.get());
            returnTypeStack_.pop_back();
            popScope();
            return;
        }
        case StmtKind::Block: {
            auto* n = static_cast<const BlockStmt*>(stmt);
            pushScope();
            for (const auto& s : n->statements) checkStmt(s.get());
            popScope();
            return;
        }
        case StmtKind::If: {
            auto* n = static_cast<const IfStmt*>(stmt);
            inferExpr(n->condition.get());
            pushScope();
            for (const auto& s : n->thenBranch->statements) checkStmt(s.get());
            popScope();
            if (n->elseBranch) {
                pushScope();
                for (const auto& s : n->elseBranch->statements) checkStmt(s.get());
                popScope();
            }
            return;
        }
        case StmtKind::While: {
            auto* n = static_cast<const WhileStmt*>(stmt);
            inferExpr(n->condition.get());
            pushScope();
            for (const auto& s : n->body->statements) checkStmt(s.get());
            popScope();
            return;
        }
        case StmtKind::For: {
            auto* n = static_cast<const ForStmt*>(stmt);
            pushScope();  // `untuk (buat i = 0; ...)` -- init's scope spans the whole loop
            if (n->init) checkStmt(n->init.get());
            if (n->condition) inferExpr(n->condition.get());
            pushScope();
            for (const auto& s : n->body->statements) checkStmt(s.get());
            popScope();
            if (n->post) inferExpr(n->post.get());
            popScope();
            return;
        }
        case StmtKind::Return: {
            auto* n = static_cast<const ReturnStmt*>(stmt);
            StaticType valueType = n->value ? inferExpr(n->value.get()) : StaticType{"kosong"};
            if (!returnTypeStack_.empty()) {
                const std::string& want = returnTypeStack_.back();
                if (!want.empty() && !valueType.name.empty() && want != valueType.name) {
                    error(i18n::tr("hasil bertipe ", "returned type ") + valueType.name +
                              i18n::tr(", tapi fungsi ini dianotasi hasil ",
                                        ", but this function is annotated to return ") +
                              want,
                          n->span);
                }
            }
            return;
        }
        case StmtKind::ClassDecl:
        case StmtKind::StructDecl:
        case StmtKind::EnumDecl:
        case StmtKind::Break:
        case StmtKind::Continue:
            return;
        case StmtKind::Try: {
            auto* n = static_cast<const TryStmt*>(stmt);
            pushScope();
            for (const auto& s : n->tryBlock->statements) checkStmt(s.get());
            popScope();
            pushScope();
            define(n->catchVar, StaticType{});
            for (const auto& s : n->catchBlock->statements) checkStmt(s.get());
            popScope();
            if (n->finallyBlock) {
                pushScope();
                for (const auto& s : n->finallyBlock->statements) checkStmt(s.get());
                popScope();
            }
            return;
        }
        case StmtKind::Throw: {
            auto* n = static_cast<const ThrowStmt*>(stmt);
            inferExpr(n->value.get());
            return;
        }
        case StmtKind::ExprStmt: {
            auto* n = static_cast<const ExprStmtNode*>(stmt);
            inferExpr(n->expr.get());
            return;
        }
    }
}

void TypeChecker::check(const Program& program) {
    scopes_.clear();
    errors_.clear();
    returnTypeStack_.clear();

    pushScope();  // global scope. Builtins are deliberately absent here
                   // (they're not FnDeclStmt-backed) -- calling one
                   // always resolves as "unknown callee", i.e. `any`.
    for (const auto& stmt : program.statements) checkStmt(stmt.get());
    popScope();

    if (!errors_.empty()) {
        std::string msg = std::to_string(errors_.size()) + " error tipe ditemukan:\n";
        for (size_t i = 0; i < errors_.size(); i++) {
            msg += "  " + std::to_string(i + 1) + ". " + errors_[i] + "\n";
        }
        throw TypeError(msg);
    }
}
