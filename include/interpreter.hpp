#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "environment.hpp"
#include "value.hpp"

// GC (gc.hpp) is a process-wide singleton; Interpreter registers
// `globals_` as the GC root on construction -- only ever construct one
// Interpreter per process.
class Interpreter {
public:
    // entryDir: directory relative `impor()` paths resolve against.
    explicit Interpreter(std::string entryDir = ".");
    void setOutputStream(std::ostream* os) { outStream_ = os; }
    void run(const Program& program);
    // For REPL use: evaluate one expression against the persistent
    // global environment (so `let`s from earlier lines stay visible).
    Value evalGlobal(const Expr* expr);

    // Registers/unregisters the calling thread's exprDepth_ with the
    // GC's per-thread safepoint registry (gc.hpp).
    static void registerCurrentThread();
    // Goroutine body, run on the thread `jalan()` spawns.
    // `rootedFlag`, if set, is released right after fn's closure is
    // rooted (before touching the GIL) -- jalan() waits on it before
    // returning, closing a real use-after-free window between
    // pthread_create() returning and this thread rooting its own closure.
    void jalankanBadanGoroutine(Value fn, std::vector<Value> args, std::atomic<bool>* rootedFlag = nullptr);
    static void unregisterCurrentThread();

private:
    Environment* globals_;
    // thread_local: each goroutine thread shares this Interpreter but
    // needs its own "am I mid-expression" depth (see gc.hpp).
    static thread_local int exprDepth_;
    static thread_local int callDepth_;
    static constexpr int kMaxCallDepth = 2000;

    // Public-facing wrappers that catch a RuntimeError on its way up
    // and attach this node's span if nothing closer to the actual
    // fault already claimed it (see RuntimeError::attachLocation in
    // environment.hpp). The *Inner methods hold the real switch logic.
    void exec(const Stmt* stmt, Environment* env);
    void execInner(const Stmt* stmt, Environment* env);
    Value eval(const Expr* expr, Environment* env);
    Value evalInner(const Expr* expr, Environment* env);
    void execBlock(const BlockStmt* block, Environment* env);

public:

    Value callValue(const Value& callee, std::vector<Value>& args, Span callSite);
    Value callFunction(const std::shared_ptr<Function>& fn, std::vector<Value>& args, Span callSite,
                       const Value* boundThis = nullptr, const std::shared_ptr<ClassInfo>& methodOwner = nullptr);
    Value callBuiltin(const std::string& name, std::vector<Value>& args);

    Value doImport(const std::string& path);
    Environment* getGlobalsEnv() const { return globals_; }
    const std::unordered_map<std::string, Value>& getGlobals() const { return globals_->vars(); }

private:
    std::ostream* outStream_ = nullptr;
    std::vector<std::unique_ptr<Program>> importedPrograms_;
    std::unordered_map<std::string, Value> moduleCache_;
    std::vector<std::string> importStack_;   // cycle detection (resolved paths)
    std::vector<std::string> importDirStack_;  // directory each nested impor() resolves relative to
};
