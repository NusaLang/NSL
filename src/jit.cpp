#include "jit.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <sys/mman.h>

#include <cstring>
#include <unordered_map>

namespace {

struct Asm {
    std::vector<uint8_t> buf;

    void b(uint8_t x) { buf.push_back(x); }

    void modrm(int mod, int reg, int rm) {
        b(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }
    void sib(int scale, int index, int base) {
        b(static_cast<uint8_t>((scale << 6) | ((index & 7) << 3) | (base & 7)));
    }

    void xorSelf32(int r) {
        b(0x31);
        modrm(3, r, r);
    }

    void movLoadDisp8(int dstGp, int baseGp, int8_t disp) {
        b(0x48);
        b(0x8B);
        modrm(1, dstGp, baseGp);
        b(static_cast<uint8_t>(disp));
    }

    void addImm8(int r, int8_t imm) {
        b(0x48);
        b(0x83);
        modrm(3, 0, r);
        b(static_cast<uint8_t>(imm));
    }

    void cmpRR(int lhs, int rhs) {
        b(0x48);
        b(0x39);
        modrm(3, rhs, lhs);
    }

    int jgePlaceholder() {
        b(0x0F);
        b(0x8D);
        size_t pos = buf.size();
        b(0);
        b(0);
        b(0);
        b(0);
        return static_cast<int>(pos);
    }

    void patchRel32(int patchPos) {
        int32_t rel = static_cast<int32_t>(buf.size() - static_cast<size_t>(patchPos) - 4);
        buf[static_cast<size_t>(patchPos)] = static_cast<uint8_t>(rel & 0xFF);
        buf[static_cast<size_t>(patchPos) + 1] = static_cast<uint8_t>((rel >> 8) & 0xFF);
        buf[static_cast<size_t>(patchPos) + 2] = static_cast<uint8_t>((rel >> 16) & 0xFF);
        buf[static_cast<size_t>(patchPos) + 3] = static_cast<uint8_t>((rel >> 24) & 0xFF);
    }

    void jmpBackTo(size_t targetPos) {
        b(0xE9);
        size_t patchPos = buf.size();
        b(0);
        b(0);
        b(0);
        b(0);
        int32_t rel =
            static_cast<int32_t>(static_cast<int64_t>(targetPos) - static_cast<int64_t>(patchPos + 4));
        buf[patchPos] = static_cast<uint8_t>(rel & 0xFF);
        buf[patchPos + 1] = static_cast<uint8_t>((rel >> 8) & 0xFF);
        buf[patchPos + 2] = static_cast<uint8_t>((rel >> 16) & 0xFF);
        buf[patchPos + 3] = static_cast<uint8_t>((rel >> 24) & 0xFF);
    }

    void ret() { b(0xC3); }

    void movsdLoadIdx8(int xmmDst, int baseGp, int idxGp) {
        b(0xF2);
        b(0x0F);
        b(0x10);
        modrm(0, xmmDst, 4);
        sib(3, idxGp, baseGp);
    }
    void movsdLoadDisp0(int xmmDst, int baseGp) {
        b(0xF2);
        b(0x0F);
        b(0x10);
        modrm(0, xmmDst, baseGp);
    }
    void movsdStoreDisp0(int baseGp, int xmmSrc) {
        b(0xF2);
        b(0x0F);
        b(0x11);
        modrm(0, xmmSrc, baseGp);
    }
    void movsdStoreIdx8(int baseGp, int idxGp, int xmmSrc) {
        b(0xF2);
        b(0x0F);
        b(0x11);
        modrm(0, xmmSrc, 4);
        sib(3, idxGp, baseGp);
    }

    void movabs64(int gp, uint64_t imm) {
        b(0x48);
        b(static_cast<uint8_t>(0xB8 + (gp & 7)));
        for (int i = 0; i < 8; i++) b(static_cast<uint8_t>((imm >> (8 * i)) & 0xFF));
    }

    void movapd(int dst, int src) {
        b(0x66);
        b(0x0F);
        b(0x28);
        modrm(3, dst, src);
    }

    void xorpd(int dst, int src) {
        b(0x66);
        b(0x0F);
        b(0x57);
        modrm(3, dst, src);
    }

    void subsdRR(int dst, int src) { arithSd(0x5C, dst, src); }

    // cvtsi2sd xmmDst, r64Src -- convert a 64-bit signed integer GPR to
    // a double in an xmm register. Used to materialize the (integer) loop
    // counter as a double once per iteration.
    void cvtsi2sd(int xmmDst, int gpSrc) {
        b(0xF2);
        b(0x48);
        b(0x0F);
        b(0x2A);
        modrm(3, xmmDst, gpSrc);
    }

    void arithSd(uint8_t opcode, int dst, int src) {
        b(0xF2);
        b(0x0F);
        b(opcode);
        modrm(3, dst, src);
    }
    void addsd(int dst, int src) { arithSd(0x58, dst, src); }
    void subsd(int dst, int src) { arithSd(0x5C, dst, src); }
    void mulsd(int dst, int src) { arithSd(0x59, dst, src); }
    void divsd(int dst, int src) { arithSd(0x5E, dst, src); }
};

enum GpReg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSI = 6, RDI = 7 };

struct DetectCtx {
    std::string counterVar;
    std::vector<std::string> arrayOrder;
    std::unordered_map<std::string, int> arrayReg;
    bool ok = true;
};

bool isCounterIndexedRead(const Expr* e, const std::string& counterVar, std::string* arrName) {
    if (e->kind != ExprKind::Index) return false;
    auto* idx = static_cast<const IndexExpr*>(e);
    if (idx->target->kind != ExprKind::Identifier) return false;
    if (idx->index->kind != ExprKind::Identifier) return false;
    if (static_cast<const IdentifierExpr*>(idx->index.get())->name != counterVar) return false;
    *arrName = static_cast<const IdentifierExpr*>(idx->target.get())->name;
    return true;
}

bool walkExprTree(const Expr* e, const std::string& counterVar, std::vector<std::string>& arrays) {
    if (!e) return false;
    std::string arrName;
    if (isCounterIndexedRead(e, counterVar, &arrName)) {
        bool seen = false;
        for (auto& a : arrays) {
            if (a == arrName) {
                seen = true;
                break;
            }
        }
        if (!seen) arrays.push_back(arrName);
        return true;
    }
    if (e->kind == ExprKind::Binary) {
        auto* be = static_cast<const BinaryExpr*>(e);
        if (be->op != "+" && be->op != "-" && be->op != "*" && be->op != "/") return false;
        return walkExprTree(be->left.get(), counterVar, arrays) &&
               walkExprTree(be->right.get(), counterVar, arrays);
    }
    return false;
}

int compileExprNative(Asm& a, const Expr* e, const std::string& counterVar,
                       const std::unordered_map<std::string, int>& arrayReg, int freeXmm) {
    std::string arrName;
    if (isCounterIndexedRead(e, counterVar, &arrName)) {
        int gp = arrayReg.at(arrName);
        a.movsdLoadIdx8(freeXmm, gp, RCX);
        return freeXmm;
    }
    auto* be = static_cast<const BinaryExpr*>(e);
    int lx = compileExprNative(a, be->left.get(), counterVar, arrayReg, freeXmm);
    int rx = compileExprNative(a, be->right.get(), counterVar, arrayReg, freeXmm + 1);
    if (be->op == "+") a.addsd(lx, rx);
    else if (be->op == "-") a.subsd(lx, rx);
    else if (be->op == "*") a.mulsd(lx, rx);
    else a.divsd(lx, rx);
    return lx;
}

// ---- narrow function-call JIT ---------------------------------------

// Returns the xmm register holding the computed value, or -1 on failure
// (expression outside the supported grammar, or register pressure
// exceeded the small fixed pool below). Grammar: +,-,*,/ and unary '-'
// over parameter identifiers and numeric literals. Nothing else --
// no calls, no indexing, no comparisons, no globals.
int compileFuncExpr(Asm& a, const Expr* e, const std::unordered_map<std::string, int>& paramReg,
                     std::vector<std::unique_ptr<double>>& literalPool, int freeXmm, int maxXmm) {
    if (freeXmm > maxXmm) return -1;
    if (e->kind == ExprKind::Identifier) {
        auto* id = static_cast<const IdentifierExpr*>(e);
        auto it = paramReg.find(id->name);
        if (it == paramReg.end()) return -1;
        a.movapd(freeXmm, it->second);
        return freeXmm;
    }
    if (e->kind == ExprKind::Literal) {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->litKind != LiteralExpr::Kind::Number) return -1;
        literalPool.push_back(std::make_unique<double>(lit->number));
        a.movabs64(RAX, reinterpret_cast<uint64_t>(literalPool.back().get()));
        a.movsdLoadDisp0(freeXmm, RAX);
        return freeXmm;
    }
    if (e->kind == ExprKind::Unary) {
        auto* u = static_cast<const UnaryExpr*>(e);
        if (u->op != "-") return -1;
        int r = compileFuncExpr(a, u->operand.get(), paramReg, literalPool, freeXmm, maxXmm);
        if (r < 0) return -1;
        a.xorpd(freeXmm + 1 <= maxXmm ? freeXmm + 1 : freeXmm, freeXmm + 1 <= maxXmm ? freeXmm + 1 : freeXmm);
        // zero - r  (use a scratch reg one above r if available, else reuse r via 0 - r trick)
        int zeroReg = (freeXmm + 1 <= maxXmm) ? freeXmm + 1 : -1;
        if (zeroReg < 0) return -1;
        a.subsd(zeroReg, r);
        return zeroReg;
    }
    if (e->kind != ExprKind::Binary) return -1;
    auto* be = static_cast<const BinaryExpr*>(e);
    if (be->op != "+" && be->op != "-" && be->op != "*" && be->op != "/") return -1;
    int lx = compileFuncExpr(a, be->left.get(), paramReg, literalPool, freeXmm, maxXmm);
    if (lx < 0) return -1;
    int rx = compileFuncExpr(a, be->right.get(), paramReg, literalPool, freeXmm + 1, maxXmm);
    if (rx < 0) return -1;
    if (be->op == "+") a.addsd(lx, rx);
    else if (be->op == "-") a.subsd(lx, rx);
    else if (be->op == "*") a.mulsd(lx, rx);
    else a.divsd(lx, rx);
    return lx;
}

