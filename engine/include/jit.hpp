#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"

struct JitLoopResult {
    bool ok = false;
    void* code = nullptr;
    size_t codeSize = 0;
    bool isMap = false;
    std::string counterVar;
    bool boundIsLiteral = false;
    double boundLiteral = 0;
    std::string boundVar;
    std::vector<std::string> arrayNames;
    std::string accumVar;
    std::string outVar;
    // Backing storage for double literals baked into the code by
    // tryCompileScalarLoop (empty/unused for tryCompileNativeLoop, which
    // never emits literal loads). Must outlive `code`.
    std::vector<std::unique_ptr<double>> literalPool;
};

JitLoopResult tryCompileNativeLoop(const ForStmt* forNode);

// Narrow function-call JIT: straight-line numeric bodies (+,-,*,/,
// unary '-' over params/literals) to native x86-64 (System V AMD64,
// doubles in xmm0..xmm(arity-1)/xmm0). Only runtime guard: args still
// Number-typed.
struct JitFuncResult {
    bool ok = false;
    void* code = nullptr;
    size_t codeSize = 0;
    int arity = 0;
    // Backing storage for double literals baked into the code (the code
    // holds absolute pointers to these, so they must outlive it -- kept
    // alive for the process lifetime alongside the VmProgram).
    std::vector<std::unique_ptr<double>> literalPool;
};

JitFuncResult tryCompileNativeFunc(const FnDeclStmt* decl);

// Whole-loop JIT for `untuk (buat i=0; i<N; i=i+1) { acc = <expr>; }`.
// A single call to a JIT-eligible function gets inlined (op-tree
// spliced in) instead of emitting a call instruction. `inlineCallee`:
// the resolved callee for `acc = someFn(...)` (vm.cpp-verified
// unshadowed global), else nullptr.
JitLoopResult tryCompileScalarLoop(const ForStmt* forNode, const FnDeclStmt* inlineCallee);
