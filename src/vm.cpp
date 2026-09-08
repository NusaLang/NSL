#include "vm.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "gc.hpp"
#include "gil.hpp"
#include "i18n.hpp"
#include "interpreter.hpp"
#include "jit.hpp"

namespace {

void collectIdentifiersInBlock(const BlockStmt* b, std::unordered_set<std::string>& out);

void collectIdentifiers(const Expr* e, std::unordered_set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
        case ExprKind::Literal: return;
        case ExprKind::Identifier: out.insert(static_cast<const IdentifierExpr*>(e)->name); return;
        case ExprKind::Unary: collectIdentifiers(static_cast<const UnaryExpr*>(e)->operand.get(), out); return;
        case ExprKind::Binary: {
            auto* n = static_cast<const BinaryExpr*>(e);
            collectIdentifiers(n->left.get(), out);
            collectIdentifiers(n->right.get(), out);
            return;
        }
        case ExprKind::Assign: {
            auto* n = static_cast<const AssignExpr*>(e);
            out.insert(n->name);
            collectIdentifiers(n->value.get(), out);
            return;
        }
        case ExprKind::Call: {
            auto* n = static_cast<const CallExpr*>(e);
            collectIdentifiers(n->callee.get(), out);
            for (auto& a : n->args) collectIdentifiers(a.get(), out);
            return;
        }
        case ExprKind::ArrayLit:
            for (auto& el : static_cast<const ArrayLitExpr*>(e)->elements) collectIdentifiers(el.get(), out);
            return;
        case ExprKind::Index: {
            auto* n = static_cast<const IndexExpr*>(e);
            collectIdentifiers(n->target.get(), out);
            collectIdentifiers(n->index.get(), out);
            return;
        }
        case ExprKind::IndexAssign: {
            auto* n = static_cast<const IndexAssignExpr*>(e);
            collectIdentifiers(n->target.get(), out);
            collectIdentifiers(n->index.get(), out);
            collectIdentifiers(n->value.get(), out);
            return;
        }
        case ExprKind::FnExpr:
            collectIdentifiersInBlock(static_cast<const FnExprNode*>(e)->decl->body.get(), out);
            return;
    }
}

void collectIdentifiersInStmt(const Stmt* s, std::unordered_set<std::string>& out);

void collectIdentifiersInBlock(const BlockStmt* b, std::unordered_set<std::string>& out) {
    if (!b) return;
    for (auto& st : b->statements) collectIdentifiersInStmt(st.get(), out);
}

void collectIdentifiersInStmt(const Stmt* s, std::unordered_set<std::string>& out) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Let: {
            auto* n = static_cast<const LetStmt*>(s);
            collectIdentifiers(n->value.get(), out);
            return;
        }
        case StmtKind::FnDecl: {
            auto* n = static_cast<const FnDeclStmt*>(s);
            collectIdentifiersInBlock(n->body.get(), out);
            return;
        }
        case StmtKind::ClassDecl: return;
        case StmtKind::Block: collectIdentifiersInBlock(static_cast<const BlockStmt*>(s), out); return;
        case StmtKind::If: {
            auto* n = static_cast<const IfStmt*>(s);
            collectIdentifiers(n->condition.get(), out);
            collectIdentifiersInBlock(n->thenBranch.get(), out);
            collectIdentifiersInBlock(n->elseBranch.get(), out);
            return;
        }
        case StmtKind::While: {
            auto* n = static_cast<const WhileStmt*>(s);
            collectIdentifiers(n->condition.get(), out);
            collectIdentifiersInBlock(n->body.get(), out);
            return;
        }
        case StmtKind::For: {
            auto* n = static_cast<const ForStmt*>(s);
            collectIdentifiersInStmt(n->init.get(), out);
            collectIdentifiers(n->condition.get(), out);
            collectIdentifiers(n->post.get(), out);
            collectIdentifiersInBlock(n->body.get(), out);
            return;
        }
        case StmtKind::Return: collectIdentifiers(static_cast<const ReturnStmt*>(s)->value.get(), out); return;
        case StmtKind::Break: return;
        case StmtKind::Continue: return;
        case StmtKind::ExprStmt: collectIdentifiers(static_cast<const ExprStmtNode*>(s)->expr.get(), out); return;
        case StmtKind::StructDecl: return;
        case StmtKind::EnumDecl: return;
        case StmtKind::Try: {
            auto* n = static_cast<const TryStmt*>(s);
            collectIdentifiersInBlock(n->tryBlock.get(), out);
            collectIdentifiersInBlock(n->catchBlock.get(), out);
            collectIdentifiersInBlock(n->finallyBlock.get(), out);
            return;
        }
        case StmtKind::Throw: collectIdentifiers(static_cast<const ThrowStmt*>(s)->value.get(), out); return;
    }
}

void findCapturedNames(const std::vector<StmtPtr>& statements, std::unordered_set<std::string>& out);

void findCapturedNames(const BlockStmt* body, std::unordered_set<std::string>& out) {
    if (!body) return;
    findCapturedNames(body->statements, out);
}

void findCapturedNames(const std::vector<StmtPtr>& statements, std::unordered_set<std::string>& out) {
    for (auto& st : statements) {
        switch (st->kind) {
            case StmtKind::FnDecl: {
                auto* n = static_cast<const FnDeclStmt*>(st.get());
                collectIdentifiersInBlock(n->body.get(), out);
                findCapturedNames(n->body.get(), out);
                break;
            }
            case StmtKind::Block: findCapturedNames(static_cast<const BlockStmt*>(st.get()), out); break;
            case StmtKind::If: {
                auto* n = static_cast<const IfStmt*>(st.get());
                findCapturedNames(n->thenBranch.get(), out);
                findCapturedNames(n->elseBranch.get(), out);
                break;
            }
            case StmtKind::While: findCapturedNames(static_cast<const WhileStmt*>(st.get())->body.get(), out); break;
            case StmtKind::For: findCapturedNames(static_cast<const ForStmt*>(st.get())->body.get(), out); break;
            case StmtKind::Try: {
                auto* n = static_cast<const TryStmt*>(st.get());
                findCapturedNames(n->tryBlock.get(), out);
                findCapturedNames(n->catchBlock.get(), out);
                findCapturedNames(n->finallyBlock.get(), out);
                break;
            }
            default: break;
        }
    }
}

struct Local {
    std::string name;
    int depth;
    bool boxed;
    int slot;
};

struct LoopCtx {
    std::vector<int> breakJumps;
    std::vector<int> continueJumps;
};

class FnCompiler {
public:
    FnCompiler(FnCompiler* enclosing_, VmFunction* fn_) : enclosing(enclosing_), fn(fn_) {}

    FnCompiler* enclosing;
    VmFunction* fn;
    std::vector<Local> locals;
    int scopeDepth = 0;
    std::vector<LoopCtx> loops;
    std::unordered_set<std::string> capturedNames;
    int nextUnboxedSlot = 0;
    int nextBoxedSlot = 0;

    int addLocal(const std::string& name) {
        bool boxed = capturedNames.count(name) != 0;
        int slot = boxed ? nextBoxedSlot++ : nextUnboxedSlot++;
        locals.push_back({name, scopeDepth, boxed, slot});
        if (boxed) fn->numBoxedLocals = nextBoxedSlot;
        else fn->numLocals = nextUnboxedSlot;
        return static_cast<int>(locals.size()) - 1;
    }

    int resolveLocal(const std::string& name) {
        for (int i = static_cast<int>(locals.size()) - 1; i >= 0; i--) {
            if (locals[i].name == name) return i;
        }
        return -1;
    }

    int addUpvalue(bool isLocal, int index) {
        for (size_t i = 0; i < fn->upvalues.size(); i++) {
            if (fn->upvalues[i].isLocal == isLocal && fn->upvalues[i].index == index) return static_cast<int>(i);
        }
        fn->upvalues.push_back({isLocal, index});
        return static_cast<int>(fn->upvalues.size()) - 1;
    }

    int resolveUpvalue(const std::string& name) {
        if (!enclosing) return -1;
        int localIdx = enclosing->resolveLocal(name);
        if (localIdx != -1) return addUpvalue(true, enclosing->locals[static_cast<size_t>(localIdx)].slot);
        int upIdx = enclosing->resolveUpvalue(name);
        if (upIdx != -1) return addUpvalue(false, upIdx);
        return -1;
    }

    void beginScope() { scopeDepth++; }
    void endScope() {
        scopeDepth--;
        while (!locals.empty() && locals.back().depth > scopeDepth) locals.pop_back();
    }