void* finalizeExecutable(const std::vector<uint8_t>& code) {
    size_t pageSize = 4096;
    size_t size = ((code.size() + pageSize - 1) / pageSize) * pageSize;
    if (size == 0) size = pageSize;
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    std::memcpy(mem, code.data(), code.size());
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, size);
        return nullptr;
    }
    return mem;
}

}  // namespace

JitLoopResult tryCompileNativeLoop(const ForStmt* forNode) {
    JitLoopResult r;
    if (!forNode->init || forNode->init->kind != StmtKind::Let) return r;
    auto* letInit = static_cast<const LetStmt*>(forNode->init.get());
    if (!letInit->value || letInit->value->kind != ExprKind::Literal) return r;
    auto* initLit = static_cast<const LiteralExpr*>(letInit->value.get());
    if (initLit->litKind != LiteralExpr::Kind::Number || initLit->number != 0.0) return r;
    std::string counterVar = letInit->name;

    if (!forNode->condition || forNode->condition->kind != ExprKind::Binary) return r;
    auto* cond = static_cast<const BinaryExpr*>(forNode->condition.get());
    if (cond->op != "<") return r;
    if (cond->left->kind != ExprKind::Identifier) return r;
    if (static_cast<const IdentifierExpr*>(cond->left.get())->name != counterVar) return r;
    bool boundIsLiteral = false;
    double boundLiteral = 0;
    std::string boundVar;
    if (cond->right->kind == ExprKind::Literal) {
        auto* lit = static_cast<const LiteralExpr*>(cond->right.get());
        if (lit->litKind != LiteralExpr::Kind::Number) return r;
        boundIsLiteral = true;
        boundLiteral = lit->number;
    } else if (cond->right->kind == ExprKind::Identifier) {
        boundVar = static_cast<const IdentifierExpr*>(cond->right.get())->name;
    } else {
        return r;
    }

    if (!forNode->post || forNode->post->kind != ExprKind::Assign) return r;
    auto* postAssign = static_cast<const AssignExpr*>(forNode->post.get());
    if (postAssign->name != counterVar) return r;
    if (postAssign->value->kind != ExprKind::Binary) return r;
    auto* postBin = static_cast<const BinaryExpr*>(postAssign->value.get());
    if (postBin->op != "+") return r;
    if (postBin->left->kind != ExprKind::Identifier ||
        static_cast<const IdentifierExpr*>(postBin->left.get())->name != counterVar)
        return r;
    if (postBin->right->kind != ExprKind::Literal) return r;
    auto* stepLit = static_cast<const LiteralExpr*>(postBin->right.get());
    if (stepLit->litKind != LiteralExpr::Kind::Number || stepLit->number != 1.0) return r;

    if (!forNode->body || forNode->body->statements.size() != 1) return r;
    const Stmt* onlyStmt = forNode->body->statements[0].get();
    if (onlyStmt->kind != StmtKind::ExprStmt) return r;
    const Expr* inner = static_cast<const ExprStmtNode*>(onlyStmt)->expr.get();

    bool isMap = false;
    std::string accumVar, outVar;
    const Expr* rhs = nullptr;
    bool reduceIsAdd = true;

    if (inner->kind == ExprKind::Assign) {
        auto* asg = static_cast<const AssignExpr*>(inner);
        if (asg->value->kind != ExprKind::Binary) return r;
        auto* bin = static_cast<const BinaryExpr*>(asg->value.get());
        if (bin->op != "+" && bin->op != "-") return r;
        if (bin->left->kind != ExprKind::Identifier ||
            static_cast<const IdentifierExpr*>(bin->left.get())->name != asg->name)
            return r;
        accumVar = asg->name;
        rhs = bin->right.get();
        reduceIsAdd = (bin->op == "+");
        isMap = false;
    } else if (inner->kind == ExprKind::IndexAssign) {
        auto* ia = static_cast<const IndexAssignExpr*>(inner);
        if (ia->target->kind != ExprKind::Identifier) return r;
        if (ia->index->kind != ExprKind::Identifier) return r;
        if (static_cast<const IdentifierExpr*>(ia->index.get())->name != counterVar) return r;
        outVar = static_cast<const IdentifierExpr*>(ia->target.get())->name;
        rhs = ia->value.get();
        isMap = true;
    } else {
        return r;
    }

    std::vector<std::string> arrays;
    if (!walkExprTree(rhs, counterVar, arrays)) return r;
    if (arrays.empty()) return r;

    int maxArrays = isMap ? 2 : 2;
    if (static_cast<int>(arrays.size()) > maxArrays) return r;
    if (isMap) {
        for (auto& a : arrays) {
            if (a == outVar) return r;
        }
        if (arrays.size() + 1 > 3) return r;
    }

    std::unordered_map<std::string, int> arrayReg;
    static const int inputRegs[2] = {RAX, RBX};
    for (size_t i = 0; i < arrays.size(); i++) arrayReg[arrays[i]] = inputRegs[i];

    Asm a;
    int outReg = RSI;
    if (isMap) {
        for (size_t i = 0; i < arrays.size(); i++) a.movLoadDisp8(inputRegs[i], RDI, static_cast<int8_t>(i * 8));
        a.movLoadDisp8(outReg, RDI, static_cast<int8_t>(arrays.size() * 8));
    } else {
        for (size_t i = 0; i < arrays.size(); i++) a.movLoadDisp8(inputRegs[i], RDI, static_cast<int8_t>(i * 8));
        a.movsdLoadDisp0(0, RSI);
    }
    a.xorSelf32(RCX);

    size_t loopTop = a.buf.size();
    a.cmpRR(RCX, RDX);
    int exitPatch = a.jgePlaceholder();

    if (isMap) {
        int resultXmm = compileExprNative(a, rhs, counterVar, arrayReg, 1);
        a.movsdStoreIdx8(outReg, RCX, resultXmm);
    } else {
        int resultXmm = compileExprNative(a, rhs, counterVar, arrayReg, 1);
        if (reduceIsAdd) a.addsd(0, resultXmm);
        else a.subsd(0, resultXmm);
    }

    a.addImm8(RCX, 1);
    a.jmpBackTo(loopTop);
    a.patchRel32(exitPatch);

    if (!isMap) a.movsdStoreDisp0(RSI, 0);
    a.ret();

    void* mem = finalizeExecutable(a.buf);
    if (!mem) return r;

    r.ok = true;
    r.code = mem;
    r.codeSize = a.buf.size();
    r.isMap = isMap;
    r.counterVar = counterVar;
    r.boundIsLiteral = boundIsLiteral;
    r.boundLiteral = boundLiteral;
    r.boundVar = boundVar;
    r.arrayNames = arrays;
    r.accumVar = accumVar;
    r.outVar = outVar;
    return r;
}

