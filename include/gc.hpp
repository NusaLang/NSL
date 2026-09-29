#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // Instances are refcounted (shared field table), so a cycle between objects
    // (a.other = b; b.other = a) would never free itself. Every instance's field
    // table is registered here; a collection clears the ones no root can reach.
    void trackInstance(const std::shared_ptr<std::unordered_map<std::string, Value>>& fields);
    void setGlobals(Environment* globals) { globals_ = globals; }
    // For environments that must outlive every scope guard (a VM module's globals,
    // which its closures reach only through a raw pointer).
    void addPermanentRoot(Environment* env) {
        std::lock_guard<std::mutex> lock(gcMutex_);
        permanentRoots_.push_back(env);
    }

    // Fast path when liveGoroutines == 0: no other thread can be
    // touching GC state, so gcMutex_ can be skipped entirely.
    // Roots live in per-thread vectors inside the *RootsByThread_ maps. The
    // vector is looked up once per thread and cached (unordered_map nodes are
    // never invalidated, and entries are never erased), so a push/pop is a
    // plain vector op instead of a hash lookup + node alloc.
    void pushRoot(Environment* env) { push(envRoots(), env); }
    void popRoot() { pop(envRoots()); }

    // Same as pushRoot/popRoot, for a VM runFrame()'s stack/locals/
    // boxedLocals (registered for the frame's duration via VmRootGuard).
    // Frames form an intrusive per-thread list (innermost first) that lives on
    // the C++ stack, so registering a frame is two pointer writes. The head
    // cell is heap-allocated once per thread and never freed: the GC may still
    // walk it after the thread exits (the list is empty by then).
    struct VmFrameRoots {
        const ValueWindow* stack = nullptr;
        const ValueWindow* locals = nullptr;
        const std::vector<Cell*>* boxedLocals = nullptr;
        VmFrameRoots* prev = nullptr;
    };
    void pushVmRoots(VmFrameRoots& roots) {
        VmFrameRoots*& head = vmHead();
        roots.prev = head;
        head = &roots;
    }
    void popVmRoots() {
        VmFrameRoots*& head = vmHead();
        head = head->prev;
    }

    // RAII-rooting for a raw C++ local mid-eval (e.g. `callee` in
    // `f(a(), b())` while `b()` still evaluates) -- lets exprDepth_
    // return to 0 across a nested call without a collection sweeping
    // the caller's not-yet-consumed temporary.
    void pushValueRoot(const Value* v) { push(valueRoots(), v); }
    void popValueRoot() { pop(valueRoots()); }
    void pushValueVectorRoot(const std::vector<Value>* v) { push(valueVectorRoots(), v); }
    void popValueVectorRoot() { pop(valueVectorRoots()); }

    // Every thread running Nusantara code must register its own
    // exprDepth_ (thread_local, see interpreter.cpp) after first
    // acquiring the GIL, and unregister before thread exit.
    void registerThread(const int* depthPtr);
    void unregisterThread(const int* depthPtr);
    static std::atomic<int> liveGoroutines;
    // Goroutines that called latar(): background workers (e.g. Go callback listeners) that
    // must not keep the process alive once the main script has finished.
    static std::atomic<int> daemonGoroutines;
    static void markDaemonThread();
    // Every goroutine's last act (replaces a bare liveGoroutines--).
    static void goroutineDone();

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
    template <class T, class Map>
    std::vector<T>& threadVec(Map& m, std::vector<T>*& cache) {
        if (!cache) {
            std::unique_lock<std::mutex> lock(gcMutex_, std::defer_lock);
            if (GC::liveGoroutines.load(std::memory_order_seq_cst) != 0) lock.lock();
            cache = &m[std::this_thread::get_id()];
        }
        return *cache;
    }
    template <class T>
    void push(std::vector<T>& v, const T& x) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) { v.push_back(x); return; }
        std::lock_guard<std::mutex> lock(gcMutex_);
        v.push_back(x);
    }
    template <class T>
    void pop(std::vector<T>& v) {
        if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) { v.pop_back(); return; }
        std::lock_guard<std::mutex> lock(gcMutex_);
        v.pop_back();
    }
    std::vector<Environment*>& envRoots() {
        static thread_local std::vector<Environment*>* c = nullptr;
        return threadVec(rootsByThread_, c);
    }
    VmFrameRoots*& vmHead() {
        static thread_local VmFrameRoots** cache = nullptr;
        if (!cache) cache = vmHeadSlow();
        return *cache;
    }
    VmFrameRoots** vmHeadSlow();
    std::vector<const Value*>& valueRoots() {
        static thread_local std::vector<const Value*>* c = nullptr;
        return threadVec(valueRootsByThread_, c);
    }
    std::vector<const std::vector<Value>*>& valueVectorRoots() {
        static thread_local std::vector<const std::vector<Value>*>* c = nullptr;
        return threadVec(valueVectorRootsByThread_, c);
    }

    void markValue(const Value& v);
    void markEnv(Environment* e);
    void markCell(Cell* c);
    // Caller must already hold gcMutex_.
    bool allThreadsAtSafePointLocked() const;

    mutable std::mutex gcMutex_;
    std::vector<std::unique_ptr<Environment>> envs_;
    std::vector<std::unique_ptr<Cell>> cells_;
    std::unordered_map<std::thread::id, std::vector<Environment*>> rootsByThread_;
    std::unordered_map<std::thread::id, VmFrameRoots**> vmHeads_;
    std::unordered_map<std::thread::id, std::vector<const Value*>> valueRootsByThread_;
    std::unordered_map<std::thread::id, std::vector<const std::vector<Value>*>> valueVectorRootsByThread_;
    std::vector<std::weak_ptr<std::unordered_map<std::string, Value>>> instances_;
    std::unordered_set<const void*> markedFields_;
    size_t instancesSinceCollect_ = 0;
    size_t instanceThreshold_ = 8192;
    std::vector<const int*> threadDepths_;
    Environment* globals_ = nullptr;
    std::vector<Environment*> permanentRoots_;

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
    explicit VmRootGuard(GC::VmFrameRoots& roots) { GC::instance().pushVmRoots(roots); }
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
