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
    MakeClass,
    BinLK,  // slot16 const16 op8: local <op> numeric constant
    BinLL,  // slot16 slot16 op8: local <op> local
    GetField,  // nameConst16: obj.name
    SetField,  // nameConst16: obj.name = value
    CallMethodK,  // nameConst16 argc8: obj.name(args), name known at compile time
    Throw,        // pops a value and throws it (lempar / re-throw after finally)
    MakeStruct,   // name16 count16 field16*: pushes a struct class
    MakeEnum,     // name16 count16 variant16*: pushes the enum's name->value map
    MakeSuper,    // pops an instance, pushes the same fields seen as the owner's parent class
    Yield,        // pops a value, suspends the generator frame and hands the value to the consumer
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

// One protected range of bytecode: an exception raised while executing an
// instruction that starts in [start, end) continues at `target` with the
// thrown value pushed on an otherwise empty operand stack.
struct VmHandler {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t target = 0;
};

// Source position of the instruction starting at `ip`, for error messages.
struct VmLine {
    uint32_t ip = 0;
    int32_t line = 0;
    int32_t col = 0;
};

struct UpvalueDesc {
    bool isLocal;
    int index;
};

struct ParamSlot {
    bool boxed;
    int slot;
};

struct VmProgram;

struct VmFunction {
    std::string name;
    int arity = 0;
    std::vector<std::string> paramNames;  // includes a method's leading "ini"
    int restIndex = -1;  // *args parameter position among the declared params (excluding a method's ini)
    int kwIndex = -1;    // **kwargs parameter position
    bool variadic() const { return restIndex >= 0 || kwIndex >= 0; }
    int minArity = -1;  // fewer arguments than `arity` are allowed down to this (defaults); -1 = arity
    int requiredArity() const { return minArity < 0 ? arity : minArity; }
    int numLocals = 0;
    int numBoxedLocals = 0;
    std::vector<ParamSlot> paramSlots;
    std::vector<uint8_t> code;
    std::vector<Value> constants;
    std::vector<UpvalueDesc> upvalues;
    std::vector<VmHandler> handlers;  // innermost first
    std::vector<VmLine> lines;        // ascending ip; first match wins (innermost node)

    // Set at compile time when eligible for the narrow function-call JIT
    // (tryCompileNativeFunc). callValue() dispatches straight to it only
    // when every argument at the call site is still Number-typed.
    void* nativeCode = nullptr;

    // Per-constant cache of globals-table slots for GetGlobal, filled lazily
    // (see Op::GetGlobal). Only used while the globals Environment has
    // stable slot addresses.
    mutable std::vector<Value*> globalSlots;

    // Where this function lives: its own program (closure/class/native-loop
    // indices are relative to it) and the globals table its top-level names
    // resolve in -- the script's for the main program, a module's own for an
    // imported one. Set by vmBind().
    VmProgram* program = nullptr;
    class Environment* globalsEnv = nullptr;

    // Per-constant inline cache for CallMethodK: the bytecode method found
    // for `classId`. Keyed by ClassInfo::id (never reused).
    struct MethodCacheEntry {
        uint64_t classId = 0;
        const Value* method = nullptr;
    };
    mutable std::vector<MethodCacheEntry> methodCache;
};

// GC-tracked one-Value box (gc.hpp), used for every VM boxed local slot
// and closure upvalue -- not shared_ptr<Value>, which can't free a
// closure that captures a container holding itself.
struct Cell;

struct VmClosure {
    const VmFunction* function = nullptr;
    std::vector<Cell*> upvalues;
    // For a method: the class that declared it (owned by that class's vmMethods
    // table, so it outlives the closure). `induk` resolves through it.
    ClassInfo* owner = nullptr;
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

// `lempar <value>`: any value can be thrown, and `tangkap` receives it as is.
class VmThrown : public VmRuntimeError {
public:
    explicit VmThrown(Value v) : VmRuntimeError(describe(v)), value(std::move(v)) {}
    Value value;

private:
    static std::string describe(const Value& v) {
        if (v.type == ValueType::String) return v.str();
        if (v.type == ValueType::Map) {
            auto it = v.map()->find("pesan");
            if (it != v.map()->end() && it->second.type == ValueType::String) return it->second.str();
        }
        return "Error dilempar: " + v.stringify();
    }
};

// Native generators (a `def` with `yield`): the frame is saved on `yield` and resumed on demand,
// so a generator costs no thread. `handle` comes from vmGenNew.
bool vmIsGenFn(const Value& fn);
Value vmGenNew(const Value& closure);
// kind 1: resume with `sent` as the yield's value; 2: resume by raising `sent` at the yield.
// Returns the yielded value with *ok = true, or *ok = false once the generator has finished.
Value vmGenResume(const Value& handle, int kind, const Value& sent, bool* ok);
void vmGenClose(const Value& handle);

int vmRun(VmProgram& program, class Interpreter* interpreter = nullptr);

// True while a VM program is running (so an `impor`ed module can run on the VM too).
bool vmIsActive();

// Keyword-argument support (used by _callkw / _callkwm in the interpreter): parameter names of a
// bytecode function value (without a method's `ini`), of the bytecode method `name` on a class
// chain, and a by-name method call on an instance.
bool vmParamNames(const Value& fn, std::vector<std::string>& out);
bool vmVarargInfo(const Value& fn, int& restIdx, int& kwIdx);
bool vmMethodParamNames(const ClassInfo* cls, const std::string& name, std::vector<std::string>& out, int* restIdx = nullptr, int* kwIdx = nullptr);
Value vmCallMethod(Value& target, const std::string& name, std::vector<Value>& args, class Interpreter* interpreter);

// Runs a compiled module's top level with `moduleGlobals` as its global namespace.
// The program must outlive every closure it creates. Errors propagate as RuntimeError.
void vmRunModule(VmProgram& program, class Environment* moduleGlobals, class Interpreter* interpreter);

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