JitFuncResult tryCompileNativeFunc(const FnDeclStmt* decl) {
    JitFuncResult r;
    // arity <= 4: params live in xmm0..xmm3, leaving xmm4..xmm7 as a
    // small scratch pool for the expression compiler (all "low" SSE
    // registers so our modrm encoder never needs a REX.R/.B bit).
    if (decl->params.empty() || decl->params.size() > 4) return r;
    if (!decl->body || decl->body->statements.size() != 1) return r;
    const Stmt* only = decl->body->statements[0].get();
    if (only->kind != StmtKind::Return) return r;
    auto* ret = static_cast<const ReturnStmt*>(only);
    if (!ret->value) return r;

    std::unordered_map<std::string, int> paramReg;
    for (size_t i = 0; i < decl->params.size(); i++) paramReg[decl->params[i]] = static_cast<int>(i);

    Asm a;
    std::vector<std::unique_ptr<double>> literalPool;
    int maxXmm = 7;
    int freeStart = static_cast<int>(decl->params.size());
    int resultReg = compileFuncExpr(a, ret->value.get(), paramReg, literalPool, freeStart, maxXmm);
    if (resultReg < 0) return r;
    if (resultReg != 0) a.movapd(0, resultReg);
    a.ret();

    void* mem = finalizeExecutable(a.buf);
    if (!mem) return r;

    r.ok = true;
    r.code = mem;
    r.codeSize = a.buf.size();
    r.arity = static_cast<int>(decl->params.size());
    r.literalPool = std::move(literalPool);
    return r;
}

