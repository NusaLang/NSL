#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "environment.hpp"
#include "value.hpp"

// Mark-and-sweep collector for Environment (closures cycle back to
// their own defining env, which refcounting alone can't free). Roots:
// globals + every in-scope Environment (GcRootGuard). collect() only
// runs when every thread's exprDepth_ is 0 (between statements) --
// tracked per-thread, required for goroutines (GcRootGuard assumes
// per-thread LIFO push/pop). Cell is vm.cpp's Environment-slot
// equivalent, GC-owned for the same cycle reason.
struct Cell {
    Value value;
    bool gcMarked_ = false;
};

class GC {
public:
    static GC& instance();

    Environment* alloc(Environment* parent);
    Cell* allocCell(Value v);
    void setGlobals(Environment* globals) { globals_ = globals; }

    // Fast path when liveGoroutines == 0: no other thread can be
    // touching GC state, so gcMutex_ can be skipped entirely.
    void pushRoot(Environment* env) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            rootsByThread_[std::this_thread::get_id()].push_back(env);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        rootsByThread_[std::this_thread::get_id()].push_back(env);
    }
    void popRoot() {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            auto it = rootsByThread_.find(std::this_thread::get_id());
            it->second.pop_back();
            if (it->second.empty()) rootsByThread_.erase(it);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        auto it = rootsByThread_.find(std::this_thread::get_id());
        it->second.pop_back();
        if (it->second.empty()) rootsByThread_.erase(it);
    }

    // Same as pushRoot/popRoot, for a VM runFrame()'s stack/locals/
    // boxedLocals (registered for the frame's duration via VmRootGuard).
    struct VmFrameRoots {
        const std::vector<Value>* stack = nullptr;
        const std::vector<Value>* locals = nullptr;
        const std::vector<Cell*>* boxedLocals = nullptr;
    };
    void pushVmRoots(const VmFrameRoots& roots) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            vmRootsByThread_[std::this_thread::get_id()].push_back(roots);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        vmRootsByThread_[std::this_thread::get_id()].push_back(roots);
    }
    void popVmRoots() {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            auto it = vmRootsByThread_.find(std::this_thread::get_id());
            it->second.pop_back();
            if (it->second.empty()) vmRootsByThread_.erase(it);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        auto it = vmRootsByThread_.find(std::this_thread::get_id());
        it->second.pop_back();
        if (it->second.empty()) vmRootsByThread_.erase(it);
    }

    // RAII-rooting for a raw C++ local mid-eval (e.g. `callee` in
    // `f(a(), b())` while `b()` still evaluates) -- lets exprDepth_
    // return to 0 across a nested call without a collection sweeping
    // the caller's not-yet-consumed temporary.
    void pushValueRoot(const Value* v) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            valueRootsByThread_[std::this_thread::get_id()].push_back(v);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        valueRootsByThread_[std::this_thread::get_id()].push_back(v);
    }
    void popValueRoot() {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            auto it = valueRootsByThread_.find(std::this_thread::get_id());
            it->second.pop_back();
            if (it->second.empty()) valueRootsByThread_.erase(it);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        auto it = valueRootsByThread_.find(std::this_thread::get_id());
        it->second.pop_back();
        if (it->second.empty()) valueRootsByThread_.erase(it);
    }
    void pushValueVectorRoot(const std::vector<Value>* v) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            valueVectorRootsByThread_[std::this_thread::get_id()].push_back(v);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        valueVectorRootsByThread_[std::this_thread::get_id()].push_back(v);
    }
    void popValueVectorRoot() {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
            auto it = valueVectorRootsByThread_.find(std::this_thread::get_id());
            it->second.pop_back();
            if (it->second.empty()) valueVectorRootsByThread_.erase(it);
            return;
        }
        std::lock_guard<std::mutex> lock(gcMutex_);
        auto it = valueVectorRootsByThread_.find(std::this_thread::get_id());
        it->second.pop_back();
        if (it->second.empty()) valueVectorRootsByThread_.erase(it);
    }

    // Every thread running Nusantara code must register its own
    // exprDepth_ (thread_local, see interpreter.cpp) after first
    // acquiring the GIL, and unregister before thread exit.
    void registerThread(const int* depthPtr);
    void unregisterThread(const int* depthPtr);
    static std::atomic<int> liveGoroutines;

    // Collects if enough allocations have piled up and every thread is
    // at a safe point. Call only while holding the GIL at exprDepth_==0.
    void collectIfNeeded();
    // Forces a collection now if every thread is at a safe point (no-op
    // otherwise). Same calling requirements as collectIfNeeded.
    void collectNow();
    // gc_paksa(): next collection must malloc_trim().
    void mintaTrim() { trimSekarang_ = true; }
    // Marks a collection as due at the next safe point without
    // collecting synchronously -- gc_paksa() itself runs mid-expression.
    void requestCollection() { allocSinceCollect_ = kCollectThreshold; }

    size_t liveCount() const { return envs_.size(); }
    size_t totalAllocated() const { return totalAllocated_; }
    size_t totalFreed() const { return totalFreed_; }
    size_t collections() const { return collections_; }