    void emitByte(uint8_t b) { fn->code.push_back(b); }
    void emitOp(Op op) { emitByte(static_cast<uint8_t>(op)); }
    void emitU16(uint16_t v) {
        emitByte(static_cast<uint8_t>(v & 0xFF));
        emitByte(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    int addConstant(Value v) {
        fn->constants.push_back(std::move(v));
        return static_cast<int>(fn->constants.size()) - 1;
    }
    void emitConstant(Value v) {
        emitOp(Op::Const);
        emitU16(static_cast<uint16_t>(addConstant(std::move(v))));
    }
    int emitJump(Op op) {
        emitOp(op);
        emitU16(0xFFFF);
        return static_cast<int>(fn->code.size()) - 2;
    }
    void patchJump(int offset) {
        int jump = static_cast<int>(fn->code.size()) - offset - 2;
        fn->code[offset] = static_cast<uint8_t>(jump & 0xFF);
        fn->code[offset + 1] = static_cast<uint8_t>((jump >> 8) & 0xFF);
    }
    void emitLoop(int loopStart) {
        emitOp(Op::Loop);
        int offset = static_cast<int>(fn->code.size()) - loopStart + 2;
        emitU16(static_cast<uint16_t>(offset));
    }
};

// NUSA_NO_JIT=1 forces pure-bytecode execution, for JIT-on/off diffing.
// See tests/cases/jit_*.ns.
bool jitDisabled() {
    static const bool d = std::getenv("NUSA_NO_JIT") != nullptr;
    return d;
}

class Compiler {
public:
    explicit Compiler(VmProgram& program_) : program(program_) {}

    VmProgram& program;
    FnCompiler* current = nullptr;
    std::unordered_set<std::string> userFunctions;
    // Top-level fn decls only -- lets the loop call-inlining JIT resolve a
    // callee back to its AST (shadowing checked separately at the call site).
    std::unordered_map<std::string, const FnDeclStmt*> fnDeclsByName;

    bool isUserFn(const std::string& name) const {
        if (userFunctions.count(name) != 0) return true;
        if (current && (current->resolveLocal(name) != -1 || current->resolveUpvalue(name) != -1)) return true;
        return false;
    }

    VmFunction* newFunction(const std::string& name) {
        auto fn = std::make_unique<VmFunction>();
        fn->name = name;
        VmFunction* raw = fn.get();
        program.functions.push_back(std::move(fn));
        return raw;
    }

    void compileIdentifierGet(const std::string& name) {
        int local = current->resolveLocal(name);
        if (local != -1) {
            const Local& l = current->locals[static_cast<size_t>(local)];
            current->emitOp(l.boxed ? Op::GetBoxedLocal : Op::GetLocal);
            current->emitU16(static_cast<uint16_t>(l.slot));
            return;
        }
        int up = current->resolveUpvalue(name);
        if (up != -1) {
            current->emitOp(Op::GetUpvalue);
            current->emitU16(static_cast<uint16_t>(up));
            return;
        }
        current->emitOp(Op::GetGlobal);
        current->emitU16(static_cast<uint16_t>(current->addConstant(Value::fromString(name))));
    }

    void compileIdentifierSet(const std::string& name) {
        int local = current->resolveLocal(name);
        if (local != -1) {
            const Local& l = current->locals[static_cast<size_t>(local)];
            current->emitOp(l.boxed ? Op::SetBoxedLocal : Op::SetLocal);
            current->emitU16(static_cast<uint16_t>(l.slot));
            return;
        }
        int up = current->resolveUpvalue(name);
        if (up != -1) {
            current->emitOp(Op::SetUpvalue);
            current->emitU16(static_cast<uint16_t>(up));
            return;
        }
        current->emitOp(Op::SetGlobal);
        current->emitU16(static_cast<uint16_t>(current->addConstant(Value::fromString(name))));
    }

    void compileExpr(const Expr* e) {
        switch (e->kind) {
            case ExprKind::Literal: {
                auto* n = static_cast<const LiteralExpr*>(e);
                switch (n->litKind) {
                    case LiteralExpr::Kind::Number: current->emitConstant(Value::fromNumber(n->number)); return;
                    case LiteralExpr::Kind::String: current->emitConstant(Value::fromString(n->str)); return;
                    case LiteralExpr::Kind::Bool: current->emitOp(n->boolean ? Op::True : Op::False); return;
                    case LiteralExpr::Kind::Null: current->emitOp(Op::Null); return;
                }
                return;
            }
            case ExprKind::Identifier: {
                auto* n = static_cast<const IdentifierExpr*>(e);
                compileIdentifierGet(n->name);
                return;
            }
            case ExprKind::Unary: {
                auto* n = static_cast<const UnaryExpr*>(e);
                compileExpr(n->operand.get());
                if (n->op == "-") { current->emitOp(Op::Neg); return; }
                if (n->op == "!") { current->emitOp(Op::Not); return; }
                throw VmCompileError("operator unary '" + n->op + "' belum didukung mode --vm");
            }
            case ExprKind::Binary: {
                auto* n = static_cast<const BinaryExpr*>(e);
                if (n->op == "&&") {
                    compileExpr(n->left.get());
                    int j = current->emitJump(Op::JumpIfFalseKeep);
                    current->emitOp(Op::Pop);
                    compileExpr(n->right.get());
                    current->patchJump(j);
                    return;
                }
                if (n->op == "||") {
                    compileExpr(n->left.get());
                    int j = current->emitJump(Op::JumpIfTrueKeep);
                    current->emitOp(Op::Pop);
                    compileExpr(n->right.get());
                    current->patchJump(j);
                    return;
                }
                compileExpr(n->left.get());
                compileExpr(n->right.get());
                if (n->op == "+") { current->emitOp(Op::Add); return; }
                if (n->op == "-") { current->emitOp(Op::Sub); return; }
                if (n->op == "*") { current->emitOp(Op::Mul); return; }
                if (n->op == "/") { current->emitOp(Op::Div); return; }
                if (n->op == "%") { current->emitOp(Op::Mod); return; }
                if (n->op == "==") { current->emitOp(Op::Eq); return; }
                if (n->op == "!=") { current->emitOp(Op::Neq); return; }
                if (n->op == "<") { current->emitOp(Op::Lt); return; }
                if (n->op == "<=") { current->emitOp(Op::Lte); return; }
                if (n->op == ">") { current->emitOp(Op::Gt); return; }
                if (n->op == ">=") { current->emitOp(Op::Gte); return; }
                throw VmCompileError("operator '" + n->op + "' belum didukung mode --vm");
            }
            case ExprKind::Assign: {
                auto* n = static_cast<const AssignExpr*>(e);
                compileExpr(n->value.get());
                compileIdentifierSet(n->name);
                return;
            }
            case ExprKind::Call: {
                auto* n = static_cast<const CallExpr*>(e);
                if (n->callee->kind == ExprKind::Identifier) {
                    auto* id = static_cast<const IdentifierExpr*>(n->callee.get());
                    if (id->name == "cetak" || id->name == "print") {
                        for (auto& a : n->args) compileExpr(a.get());
                        current->emitOp(Op::Print);
                        current->emitByte(static_cast<uint8_t>(n->args.size()));
                        return;
                    }
                    if ((id->name == "panjang" || id->name == "length") && !isUserFn(id->name)) {
                        if (n->args.size() != 1) throw VmCompileError(id->name + "() butuh 1 argumen mode --vm");
                        compileExpr(n->args[0].get());
                        current->emitOp(Op::Length);
                        return;
                    }
                    if ((id->name == "tambah" || id->name == "push") && !isUserFn(id->name)) {
                        if (n->args.size() != 2) throw VmCompileError(id->name + "() butuh 2 argumen mode --vm");
                        compileExpr(n->args[0].get());
                        compileExpr(n->args[1].get());
                        current->emitOp(Op::Push);
                        return;
                    }
                    if (id->name == "jalan" || id->name == "go") {
                        if (n->args.empty()) throw VmCompileError(id->name + "() butuh minimal 1 argumen mode --vm");
                        for (auto& a : n->args) compileExpr(a.get());
                        current->emitOp(Op::GoSpawn);
                        current->emitByte(static_cast<uint8_t>(n->args.size()));
                        return;
                    }
                    if (id->name == "kanal_baru" || id->name == "channel") {
                        if (n->args.size() > 1) throw VmCompileError(id->name + "() butuh 0 atau 1 argumen mode --vm");
                        if (n->args.size() == 1) compileExpr(n->args[0].get());
                        current->emitOp(Op::ChanNew);
                        current->emitByte(static_cast<uint8_t>(n->args.size()));
                        return;
                    }
                    if (id->name == "kanal_kirim" || id->name == "chan_send") {
                        if (n->args.size() != 2) throw VmCompileError(id->name + "() butuh 2 argumen mode --vm");
                        compileExpr(n->args[0].get());
                        compileExpr(n->args[1].get());
                        current->emitOp(Op::ChanSend);
                        return;
                    }
                    if (id->name == "kanal_terima" || id->name == "chan_recv") {
                        if (n->args.size() != 1) throw VmCompileError(id->name + "() butuh 1 argumen mode --vm");
                        compileExpr(n->args[0].get());
                        current->emitOp(Op::ChanRecv);
                        return;
                    }
                    if (id->name == "impor" || id->name == "import") {
                        if (n->args.size() != 1) throw VmCompileError(id->name + "() butuh 1 argumen mode --vm");
                        compileExpr(n->args[0].get());
                        current->emitOp(Op::Import);
                        return;
                    }
                }
                if (n->callee->kind == ExprKind::Index) {
                    auto* idx = static_cast<const IndexExpr*>(n->callee.get());
                    compileExpr(idx->target.get());
                    compileExpr(idx->index.get());
                    for (auto& a : n->args) compileExpr(a.get());
                    current->emitOp(Op::CallMethod);
                    current->emitByte(static_cast<uint8_t>(n->args.size()));
                    return;
                }
                compileExpr(n->callee.get());
                for (auto& a : n->args) compileExpr(a.get());
                current->emitOp(Op::Call);
                current->emitByte(static_cast<uint8_t>(n->args.size()));
                return;
            }
            case ExprKind::ArrayLit: {
                auto* n = static_cast<const ArrayLitExpr*>(e);
                for (auto& el : n->elements) compileExpr(el.get());
                current->emitOp(Op::NewArray);
                current->emitU16(static_cast<uint16_t>(n->elements.size()));
                return;
            }
            case ExprKind::Index: {
                auto* n = static_cast<const IndexExpr*>(e);
                compileExpr(n->target.get());
                compileExpr(n->index.get());
                current->emitOp(Op::GetIndex);
                return;
            }
            case ExprKind::IndexAssign: {
                auto* n = static_cast<const IndexAssignExpr*>(e);
                compileExpr(n->target.get());
                compileExpr(n->index.get());
                compileExpr(n->value.get());
                current->emitOp(Op::SetIndex);
                return;
            }
            case ExprKind::FnExpr:
                throw VmCompileError("fungsi anonim belum didukung mode --vm");
        }
    }

    void compileFunctionBody(const FnDeclStmt* decl, VmFunction* fn) {
        FnCompiler fc(current, fn);
        current = &fc;
        findCapturedNames(decl->body.get(), fc.capturedNames);
        fc.beginScope();
        std::vector<ParamSlot> paramSlots;
        for (auto& p : decl->params) {
            int li = fc.addLocal(p);
            const Local& l = fc.locals[static_cast<size_t>(li)];
            paramSlots.push_back({l.boxed, l.slot});
        }
        fn->paramSlots = std::move(paramSlots);
        fn->arity = static_cast<int>(decl->params.size());
        {
            JitFuncResult jf = jitDisabled() ? JitFuncResult{} : tryCompileNativeFunc(decl);
            if (jf.ok) {
                fn->nativeCode = jf.code;
                program.nativeFuncLiteralPools.push_back(std::move(jf.literalPool));
            }
        }
        compileBlock(decl->body.get());
        fc.endScope();
        current->emitOp(Op::ReturnNull);
        current = fc.enclosing;
    }

    void compileFnDecl(const FnDeclStmt* decl, bool topLevel) {
        if (topLevel) fnDeclsByName[decl->name] = decl;
        VmFunction* fn = newFunction(decl->name);
        FnCompiler* declaringCompiler = current;
        compileFunctionBody(decl, fn);
        declaringCompiler->emitOp(Op::MakeClosure);
        declaringCompiler->emitU16(static_cast<uint16_t>(indexOfFunction(fn)));
        for (auto& uv : fn->upvalues) {
            declaringCompiler->emitByte(uv.isLocal ? 1 : 0);
            declaringCompiler->emitU16(static_cast<uint16_t>(uv.index));
        }
        if (topLevel) {
            declaringCompiler->emitOp(Op::DefineGlobal);
            declaringCompiler->emitU16(static_cast<uint16_t>(declaringCompiler->addConstant(Value::fromString(decl->name))));
        } else {
            int li = declaringCompiler->addLocal(decl->name);
            const Local& l = declaringCompiler->locals[static_cast<size_t>(li)];
            declaringCompiler->emitOp(l.boxed ? Op::DefineBoxedLocal : Op::DefineLocal);
            declaringCompiler->emitU16(static_cast<uint16_t>(l.slot));
        }
    }

    int indexOfFunction(VmFunction* fn) {
        for (size_t i = 0; i < program.functions.size(); i++) {
            if (program.functions[i].get() == fn) return static_cast<int>(i);
        }
        return -1;
    }

    // Resolves an `acc = someFn(...)` loop body's callee for the whole-loop
    // inlining JIT. nullptr unless it provably resolves to a global function.
    const FnDeclStmt* resolveInlineCallee(const ForStmt* n) {
        if (!n->body || n->body->statements.size() != 1) return nullptr;
        if (n->body->statements[0]->kind != StmtKind::ExprStmt) return nullptr;
        const Expr* inner = static_cast<const ExprStmtNode*>(n->body->statements[0].get())->expr.get();
        if (inner->kind != ExprKind::Assign) return nullptr;
        auto* asg = static_cast<const AssignExpr*>(inner);
        if (asg->value->kind != ExprKind::Call) return nullptr;
        auto* call = static_cast<const CallExpr*>(asg->value.get());
        if (call->callee->kind != ExprKind::Identifier) return nullptr;
        const std::string& calleeName = static_cast<const IdentifierExpr*>(call->callee.get())->name;
        if (current->resolveLocal(calleeName) != -1 || current->resolveUpvalue(calleeName) != -1) return nullptr;
        auto it = fnDeclsByName.find(calleeName);
        return it == fnDeclsByName.end() ? nullptr : it->second;
    }

    int tryRegisterNativeLoop(const ForStmt* n) {
        if (jitDisabled()) return -1;
        JitLoopResult jit = tryCompileNativeLoop(n);
        if (!jit.ok) jit = tryCompileScalarLoop(n, resolveInlineCallee(n));
        if (!jit.ok) return -1;
        program.nativeFuncLiteralPools.push_back(std::move(jit.literalPool));
        NativeLoopDesc desc;
        desc.code = jit.code;
        desc.codeSize = jit.codeSize;
        desc.isMap = jit.isMap;
        desc.boundIsLiteral = jit.boundIsLiteral;
        desc.boundLiteral = jit.boundLiteral;
        for (auto& name : jit.arrayNames) {
            int li = current->resolveLocal(name);
            if (li == -1) return -1;
            const Local& l = current->locals[static_cast<size_t>(li)];
            desc.arraySlots.push_back(l.slot);
            desc.arrayBoxed.push_back(l.boxed ? 1 : 0);
        }
        if (!jit.boundIsLiteral) {
            int li = current->resolveLocal(jit.boundVar);
            if (li == -1) {
                // Not a local -- allow only a provable global (no upvalue
                // shadow); arrays/maps stay local-only, see accumIsGlobal.
                if (jit.isMap) return -1;
                if (current->resolveUpvalue(jit.boundVar) != -1) return -1;
                desc.boundIsGlobal = true;
                desc.boundGlobalName = jit.boundVar;
            } else {
                const Local& l = current->locals[static_cast<size_t>(li)];
                desc.boundSlot = l.slot;
                desc.boundBoxed = l.boxed ? 1 : 0;
            }
        }
        if (jit.isMap) {
            // Arrays/map output intentionally stay local-only -- see the
            // comment on NativeLoopDesc::accumIsGlobal.
            int li = current->resolveLocal(jit.outVar);
            if (li == -1) return -1;
            const Local& l = current->locals[static_cast<size_t>(li)];
            desc.outSlot = l.slot;
            desc.outBoxed = l.boxed ? 1 : 0;
        } else {
            int li = current->resolveLocal(jit.accumVar);
            if (li == -1) {
                if (current->resolveUpvalue(jit.accumVar) != -1) return -1;
                desc.accumIsGlobal = true;
                desc.accumGlobalName = jit.accumVar;
            } else {
                const Local& l = current->locals[static_cast<size_t>(li)];
                desc.accumSlot = l.slot;
                desc.accumBoxed = l.boxed ? 1 : 0;
            }
        }
        program.nativeLoops.push_back(desc);
        return static_cast<int>(program.nativeLoops.size()) - 1;
    }

    void compileStmt(const Stmt* s, bool topLevel) {
        switch (s->kind) {
            case StmtKind::Let: {
                auto* n = static_cast<const LetStmt*>(s);
                compileExpr(n->value.get());
                if (topLevel && current->enclosing == nullptr && current == topCompiler) {
                    current->emitOp(Op::DefineGlobal);
                    current->emitU16(static_cast<uint16_t>(current->addConstant(Value::fromString(n->name))));
                } else {
                    int li = current->addLocal(n->name);
                    const Local& l = current->locals[static_cast<size_t>(li)];
                    current->emitOp(l.boxed ? Op::DefineBoxedLocal : Op::DefineLocal);
                    current->emitU16(static_cast<uint16_t>(l.slot));
                }
                return;
            }
            case StmtKind::FnDecl: {
                auto* n = static_cast<const FnDeclStmt*>(s);
                compileFnDecl(n, topLevel && current == topCompiler);
                return;
            }
            case StmtKind::ClassDecl:
                throw VmCompileError("kelas belum didukung mode --vm");
            case StmtKind::Block: {
                auto* n = static_cast<const BlockStmt*>(s);
                current->beginScope();
                compileBlock(n);
                current->endScope();
                return;
            }
            case StmtKind::If: {
                auto* n = static_cast<const IfStmt*>(s);
                compileExpr(n->condition.get());
                int elseJump = current->emitJump(Op::JumpIfFalse);
                current->beginScope();
                compileBlock(n->thenBranch.get());
                current->endScope();
                int endJump = current->emitJump(Op::Jump);
                current->patchJump(elseJump);
                if (n->elseBranch) {
                    current->beginScope();
                    compileBlock(n->elseBranch.get());
                    current->endScope();
                }
                current->patchJump(endJump);
                return;
            }
            case StmtKind::While: {
                auto* n = static_cast<const WhileStmt*>(s);
                int loopStart = static_cast<int>(current->fn->code.size());
                compileExpr(n->condition.get());
                int exitJump = current->emitJump(Op::JumpIfFalse);
                current->loops.push_back({});
                current->beginScope();
                compileBlock(n->body.get());
                current->endScope();
                for (int cj : current->loops.back().continueJumps) current->patchJump(cj);
                current->emitLoop(loopStart);
                current->patchJump(exitJump);
                for (int bj : current->loops.back().breakJumps) current->patchJump(bj);
                current->loops.pop_back();
                return;
            }
            case StmtKind::For: {
                auto* n = static_cast<const ForStmt*>(s);
                int nativeIdx = tryRegisterNativeLoop(n);
                int skipJump = -1;
                if (nativeIdx != -1) {
                    current->emitOp(Op::TryNativeLoop);
                    current->emitU16(static_cast<uint16_t>(nativeIdx));
                    skipJump = current->emitJump(Op::JumpIfFalse);
                }
                current->beginScope();
                if (n->init) compileStmt(n->init.get(), false);
                int loopStart = static_cast<int>(current->fn->code.size());
                int exitJump = -1;
                if (n->condition) {
                    compileExpr(n->condition.get());
                    exitJump = current->emitJump(Op::JumpIfFalse);
                }
                current->loops.push_back({});
                current->beginScope();
                compileBlock(n->body.get());
                current->endScope();
                for (int cj : current->loops.back().continueJumps) current->patchJump(cj);
                if (n->post) {
                    compileExpr(n->post.get());
                    current->emitOp(Op::Pop);
                }
                current->emitLoop(loopStart);
                if (exitJump != -1) current->patchJump(exitJump);
                for (int bj : current->loops.back().breakJumps) current->patchJump(bj);
                current->loops.pop_back();
                current->endScope();
                if (skipJump != -1) current->patchJump(skipJump);
                return;
            }
            case StmtKind::Return: {
                auto* n = static_cast<const ReturnStmt*>(s);
                if (n->value) {
                    compileExpr(n->value.get());
                    current->emitOp(Op::Return);
                } else {
                    current->emitOp(Op::ReturnNull);
                }
                return;
            }
            case StmtKind::Break: {
                if (current->loops.empty()) throw VmCompileError("'berhenti' di luar loop");
                int j = current->emitJump(Op::Jump);
                current->loops.back().breakJumps.push_back(j);
                return;
            }
            case StmtKind::Continue: {
                if (current->loops.empty()) throw VmCompileError("'lanjut' di luar loop");
                int j = current->emitJump(Op::Jump);
                current->loops.back().continueJumps.push_back(j);
                return;
            }
            case StmtKind::ExprStmt: {
                auto* n = static_cast<const ExprStmtNode*>(s);
                compileExpr(n->expr.get());
                current->emitOp(Op::Pop);
                return;
            }
            case StmtKind::StructDecl:
                throw VmCompileError("bentuk belum didukung mode --vm");
            case StmtKind::EnumDecl:
                throw VmCompileError("jenis belum didukung mode --vm");
            case StmtKind::Try:
                throw VmCompileError("coba/tangkap belum didukung mode --vm");
            case StmtKind::Throw:
                throw VmCompileError("lempar belum didukung mode --vm");
        }
    }

    void compileBlock(const BlockStmt* block) {
        for (auto& st : block->statements) compileStmt(st.get(), false);
    }

    FnCompiler* topCompiler = nullptr;

    void compileProgram(const Program& prog) {
        for (auto& st : prog.statements) {
            if (st->kind == StmtKind::FnDecl) {
                userFunctions.insert(static_cast<const FnDeclStmt*>(st.get())->name);
            }
        }
        VmFunction* top = newFunction("<script>");
        program.topLevel = top;
        FnCompiler fc(nullptr, top);
        current = &fc;
        topCompiler = &fc;
        findCapturedNames(prog.statements, fc.capturedNames);
        for (auto& st : prog.statements) compileStmt(st.get(), true);
        current->emitOp(Op::ReturnNull);
    }
};

}  // namespace

std::unique_ptr<VmProgram> vmCompile(const Program& program) {
    auto vp = std::make_unique<VmProgram>();
    Compiler c(*vp);
    c.compileProgram(program);
    return vp;
}

namespace {

std::shared_ptr<Function> vmLookupMethod(const std::shared_ptr<ClassInfo>& start, const std::string& name,
                                          std::shared_ptr<ClassInfo>* ownerOut = nullptr) {
    for (std::shared_ptr<ClassInfo> c = start; c; c = c->parent) {
        auto it = c->methods.find(name);
        if (it != c->methods.end()) {
            if (ownerOut) *ownerOut = c;
            return it->second;
        }
    }
    return nullptr;
}

// Same lookup interpreter.cpp's indexGet() does for Array/Map/String/
// Instance -- kept in sync by hand since VmArray needs a distinct
// numeric/boxed fast path indexGet doesn't have.
Value vmGetIndex(const Value& target, const Value& idxv) {
    if (target.type == ValueType::VmArray) {
        if (idxv.type != ValueType::Number) throw VmRuntimeError("Index larik harus angka");
        long long i = static_cast<long long>(idxv.number);
        VmArrayState& st = *target.vmArray();
        size_t size = st.numeric ? st.nums.size() : st.boxed->size();
        if (i < 0 || static_cast<size_t>(i) >= size) throw VmRuntimeError("Index larik di luar batas: " + std::to_string(i));
        return st.numeric ? Value::fromNumber(st.nums[static_cast<size_t>(i)]) : (*st.boxed)[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Array) {
        if (idxv.type != ValueType::Number) throw VmRuntimeError("Index larik harus angka");
        long long i = static_cast<long long>(idxv.number);
        auto arr = target.arrayShared();
        if (i < 0 || static_cast<size_t>(i) >= arr->size()) throw VmRuntimeError("Index larik di luar batas: " + std::to_string(i));
        return (*arr)[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Map) {
        if (idxv.type != ValueType::String) throw VmRuntimeError("Index peta harus teks");
        auto m = target.mapShared();
        auto it = m->find(idxv.str());
        return it != m->end() ? it->second : Value::null();
    }
    if (target.type == ValueType::String) {
        if (idxv.type != ValueType::Number) throw VmRuntimeError("Index teks harus angka");
        long long i = static_cast<long long>(idxv.number);
        const std::string& str = target.str();
        if (i < 0 || static_cast<size_t>(i) >= str.size()) throw VmRuntimeError("Index teks di luar batas: " + std::to_string(i));
        return Value::fromString(std::string(1, str[static_cast<size_t>(i)]));
    }
    if (target.type == ValueType::Instance) {
        if (idxv.type != ValueType::String) throw VmRuntimeError("Kunci objek harus teks");
        auto fit = target.instance()->fields->find(idxv.str());
        if (fit != target.instance()->fields->end()) return fit->second;
        auto method = vmLookupMethod(target.instance()->classInfo, idxv.str());
        if (method) return Value::fromFunction(method);
        return Value::null();
    }
    throw VmRuntimeError("Tipe '" + std::string(target.typeName()) + "' nggak bisa di-index pakai []");
}

struct VmContext {
    std::unordered_map<std::string, Value> stringInterns;
    std::vector<NativeLoopDesc>* nativeLoops = nullptr;
    Interpreter* interpreter = nullptr;
    std::vector<std::unique_ptr<VmFunction>>* functions = nullptr;
    Environment* globals = nullptr;
    int depth = 0;
};

Value runFrame(const VmFunction* fn, VmClosure* closure, std::vector<Value> locals,
               std::vector<Cell*> boxedLocals, VmContext& ctx);

Value callValue(const Value& callee, std::vector<Value>& args, VmContext& ctx) {
    if (callee.type == ValueType::Builtin) {
        if (!ctx.interpreter) throw VmRuntimeError("Fungsi '" + callee.builtinName() + "' belum di-support murni di VM");
        return ctx.interpreter->callBuiltin(callee.builtinName(), args);
    }
    if (callee.type != ValueType::VmFn) {
        if (!ctx.interpreter) throw VmRuntimeError("Manggil fungsi non-VM butuh interpreter context");
        try {
            return ctx.interpreter->callValue(callee, args, Span{0, 0, 0});
        } catch (const RuntimeError& e) {
            throw VmRuntimeError(e.what());
        }
    }
    VmClosure* closure = callee.vmClosure();
    const VmFunction* fn = closure->function;
    if (static_cast<int>(args.size()) != fn->arity) {
        throw VmRuntimeError("fungsi '" + fn->name + "' butuh " + std::to_string(fn->arity) + " argumen, dapat " +
                              std::to_string(args.size()));
    }
    if (fn->nativeCode) {
        bool allNumeric = true;
        for (auto& av : args) {
            if (av.type != ValueType::Number) {
                allNumeric = false;
                break;
            }
        }
        if (allNumeric) {
            double result = 0.0;
            switch (fn->arity) {
                case 1: {
                    auto f = reinterpret_cast<double (*)(double)>(fn->nativeCode);
                    result = f(args[0].number);
                    break;
                }
                case 2: {
                    auto f = reinterpret_cast<double (*)(double, double)>(fn->nativeCode);
                    result = f(args[0].number, args[1].number);
                    break;
                }
                case 3: {
                    auto f = reinterpret_cast<double (*)(double, double, double)>(fn->nativeCode);
                    result = f(args[0].number, args[1].number, args[2].number);
                    break;
                }
                case 4: {
                    auto f = reinterpret_cast<double (*)(double, double, double, double)>(fn->nativeCode);
                    result = f(args[0].number, args[1].number, args[2].number, args[3].number);
                    break;
                }
                default:
                    allNumeric = false;
            }
            if (allNumeric) return Value::fromNumber(result);
        }
        // Fall through: non-numeric args, or arity above 4 (can't happen).
    }
    std::vector<Value> locals(static_cast<size_t>(fn->numLocals));
    std::vector<Cell*> boxedLocals(static_cast<size_t>(fn->numBoxedLocals));
    for (size_t i = 0; i < args.size(); i++) {
        const ParamSlot& ps = fn->paramSlots[i];
        if (ps.boxed) boxedLocals[static_cast<size_t>(ps.slot)] = GC::instance().allocCell(std::move(args[i]));
        else locals[static_cast<size_t>(ps.slot)] = std::move(args[i]);
    }
    return runFrame(fn, closure, std::move(locals), std::move(boxedLocals), ctx);
}

Value runFrame(const VmFunction* fn, VmClosure* closure, std::vector<Value> locals,
               std::vector<Cell*> boxedLocals, VmContext& ctx) {
    if (++ctx.depth > 3000) {
        ctx.depth--;
        throw VmRuntimeError("rekursi kelewat dalam mode --vm");
    }
    struct DepthPop {
        VmContext& c;
        ~DepthPop() { c.depth--; }
    } depthPop{ctx};

    std::vector<Value> stack;
    stack.reserve(16);
    size_t ip = 0;
    const std::vector<uint8_t>& code = fn->code;

    // Roots this frame's stack/locals/boxedLocals for its whole lifetime --
    // see GC::pushVmRoots in gc.hpp.
    VmRootGuard vmRootGuard(GC::VmFrameRoots{&stack, &locals, &boxedLocals});

    auto readByte = [&]() -> uint8_t { return code[ip++]; };
    auto readU16 = [&]() -> uint16_t {
        uint16_t v = static_cast<uint16_t>(code[ip] | (code[ip + 1] << 8));
        ip += 2;
        return v;
    };
    auto pop = [&]() -> Value {
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    // The VM loop otherwise has no GC safepoint at all (unlike execBlock's
    // per-statement check) -- checked every 64 ops, cheap until the
    // allocation threshold is actually crossed.
    uint32_t opsSinceGcCheck = 0;

    while (true) {
        if ((++opsSinceGcCheck & 0x3F) == 0) GC::instance().collectIfNeeded();
        Op op = static_cast<Op>(readByte());
        switch (op) {
            case Op::Const: stack.push_back(fn->constants[readU16()]); break;
            case Op::Null: stack.push_back(Value::null()); break;
            case Op::True: stack.push_back(Value::fromBool(true)); break;
            case Op::False: stack.push_back(Value::fromBool(false)); break;
            case Op::Pop: stack.pop_back(); break;
            case Op::Neg: {
                Value v = pop();
                if (v.type != ValueType::Number) throw VmRuntimeError("Operand '-' harus angka");
                stack.push_back(Value::fromNumber(-v.number));
                break;
            }
            case Op::Not: {
                Value v = pop();
                stack.push_back(Value::fromBool(!v.truthy()));
                break;
            }
            case Op::Add: {
                Value b = pop();
                Value a = pop();
                if (a.type == ValueType::Number && b.type == ValueType::Number) {
                    stack.push_back(Value::fromNumber(a.number + b.number));
                } else if (a.type == ValueType::String && b.type == ValueType::String) {
                    stack.push_back(Value::fromString(a.str() + b.str()));
                } else {
                    throw VmRuntimeError("Operand '+' harus dua angka atau dua teks");
                }
                break;
            }
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod: {
                Value b = pop();
                Value a = pop();
                if (a.type != ValueType::Number || b.type != ValueType::Number) {
                    throw VmRuntimeError("Operand aritmetika harus angka");
                }
                double r = 0;
                if (op == Op::Sub) r = a.number - b.number;
                else if (op == Op::Mul) r = a.number * b.number;
                else if (op == Op::Div) r = a.number / b.number;
                else r = std::fmod(a.number, b.number);
                stack.push_back(Value::fromNumber(r));
                break;
            }
            case Op::Eq: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value::fromBool(a.type == b.type &&
                                                 ((a.type == ValueType::Number && a.number == b.number) ||
                                                  (a.type == ValueType::String && a.str() == b.str()) ||
                                                  (a.type == ValueType::Bool && a.boolean() == b.boolean()) ||
                                                  (a.type == ValueType::Null))));
                break;
            }
            case Op::Neq: {
                Value b = pop();
                Value a = pop();
                bool eq = a.type == b.type &&
                          ((a.type == ValueType::Number && a.number == b.number) ||
                           (a.type == ValueType::String && a.str() == b.str()) ||
                           (a.type == ValueType::Bool && a.boolean() == b.boolean()) || (a.type == ValueType::Null));
                stack.push_back(Value::fromBool(!eq));
                break;
            }
            case Op::Lt:
            case Op::Lte:
            case Op::Gt:
            case Op::Gte: {
                Value b = pop();
                Value a = pop();
                bool r;
                if (a.type == ValueType::Number && b.type == ValueType::Number) {
                    if (op == Op::Lt) r = a.number < b.number;
                    else if (op == Op::Lte) r = a.number <= b.number;
                    else if (op == Op::Gt) r = a.number > b.number;
                    else r = a.number >= b.number;
                } else if (a.type == ValueType::String && b.type == ValueType::String) {
                    if (op == Op::Lt) r = a.str() < b.str();
                    else if (op == Op::Lte) r = a.str() <= b.str();
                    else if (op == Op::Gt) r = a.str() > b.str();
                    else r = a.str() >= b.str();
                } else {
                    throw VmRuntimeError("Operand perbandingan harus dua angka atau dua teks");
                }
                stack.push_back(Value::fromBool(r));
                break;
            }
            case Op::GetLocal: {
                uint16_t idx = readU16();
                stack.push_back(locals[idx]);
                break;
            }
            case Op::SetLocal: {
                uint16_t idx = readU16();
                locals[idx] = stack.back();
                break;
            }
            case Op::DefineLocal: {
                uint16_t idx = readU16();
                locals[idx] = pop();
                break;
            }
            case Op::GetBoxedLocal: {
                uint16_t idx = readU16();
                stack.push_back(boxedLocals[idx]->value);
                break;
            }
            case Op::SetBoxedLocal: {
                uint16_t idx = readU16();
                boxedLocals[idx]->value = stack.back();
                break;
            }
            case Op::DefineBoxedLocal: {
                uint16_t idx = readU16();
                boxedLocals[idx] = GC::instance().allocCell(pop());
                break;
            }
            case Op::GetUpvalue: {
                uint16_t idx = readU16();
                stack.push_back(closure->upvalues[idx]->value);
                break;
            }
            case Op::SetUpvalue: {
                uint16_t idx = readU16();
                closure->upvalues[idx]->value = stack.back();
                break;
            }
            case Op::GetGlobal: {
                uint16_t idx = readU16();
                const std::string& name = fn->constants[idx].str();
                Value* slot = ctx.globals ? ctx.globals->find(name) : nullptr;
                if (!slot) throw VmRuntimeError("Undefined variable '" + name + "'");
                stack.push_back(*slot);
                break;
            }
            case Op::SetGlobal: {
                uint16_t idx = readU16();
                const std::string& name = fn->constants[idx].str();
                Value* slot = ctx.globals ? ctx.globals->find(name) : nullptr;
                if (!slot) throw VmRuntimeError("Undefined variable '" + name + "'");
                *slot = stack.back();
                break;
            }
            case Op::DefineGlobal: {
                uint16_t idx = readU16();
                const std::string& name = fn->constants[idx].str();
                if (ctx.globals) ctx.globals->define(name, pop());
                else pop();
                break;
            }
            case Op::JumpIfFalse: {
                uint16_t offset = readU16();
                Value c = pop();
                if (!c.truthy()) ip += offset;
                break;
            }
            case Op::Jump: {
                uint16_t offset = readU16();
                ip += offset;
                break;
            }
            case Op::JumpIfFalseKeep: {
                uint16_t offset = readU16();
                if (!stack.back().truthy()) ip += offset;
                break;
            }
            case Op::JumpIfTrueKeep: {
                uint16_t offset = readU16();
                if (stack.back().truthy()) ip += offset;
                break;
            }
            case Op::Loop: {
                uint16_t offset = readU16();
                ip -= offset;
                break;
            }
            case Op::Call: {
                uint8_t argCount = readByte();
                // Fast path: JIT-compiled all-numeric function called directly
                // off the stack, zero heap allocation. Any mismatch (type,
                // arity) falls through to the general callValue() path.
                if (argCount >= 1 && argCount <= 4 && stack.size() >= static_cast<size_t>(argCount) + 1) {
                    Value& calleeSlot = stack[stack.size() - argCount - 1];
                    if (calleeSlot.type == ValueType::VmFn) {
                        const VmFunction* fn = calleeSlot.vmClosure()->function;
                        if (fn->nativeCode && fn->arity == argCount) {
                            size_t base = stack.size() - argCount;
                            bool allNumeric = true;
                            for (size_t i = 0; i < static_cast<size_t>(argCount); i++) {
                                if (stack[base + i].type != ValueType::Number) {
                                    allNumeric = false;
                                    break;
                                }
                            }
                            if (allNumeric) {
                                double a0 = stack[base].number;
                                double a1 = argCount > 1 ? stack[base + 1].number : 0.0;
                                double a2 = argCount > 2 ? stack[base + 2].number : 0.0;
                                double a3 = argCount > 3 ? stack[base + 3].number : 0.0;
                                double result = 0.0;
                                switch (argCount) {
                                    case 1: result = reinterpret_cast<double (*)(double)>(fn->nativeCode)(a0); break;
                                    case 2: result = reinterpret_cast<double (*)(double, double)>(fn->nativeCode)(a0, a1); break;
                                    case 3: result = reinterpret_cast<double (*)(double, double, double)>(fn->nativeCode)(a0, a1, a2); break;
                                    case 4: result = reinterpret_cast<double (*)(double, double, double, double)>(fn->nativeCode)(a0, a1, a2, a3); break;
                                }
                                stack.resize(base - 1);
                                stack.push_back(Value::fromNumber(result));
                                break;
                            }
                        }
                    }
                }
                std::vector<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; i--) args[static_cast<size_t>(i)] = pop();
                Value callee = pop();
                Value result = callValue(callee, args, ctx);
                stack.push_back(result);
                break;
            }
            case Op::CallMethod: {
                uint8_t argCount = readByte();
                std::vector<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; i--) args[static_cast<size_t>(i)] = pop();
                Value idxv = pop();
                Value target = pop();
                // Mirrors interpreter.cpp: a plain GetIndex+Call would hand
                // back an unbound method (no `ini`).
                if (target.type == ValueType::Instance) {
                    if (idxv.type != ValueType::String) throw VmRuntimeError("Kunci objek harus teks");
                    std::shared_ptr<ClassInfo> owner;
                    auto method = vmLookupMethod(target.instance()->classInfo, idxv.str(), &owner);
                    if (method) {
                        if (!ctx.interpreter) throw VmRuntimeError("Manggil metode butuh interpreter context");
                        try {
                            stack.push_back(ctx.interpreter->callFunction(method, args, Span{0, 0, 0}, &target, owner));
                        } catch (const RuntimeError& e) {
                            throw VmRuntimeError(e.what());
                        }
                    } else {
                        auto fit = target.instance()->fields->find(idxv.str());
                        Value fieldVal = fit != target.instance()->fields->end() ? fit->second : Value::null();
                        stack.push_back(callValue(fieldVal, args, ctx));
                    }
                } else {
                    Value callee = vmGetIndex(target, idxv);
                    stack.push_back(callValue(callee, args, ctx));
                }
                break;
            }
            case Op::MakeClosure: {
                uint16_t funcIdx = readU16();
                const VmFunction* target = (*ctx.functions)[funcIdx].get();
                auto vc = std::make_shared<VmClosure>();
                vc->function = target;
                for (size_t i = 0; i < target->upvalues.size(); i++) {
                    uint8_t isLocal = readByte();
                    uint16_t index = readU16();
                    if (isLocal) {
                        vc->upvalues.push_back(boxedLocals[index]);
                    } else {
                        vc->upvalues.push_back(closure->upvalues[index]);
                    }
                }
                stack.push_back(Value::fromVmClosure(vc));
                break;
            }
            case Op::NewArray: {
                uint16_t count = readU16();
                std::vector<Value> elems(count);
                for (int i = count - 1; i >= 0; i--) elems[static_cast<size_t>(i)] = pop();
                auto st = std::make_shared<VmArrayState>();
                bool allNum = true;
                for (auto& el : elems) {
                    if (el.type != ValueType::Number) {
                        allNum = false;
                        break;
                    }
                }
                if (allNum) {
                    st->numeric = true;
                    st->nums.reserve(elems.size());
                    for (auto& el : elems) st->nums.push_back(el.number);
                } else {
                    st->numeric = false;
                    st->boxed = std::make_shared<std::vector<Value>>(std::move(elems));
                }
                stack.push_back(Value::fromVmArray(st));
                break;
            }
            case Op::Import: {
                Value val = pop();
                if (val.type != ValueType::String) throw VmRuntimeError("impor(): butuh teks");
                if (!ctx.interpreter) throw VmRuntimeError("impor(): butuh interpreter context");
                try {
                    stack.push_back(ctx.interpreter->doImport(val.str()));
                } catch (const RuntimeError& e) {
                    throw VmRuntimeError(e.what());
                }
                break;
            }
            case Op::GetIndex: {
                Value idxv = pop();
                Value target = pop();
                stack.push_back(vmGetIndex(target, idxv));
                break;
            }
            case Op::SetIndex: {
                Value val = pop();
                Value idxv = pop();
                Value target = pop();
                if (target.type == ValueType::VmArray) {
                    if (idxv.type != ValueType::Number) throw VmRuntimeError("Index larik harus angka");
                    long long i = static_cast<long long>(idxv.number);
                    if (i < 0) throw VmRuntimeError("Index larik negatif nggak valid: " + std::to_string(i));
                    VmArrayState& st = *target.vmArray();
                    size_t size = st.numeric ? st.nums.size() : st.boxed->size();
                    bool needsGrow = static_cast<size_t>(i) >= size;
                    if (st.numeric && val.type == ValueType::Number && !needsGrow) {
                        st.nums[static_cast<size_t>(i)] = val.number;
                    } else {
                        if (st.numeric) {
                            st.boxed = std::make_shared<std::vector<Value>>();
                            st.boxed->reserve(st.nums.size());
                            for (double d : st.nums) st.boxed->push_back(Value::fromNumber(d));
                            st.numeric = false;
                            st.nums.clear();
                        }
                        if (needsGrow) {
                            st.boxed->resize(static_cast<size_t>(i) + 1, Value::null());
                        }
                        (*st.boxed)[static_cast<size_t>(i)] = val;
                    }
                } else if (target.type == ValueType::Array) {
                    if (idxv.type != ValueType::Number) throw VmRuntimeError("Index larik harus angka");
                    long long i = static_cast<long long>(idxv.number);
                    if (i < 0) throw VmRuntimeError("Index larik negatif nggak valid: " + std::to_string(i));
                    auto arr = target.arrayShared();
                    if (static_cast<size_t>(i) >= arr->size()) {
                        arr->resize(static_cast<size_t>(i) + 1, Value::null());
                    }
                    (*arr)[static_cast<size_t>(i)] = val;
                } else if (target.type == ValueType::Map) {
                    if (idxv.type != ValueType::String) throw VmRuntimeError("Index peta harus teks");
                    auto m = target.mapShared();
                    (*m)[idxv.str()] = val;
                } else if (target.type == ValueType::Instance) {
                    if (idxv.type != ValueType::String) throw VmRuntimeError("Kunci objek harus teks");
                    (*target.instance()->fields)[idxv.str()] = val;
                } else {
                    throw VmRuntimeError("Tipe '" + std::string(target.typeName()) + "' nggak bisa di-index pakai []");
                }
                stack.push_back(val);
                break;
            }
            case Op::Length: {
                Value v = pop();
                if (v.type == ValueType::VmArray) {
                    VmArrayState& st = *v.vmArray();
                    stack.push_back(Value::fromNumber(static_cast<double>(st.numeric ? st.nums.size() : st.boxed->size())));
                } else if (v.type == ValueType::Array) {
                    stack.push_back(Value::fromNumber(static_cast<double>(v.array()->size())));
                } else if (v.type == ValueType::Map) {
                    stack.push_back(Value::fromNumber(static_cast<double>(v.map()->size())));
                } else if (v.type == ValueType::String) {
                    stack.push_back(Value::fromNumber(static_cast<double>(v.str().size())));
                } else {
                    throw VmRuntimeError("panjang(): butuh teks, larik, atau peta mode --vm");
                }
                break;
            }
            case Op::Push: {
                Value val = pop();
                Value target = pop();
                if (target.type == ValueType::VmArray) {
                    VmArrayState& st = *target.vmArray();
                    if (st.numeric && val.type == ValueType::Number) {
                        st.nums.push_back(val.number);
                    } else {
                        if (st.numeric) {
                            st.boxed = std::make_shared<std::vector<Value>>();
                            st.boxed->reserve(st.nums.size());
                            for (double d : st.nums) st.boxed->push_back(Value::fromNumber(d));
                            st.numeric = false;
                            st.nums.clear();
                        }
                        st.boxed->push_back(val);
                    }
                    stack.push_back(Value::fromNumber(static_cast<double>(st.numeric ? st.nums.size() : st.boxed->size())));
                } else if (target.type == ValueType::Array) {
                    target.array()->push_back(val);
                    stack.push_back(Value::fromNumber(static_cast<double>(target.array()->size())));
                } else {
                    throw VmRuntimeError("tambah(): butuh larik mode --vm");
                }
                break;
            }
            case Op::Print: {
                uint8_t argCount = readByte();
                std::vector<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; i--) args[static_cast<size_t>(i)] = pop();
                for (size_t i = 0; i < args.size(); i++) {
                    if (i > 0) std::cout << " ";
                    std::cout << args[i].stringify();
                }
                std::cout << "\n";
                stack.push_back(Value::null());
                break;
            }
            case Op::TryNativeLoop: {
                uint16_t idx = readU16();
                NativeLoopDesc& d = (*ctx.nativeLoops)[idx];
                bool taken = false;
                std::vector<double*> bases;
                bases.reserve(d.arraySlots.size() + 1);
                bool preOk = true;
                for (size_t k = 0; preOk && k < d.arraySlots.size(); k++) {
                    Value& v = d.arrayBoxed[k] ? boxedLocals[static_cast<size_t>(d.arraySlots[k])]->value
                                                : locals[static_cast<size_t>(d.arraySlots[k])];
                    if (v.type != ValueType::VmArray || !v.vmArray()->numeric) preOk = false;
                    else bases.push_back(v.vmArray()->nums.data());
                }
                Value* accumVal = nullptr;
                Value* outVal = nullptr;
                // Whether the accumulator or the (non-literal) bound live
                // in a global slot rather than a local one -- see the
                // NativeLoopDesc::accumIsGlobal comment in vm.hpp for why
                // this also decides whether the GIL stays held below.
                bool touchesGlobal = false;
                if (preOk && !d.isMap) {
                    if (d.accumIsGlobal) {
                        accumVal = ctx.globals ? ctx.globals->find(d.accumGlobalName) : nullptr;
                        if (!accumVal) preOk = false;
                        else touchesGlobal = true;
                    } else {
                        accumVal = d.accumBoxed ? &boxedLocals[static_cast<size_t>(d.accumSlot)]->value
                                                 : &locals[static_cast<size_t>(d.accumSlot)];
                    }
                    if (preOk && accumVal->type != ValueType::Number) preOk = false;
                }
                if (preOk && d.isMap) {
                    outVal = d.outBoxed ? &boxedLocals[static_cast<size_t>(d.outSlot)]->value
                                         : &locals[static_cast<size_t>(d.outSlot)];
                    if (outVal->type != ValueType::VmArray || !outVal->vmArray()->numeric) preOk = false;
                }
                int64_t n = 0;
                if (preOk) {
                    if (d.boundIsLiteral) {
                        n = static_cast<int64_t>(d.boundLiteral);
                    } else if (d.boundIsGlobal) {
                        Value* bv = ctx.globals ? ctx.globals->find(d.boundGlobalName) : nullptr;
                        if (!bv || bv->type != ValueType::Number) preOk = false;
                        else {
                            n = static_cast<int64_t>(bv->number);
                            touchesGlobal = true;
                        }
                    } else {
                        Value& bv = d.boundBoxed ? boxedLocals[static_cast<size_t>(d.boundSlot)]->value
                                                  : locals[static_cast<size_t>(d.boundSlot)];
                        if (bv.type != ValueType::Number) preOk = false;
                        else n = static_cast<int64_t>(bv.number);
                    }
                }
                if (preOk && n < 0) preOk = false;
                if (preOk) {
                    for (size_t k = 0; preOk && k < d.arraySlots.size(); k++) {
                        Value& v = d.arrayBoxed[k] ? boxedLocals[static_cast<size_t>(d.arraySlots[k])]->value
                                                    : locals[static_cast<size_t>(d.arraySlots[k])];
                        if (static_cast<int64_t>(v.vmArray()->nums.size()) < n) preOk = false;
                    }
                    if (preOk && d.isMap && static_cast<int64_t>(outVal->vmArray()->nums.size()) < n) preOk = false;
                }
                if (preOk) {
                    if (d.isMap) {
                        bases.push_back(outVal->vmArray()->nums.data());
                        auto fn = reinterpret_cast<void (*)(double**, double*, int64_t)>(d.code);
                        {
                            GilRelease release;
                            fn(bases.data(), nullptr, n);
                        }
                    } else {
                        double accum = accumVal->number;
                        auto fn = reinterpret_cast<void (*)(double**, double*, int64_t)>(d.code);
                        if (touchesGlobal) {
                            // A global is visible to every goroutine -- releasing
                            // the GIL here would race another thread reassigning
                            // it concurrently with this raw double read/write.
                            fn(bases.data(), &accum, n);
                        } else {
                            GilRelease release;
                            fn(bases.data(), &accum, n);
                        }
                        *accumVal = Value::fromNumber(accum);
                    }
                    taken = true;
                }
                stack.push_back(Value::fromBool(!taken));
                break;
            }
            case Op::GoSpawn: {
                uint8_t argCount = readByte();
                std::vector<Value> allArgs(argCount);
                for (int i = argCount - 1; i >= 0; i--) allArgs[static_cast<size_t>(i)] = pop();
                if (allArgs.empty() || allArgs[0].type != ValueType::VmFn) {
                    throw VmRuntimeError("jalan(): argumen pertama harus fungsi");
                }
                Value fn = allArgs[0];
                std::vector<Value> goroutineArgs(allArgs.begin() + 1, allArgs.end());
                auto* interpreterPtr = ctx.interpreter;
                auto* functionsPtr = ctx.functions;
                auto* nativeLoopsPtr = ctx.nativeLoops;
                auto globalsSnapshot = ctx.globals;
                GC::liveGoroutines++;
                std::thread([fn, goroutineArgs, interpreterPtr, functionsPtr, nativeLoopsPtr, globalsSnapshot]() mutable {
                    Interpreter::registerCurrentThread();
                    GIL::instance().lock();
                    VmContext threadCtx;
                    threadCtx.interpreter = interpreterPtr;
                    threadCtx.functions = functionsPtr;
                    threadCtx.nativeLoops = nativeLoopsPtr;
                    threadCtx.globals = globalsSnapshot;
                    try {
                        callValue(fn, goroutineArgs, threadCtx);
                    } catch (const VmRuntimeError& e) {
                        std::cerr << "[jalan --vm] goroutine error: " << e.what() << "\n";
                    } catch (const std::exception& e) {
                        std::cerr << "[jalan --vm] goroutine error: " << e.what() << "\n";
                    }
                    Interpreter::unregisterCurrentThread();
                    GIL::instance().unlock();
                    GC::liveGoroutines--;
                }).detach();
                stack.push_back(Value::null());
                break;
            }
            case Op::ChanNew: {
                uint8_t argCount = readByte();
                Value capArg = argCount == 1 ? pop() : Value::null();
                auto chan = std::make_shared<ChannelState>();
                if (argCount == 1) {
                    if (capArg.type != ValueType::Number) throw VmRuntimeError("kanal_baru(): argumen harus angka");
                    if (capArg.number > 0) chan->capacity = static_cast<size_t>(capArg.number);
                }
                stack.push_back(Value::fromChannel(chan));
                break;
            }
            case Op::ChanSend: {
                Value v = pop();
                Value chanVal = pop();
                if (chanVal.type != ValueType::Channel) throw VmRuntimeError("kanal_kirim(): argumen ke-1 harus kanal");
                auto chan = chanVal.channelShared();
                {
                    GilRelease release;
                    std::unique_lock<std::mutex> lock(chan->mu);
                    if (chan->closed) throw VmRuntimeError("kanal_kirim(): nggak bisa kirim ke kanal yang udah ditutup");
                    if (chan->capacity > 0 && chan->queue.size() >= chan->capacity) {
                        chan->notFull.wait(lock, [&] { return chan->queue.size() < chan->capacity || chan->closed; });
                        if (chan->closed) throw VmRuntimeError("kanal_kirim(): kanal ditutup pas nunggu ngirim");
                    }
                    chan->queue.push_back(v);
                    lock.unlock();
                    chan->notEmpty.notify_one();
                }
                stack.push_back(Value::null());
                break;
            }
            case Op::ChanRecv: {
                Value chanVal = pop();
                if (chanVal.type != ValueType::Channel) throw VmRuntimeError("kanal_terima(): argumen harus kanal");
                auto chan = chanVal.channelShared();
                Value result = Value::null();
                {
                    GilRelease release;
                    std::unique_lock<std::mutex> lock(chan->mu);
                    if (chan->queue.empty() && !chan->closed) {
                        chan->notEmpty.wait(lock, [&] { return !chan->queue.empty() || chan->closed; });
                    }
                    if (!chan->queue.empty()) {
                        result = chan->queue.front();
                        chan->queue.pop_front();
                        lock.unlock();
                        chan->notFull.notify_one();
                    }
                }
                stack.push_back(result);
                break;
            }
            case Op::Return: {
                return pop();
            }
            case Op::ReturnNull: {
                return Value::null();
            }
        }
    }
}

}  // namespace