namespace {

// Parses the shared `untuk (buat i = 0; i < N|bound; i = i + 1)` shape.
// Factored out of tryCompileNativeLoop so the scalar-loop JIT below can
// reuse it without duplicating the (already finicky) AST matching.
bool parseCountingLoop(const ForStmt* forNode, std::string& counterVar, bool& boundIsLiteral,
                        double& boundLiteral, std::string& boundVar) {
    if (!forNode->init || forNode->init->kind != StmtKind::Let) return false;
    auto* letInit = static_cast<const LetStmt*>(forNode->init.get());
    if (!letInit->value || letInit->value->kind != ExprKind::Literal) return false;
    auto* initLit = static_cast<const LiteralExpr*>(letInit->value.get());
    if (initLit->litKind != LiteralExpr::Kind::Number || initLit->number != 0.0) return false;
    counterVar = letInit->name;

    if (!forNode->condition || forNode->condition->kind != ExprKind::Binary) return false;
    auto* cond = static_cast<const BinaryExpr*>(forNode->condition.get());
    if (cond->op != "<") return false;
    if (cond->left->kind != ExprKind::Identifier) return false;
    if (static_cast<const IdentifierExpr*>(cond->left.get())->name != counterVar) return false;
    if (cond->right->kind == ExprKind::Literal) {
        auto* lit = static_cast<const LiteralExpr*>(cond->right.get());
        if (lit->litKind != LiteralExpr::Kind::Number) return false;
        boundIsLiteral = true;
        boundLiteral = lit->number;
    } else if (cond->right->kind == ExprKind::Identifier) {
        boundVar = static_cast<const IdentifierExpr*>(cond->right.get())->name;
    } else {
        return false;
    }

    if (!forNode->post || forNode->post->kind != ExprKind::Assign) return false;
    auto* postAssign = static_cast<const AssignExpr*>(forNode->post.get());
    if (postAssign->name != counterVar) return false;
    if (postAssign->value->kind != ExprKind::Binary) return false;
    auto* postBin = static_cast<const BinaryExpr*>(postAssign->value.get());
    if (postBin->op != "+") return false;
    if (postBin->left->kind != ExprKind::Identifier ||
        static_cast<const IdentifierExpr*>(postBin->left.get())->name != counterVar)
        return false;
    if (postBin->right->kind != ExprKind::Literal) return false;
    auto* stepLit = static_cast<const LiteralExpr*>(postBin->right.get());
    if (stepLit->litKind != LiteralExpr::Kind::Number || stepLit->number != 1.0) return false;
    return true;
}

}  // namespace