private:
    void markValue(const Value& v);
    void markEnv(Environment* e);
    void markCell(Cell* c);
    // Caller must already hold gcMutex_.
    bool allThreadsAtSafePointLocked() const;

    mutable std::mutex gcMutex_;
    std::vector<std::unique_ptr<Environment>> envs_;
    std::vector<std::unique_ptr<Cell>> cells_;
    std::unordered_map<std::thread::id, std::vector<Environment*>> rootsByThread_;
    std::unordered_map<std::thread::id, std::vector<VmFrameRoots>> vmRootsByThread_;
    std::unordered_map<std::thread::id, std::vector<const Value*>> valueRootsByThread_;
    std::unordered_map<std::thread::id, std::vector<const std::vector<Value>*>> valueVectorRootsByThread_;
    std::vector<const int*> threadDepths_;
    Environment* globals_ = nullptr;

    size_t allocSinceCollect_ = 0;
    size_t totalAllocated_ = 0;
    size_t totalFreed_ = 0;
    size_t collections_ = 0;
    bool trimSekarang_ = false;
    static constexpr size_t kCollectThreshold = 256;
};

// Roots `env` as reachable from the C++ call stack for as long as this
// guard is alive.
struct GcRootGuard {
    explicit GcRootGuard(Environment* env) { GC::instance().pushRoot(env); }
    ~GcRootGuard() { GC::instance().popRoot(); }
    GcRootGuard(const GcRootGuard&) = delete;
    GcRootGuard& operator=(const GcRootGuard&) = delete;
};

// RAII equivalent of GcRootGuard for one active VM call frame.
struct VmRootGuard {
    explicit VmRootGuard(const GC::VmFrameRoots& roots) { GC::instance().pushVmRoots(roots); }
    ~VmRootGuard() { GC::instance().popVmRoots(); }
    VmRootGuard(const VmRootGuard&) = delete;
    VmRootGuard& operator=(const VmRootGuard&) = delete;
};

// Roots a single Value sitting in a raw C++ local mid-eval.
struct ValueRootGuard {
    explicit ValueRootGuard(const Value& v) { GC::instance().pushValueRoot(&v); }
    ~ValueRootGuard() { GC::instance().popValueRoot(); }
    ValueRootGuard(const ValueRootGuard&) = delete;
    ValueRootGuard& operator=(const ValueRootGuard&) = delete;
};

// Same as ValueRootGuard, for an in-progress std::vector<Value> (e.g. a
// call's `args` accumulator).
struct ValueVectorRootGuard {
    explicit ValueVectorRootGuard(const std::vector<Value>& v) { GC::instance().pushValueVectorRoot(&v); }
    ~ValueVectorRootGuard() { GC::instance().popValueVectorRoot(); }
    ValueVectorRootGuard(const ValueVectorRootGuard&) = delete;
    ValueVectorRootGuard& operator=(const ValueVectorRootGuard&) = delete;
};
