#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "value.hpp"

enum class Op : uint8_t {
    Const,
    Null,
    True,
    False,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Neq,
    Lt,
    Lte,
    Gt,
    Gte,
    Pop,
    GetLocal,
    SetLocal,
    DefineLocal,
    GetBoxedLocal,
    SetBoxedLocal,
    DefineBoxedLocal,
    GetUpvalue,
    SetUpvalue,
    GetGlobal,
    SetGlobal,
    DefineGlobal,
    JumpIfFalse,
    Jump,
    JumpIfFalseKeep,
    JumpIfTrueKeep,
    Loop,
    Call,
    MakeClosure,
    Print,
    Length,
    Push,
    NewArray,
    GetIndex,
    SetIndex,
    Return,
    ReturnNull,
    TryNativeLoop,
    GoSpawn,
    ChanNew,
    ChanSend,
    ChanRecv,
    Import,
    CallMethod,
};

struct NativeLoopDesc {
    void* code = nullptr;
    size_t codeSize = 0;
    bool isMap = false;
    std::vector<int> arraySlots;
    std::vector<uint8_t> arrayBoxed;
    int accumSlot = -1;
    uint8_t accumBoxed = 0;
    int outSlot = -1;
    uint8_t outBoxed = 0;
    bool boundIsLiteral = false;
    double boundLiteral = 0;
    int boundSlot = -1;
    uint8_t boundBoxed = 0;

    // Global-slot variant of accumSlot/boundSlot, for a top-level scalar
    // accumulator loop. When set, the native call runs WITHOUT releasing
    // the GIL -- a global is visible to every goroutine, so a raw
    // unsynchronized double* into it would otherwise race.
    bool accumIsGlobal = false;
    std::string accumGlobalName;
    bool boundIsGlobal = false;
    std::string boundGlobalName;
};

struct UpvalueDesc {
    bool isLocal;
    int index;
};

struct ParamSlot {
    bool boxed;
    int slot;
};

struct VmFunction {
    std::string name;
    int arity = 0;
    int numLocals = 0;
    int numBoxedLocals = 0;
    std::vector<ParamSlot> paramSlots;
    std::vector<uint8_t> code;
    std::vector<Value> constants;
    std::vector<UpvalueDesc> upvalues;

    // Set at compile time when eligible for the narrow function-call JIT
    // (tryCompileNativeFunc). callValue() dispatches straight to it only
    // when every argument at the call site is still Number-typed.
    void* nativeCode = nullptr;
};

// GC-tracked one-Value box (gc.hpp), used for every VM boxed local slot
// and closure upvalue -- not shared_ptr<Value>, which can't free a
// closure that captures a container holding itself.
struct Cell;

struct VmClosure {
    const VmFunction* function = nullptr;
    std::vector<Cell*> upvalues;
};

struct VmProgram {
    std::vector<std::unique_ptr<VmFunction>> functions;
    VmFunction* topLevel = nullptr;
    std::vector<NativeLoopDesc> nativeLoops;
    // Keeps the double literal pools referenced by JIT-compiled function
    // bodies (see VmFunction::nativeCode) alive for the program's lifetime.
    std::vector<std::vector<std::unique_ptr<double>>> nativeFuncLiteralPools;
};

class VmCompileError : public std::runtime_error {
public:
    explicit VmCompileError(const std::string& msg) : std::runtime_error(msg) {}
};

std::unique_ptr<VmProgram> vmCompile(const Program& program);

class VmRuntimeError : public std::runtime_error {
public:
    explicit VmRuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

int vmRun(VmProgram& program, class Interpreter* interpreter = nullptr);

// Calls a VmFn from outside the VM's own call stack (e.g.
// Interpreter::callValue, for a callback stored via http_dengar()/jalan()).
// Needs a VM program already running on this process; throws otherwise.
Value vmCallValue(const Value& callee, std::vector<Value>& args, class Interpreter* interpreter);

void vmSerialize(const VmProgram& program, const std::string& path, const std::string& combinedHash);
std::unique_ptr<VmProgram> vmDeserialize(const std::string& path, const std::string& expectedHash);

// Re-attaches the JIT (VmFunction::nativeCode) to a VmProgram loaded
// from the on-disk cache -- vmDeserialize never reconstructs nativeCode
// (a raw PROT_EXEC pointer can't survive a different process/ASLR), so
// this recompiles it from sourceAst via the normal codegen path.
void vmAttachNativeFunctions(VmProgram& program, const Program& sourceAst);