JitLoopResult tryCompileScalarLoop(const ForStmt* forNode, const FnDeclStmt* inlineCallee) {
    JitLoopResult r;
    std::string counterVar, boundVar;
    bool boundIsLiteral = false;
    double boundLiteral = 0;
    if (!parseCountingLoop(forNode, counterVar, boundIsLiteral, boundLiteral, boundVar)) return r;

    if (!forNode->body || forNode->body->statements.size() != 1) return r;
    const Stmt* onlyStmt = forNode->body->statements[0].get();
    if (onlyStmt->kind != StmtKind::ExprStmt) return r;
    const Expr* inner = static_cast<const ExprStmtNode*>(onlyStmt)->expr.get();
    if (inner->kind != ExprKind::Assign) return r;
    auto* asg = static_cast<const AssignExpr*>(inner);
    std::string accumVar = asg->name;
    if (accumVar == counterVar) return r;
    const Expr* valueExpr = asg->value.get();

    // xmm0 = accumulator (persistent across iterations), xmm1 = loop
    // counter as a double (refreshed every iteration). xmm2..xmm7 are
    // scratch for the expression compiler.
    std::unordered_map<std::string, int> baseEnv;
    baseEnv[accumVar] = 0;
    baseEnv[counterVar] = 1;
    int maxXmm = 7;

    Asm a;
    std::vector<std::unique_ptr<double>> literalPool;

    // -- prologue: RDI=array bases (unused here), RSI=accum ptr, RDX=n --
    a.movsdLoadDisp0(0, RSI);
    a.xorSelf32(RCX);
    size_t loopTop = a.buf.size();
    a.cmpRR(RCX, RDX);
    int exitPatch = a.jgePlaceholder();
    a.cvtsi2sd(1, RCX);

    int resultReg = -1;
    if (valueExpr->kind == ExprKind::Call) {
        auto* call = static_cast<const CallExpr*>(valueExpr);
        if (!inlineCallee) return r;
        if (call->callee->kind != ExprKind::Identifier) return r;
        if (static_cast<const IdentifierExpr*>(call->callee.get())->name != inlineCallee->name) return r;
        if (call->args.size() != inlineCallee->params.size()) return r;
        if (call->args.empty() || call->args.size() > 4) return r;
        if (!inlineCallee->body || inlineCallee->body->statements.size() != 1) return r;
        const Stmt* co = inlineCallee->body->statements[0].get();
        if (co->kind != StmtKind::Return) return r;
        auto* cret = static_cast<const ReturnStmt*>(co);
        if (!cret->value) return r;

        std::vector<int> argRegs;
        int freeXmm = 2;
        for (auto& argExpr : call->args) {
            int reg = compileFuncExpr(a, argExpr.get(), baseEnv, literalPool, freeXmm, maxXmm);
            if (reg < 0) return r;
            argRegs.push_back(reg);
            freeXmm = reg + 1;
        }
        std::unordered_map<std::string, int> paramEnv;
        for (size_t i = 0; i < inlineCallee->params.size(); i++) paramEnv[inlineCallee->params[i]] = argRegs[i];
        resultReg = compileFuncExpr(a, cret->value.get(), paramEnv, literalPool, freeXmm, maxXmm);
    } else {
        resultReg = compileFuncExpr(a, valueExpr, baseEnv, literalPool, 2, maxXmm);
    }
    if (resultReg < 0) return r;
    if (resultReg != 0) a.movapd(0, resultReg);

    a.addImm8(RCX, 1);
    a.jmpBackTo(loopTop);
    a.patchRel32(exitPatch);
    a.movsdStoreDisp0(RSI, 0);
    a.ret();

    void* mem = finalizeExecutable(a.buf);
    if (!mem) return r;

    r.ok = true;
    r.code = mem;
    r.codeSize = a.buf.size();
    r.isMap = false;
    r.counterVar = counterVar;
    r.boundIsLiteral = boundIsLiteral;
    r.boundLiteral = boundLiteral;
    r.boundVar = boundVar;
    r.accumVar = accumVar;
    r.literalPool = std::move(literalPool);
    return r;
}

#else

JitLoopResult tryCompileNativeLoop(const ForStmt*) {
    return JitLoopResult{};
}

JitFuncResult tryCompileNativeFunc(const FnDeclStmt*) {
    return JitFuncResult{};
}

JitLoopResult tryCompileScalarLoop(const ForStmt*, const FnDeclStmt*) {
    return JitLoopResult{};
}

#endif