namespace {
// Set once by vmRun() below, never cleared -- one VM program per process.
struct ActiveVmState {
    std::vector<std::unique_ptr<VmFunction>>* functions = nullptr;
    std::vector<NativeLoopDesc>* nativeLoops = nullptr;
    Environment* globals = nullptr;
};
std::atomic<ActiveVmState*> g_activeVm{nullptr};
}  // namespace

Value vmCallValue(const Value& callee, std::vector<Value>& args, Interpreter* interpreter) {
    ActiveVmState* st = g_activeVm.load(std::memory_order_acquire);
    if (!st) {
        throw RuntimeError(i18n::tr("Manggil fungsi VM butuh program VM yang lagi jalan",
                                     "Calling a VM function requires a VM program to be running"));
    }
    VmContext ctx;
    ctx.functions = st->functions;
    ctx.nativeLoops = st->nativeLoops;
    ctx.globals = st->globals;
    ctx.interpreter = interpreter;
    try {
        return callValue(callee, args, ctx);
    } catch (const VmRuntimeError& e) {
        throw RuntimeError(e.what());
    } catch (const VmCompileError& e) {
        throw RuntimeError(e.what());
    }
}

int vmRun(VmProgram& program, Interpreter* interpreter) {
    if (!program.topLevel) return 0;
    VmContext ctx;
    ctx.functions = &program.functions;
    ctx.nativeLoops = &program.nativeLoops;
    ctx.interpreter = interpreter;
    if (interpreter) ctx.globals = interpreter->getGlobalsEnv();
    static ActiveVmState activeState;
    activeState.functions = ctx.functions;
    activeState.nativeLoops = ctx.nativeLoops;
    activeState.globals = ctx.globals;
    g_activeVm.store(&activeState, std::memory_order_release);

    VmClosure topClosure;
    topClosure.function = program.topLevel;
    std::vector<Value> locals(static_cast<size_t>(program.topLevel->numLocals));
    std::vector<Cell*> boxedLocals(static_cast<size_t>(program.topLevel->numBoxedLocals));
    try {
        Value result = runFrame(program.topLevel, &topClosure, std::move(locals), std::move(boxedLocals), ctx);
        (void)result;
        return 0;
    } catch (const VmCompileError& e) {
        std::cerr << "nusa --vm: error kompilasi: " << e.what() << "\n";
        return 1;
    } catch (const VmRuntimeError& e) {
        std::cerr << "nusa --vm: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// On-disk bytecode cache format. Only Number/String ever appear in
// VmFunction::constants. A program using the native-loop JIT is never
// cached (its machine-code pointer can't serialize) -- next run recompiles.
namespace {

constexpr uint32_t kCacheMagic = 0x4E534256; // "NSBV"
constexpr uint32_t kCacheVersion = 1;

void writeU32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeI32(std::ofstream& f, int32_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeU8(std::ofstream& f, uint8_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeDouble(std::ofstream& f, double v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeString(std::ofstream& f, const std::string& s) {
    writeU32(f, static_cast<uint32_t>(s.size()));
    if (!s.empty()) f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool readU32(std::ifstream& f, uint32_t& v) { f.read(reinterpret_cast<char*>(&v), sizeof(v)); return static_cast<bool>(f); }
bool readI32(std::ifstream& f, int32_t& v) { f.read(reinterpret_cast<char*>(&v), sizeof(v)); return static_cast<bool>(f); }
bool readU8(std::ifstream& f, uint8_t& v) { f.read(reinterpret_cast<char*>(&v), sizeof(v)); return static_cast<bool>(f); }
bool readDouble(std::ifstream& f, double& v) { f.read(reinterpret_cast<char*>(&v), sizeof(v)); return static_cast<bool>(f); }
bool readString(std::ifstream& f, std::string& s) {
    uint32_t len = 0;
    if (!readU32(f, len)) return false;
    s.resize(len);
    if (len > 0) f.read(&s[0], static_cast<std::streamsize>(len));
    return static_cast<bool>(f);
}

void writeFunction(std::ofstream& f, const VmFunction& fn) {
    writeString(f, fn.name);
    writeI32(f, fn.arity);
    writeI32(f, fn.numLocals);
    writeI32(f, fn.numBoxedLocals);
    writeU32(f, static_cast<uint32_t>(fn.paramSlots.size()));
    for (const auto& p : fn.paramSlots) {
        writeU8(f, p.boxed ? 1 : 0);
        writeI32(f, p.slot);
    }
    writeU32(f, static_cast<uint32_t>(fn.code.size()));
    if (!fn.code.empty()) f.write(reinterpret_cast<const char*>(fn.code.data()), static_cast<std::streamsize>(fn.code.size()));
    writeU32(f, static_cast<uint32_t>(fn.constants.size()));
    for (const auto& c : fn.constants) {
        if (c.type == ValueType::Number) {
            writeU8(f, 0);
            writeDouble(f, c.number);
        } else {
            writeU8(f, 1);
            writeString(f, c.str());
        }
    }
    writeU32(f, static_cast<uint32_t>(fn.upvalues.size()));
    for (const auto& uv : fn.upvalues) {
        writeU8(f, uv.isLocal ? 1 : 0);
        writeI32(f, uv.index);
    }
}

bool readFunction(std::ifstream& f, VmFunction& fn) {
    if (!readString(f, fn.name)) return false;
    if (!readI32(f, fn.arity)) return false;
    if (!readI32(f, fn.numLocals)) return false;
    if (!readI32(f, fn.numBoxedLocals)) return false;
    uint32_t nParams = 0;
    if (!readU32(f, nParams)) return false;
    fn.paramSlots.resize(nParams);
    for (auto& p : fn.paramSlots) {
        uint8_t boxed = 0;
        if (!readU8(f, boxed)) return false;
        p.boxed = boxed != 0;
        if (!readI32(f, p.slot)) return false;
    }
    uint32_t codeLen = 0;
    if (!readU32(f, codeLen)) return false;
    fn.code.resize(codeLen);
    if (codeLen > 0) {
        f.read(reinterpret_cast<char*>(fn.code.data()), static_cast<std::streamsize>(codeLen));
        if (!f) return false;
    }
    uint32_t nConst = 0;
    if (!readU32(f, nConst)) return false;
    fn.constants.reserve(nConst);
    for (uint32_t i = 0; i < nConst; i++) {
        uint8_t tag = 0;
        if (!readU8(f, tag)) return false;
        if (tag == 0) {
            double d = 0;
            if (!readDouble(f, d)) return false;
            fn.constants.push_back(Value::fromNumber(d));
        } else {
            std::string s;
            if (!readString(f, s)) return false;
            fn.constants.push_back(Value::fromString(std::move(s)));
        }
    }
    uint32_t nUp = 0;
    if (!readU32(f, nUp)) return false;
    fn.upvalues.resize(nUp);
    for (auto& uv : fn.upvalues) {
        uint8_t isLocal = 0;
        if (!readU8(f, isLocal)) return false;
        uv.isLocal = isLocal != 0;
        if (!readI32(f, uv.index)) return false;
    }
    return true;
}

} // namespace

void vmSerialize(const VmProgram& program, const std::string& path, const std::string& combinedHash) {
    if (!program.nativeLoops.empty()) {
        // Native-JIT loops hold raw machine code that can't be safely
        // written to disk and reloaded across process runs. Skip caching
        // this program entirely rather than risk deserializing garbage
        // into a function pointer.
        return;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    writeU32(f, kCacheMagic);
    writeU32(f, kCacheVersion);
    writeString(f, combinedHash);
    writeU32(f, static_cast<uint32_t>(program.functions.size()));
    int32_t topIdx = -1;
    for (size_t i = 0; i < program.functions.size(); i++) {
        if (program.functions[i].get() == program.topLevel) { topIdx = static_cast<int32_t>(i); break; }
    }
    writeI32(f, topIdx);
    for (const auto& fn : program.functions) {
        writeFunction(f, *fn);
    }
    if (!f) {
        // Write failed partway through (disk full, permissions, etc.) --
        // don't leave a truncated/corrupt cache file behind.
        f.close();
        std::remove(path.c_str());
    }
}

std::unique_ptr<VmProgram> vmDeserialize(const std::string& path, const std::string& expectedHash) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    uint32_t magic = 0, version = 0;
    if (!readU32(f, magic) || magic != kCacheMagic) return nullptr;
    if (!readU32(f, version) || version != kCacheVersion) return nullptr;
    std::string hash;
    if (!readString(f, hash) || hash != expectedHash) return nullptr;
    uint32_t nFn = 0;
    if (!readU32(f, nFn)) return nullptr;
    int32_t topIdx = -1;
    if (!readI32(f, topIdx)) return nullptr;
    auto program = std::make_unique<VmProgram>();
    program->functions.reserve(nFn);
    for (uint32_t i = 0; i < nFn; i++) {
        auto fn = std::make_unique<VmFunction>();
        if (!readFunction(f, *fn)) return nullptr;
        program->functions.push_back(std::move(fn));
    }
    if (topIdx < 0 || static_cast<uint32_t>(topIdx) >= program->functions.size()) return nullptr;
    program->topLevel = program->functions[static_cast<size_t>(topIdx)].get();
    return program;
}

void vmAttachNativeFunctions(VmProgram& program, const Program& sourceAst) {
    if (jitDisabled()) return;
    // Only attach when the name is unambiguous in both the AST and the
    // deserialized VmFunctions -- any collision is skipped rather than
    // risk attaching the wrong function's compiled body.
    std::unordered_map<std::string, const FnDeclStmt*> declByName;
    std::unordered_map<std::string, int> astNameCount;
    for (auto& st : sourceAst.statements) {
        if (st->kind != StmtKind::FnDecl) continue;
        auto* d = static_cast<const FnDeclStmt*>(st.get());
        astNameCount[d->name]++;
        declByName[d->name] = d;
    }
    std::unordered_map<std::string, int> fnNameCount;
    for (auto& fn : program.functions) fnNameCount[fn->name]++;

    for (auto& fn : program.functions) {
        if (fn.get() == program.topLevel) continue;
        if (fnNameCount[fn->name] != 1) continue;
        auto it = declByName.find(fn->name);
        if (it == declByName.end() || astNameCount[fn->name] != 1) continue;
        const FnDeclStmt* decl = it->second;
        if (static_cast<int>(decl->params.size()) != fn->arity) continue;
        JitFuncResult jf = tryCompileNativeFunc(decl);
        if (jf.ok) {
            fn->nativeCode = jf.code;
            program.nativeFuncLiteralPools.push_back(std::move(jf.literalPool));
        }
    }
}

