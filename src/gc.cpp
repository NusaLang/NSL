#include "gc.hpp"

#include <algorithm>

#include "vm.hpp"  // for VmClosure -- see markValue's ValueType::VmFn case

// malloc_trim() returns freed pages to the OS (glibc-only).
#if defined(__GLIBC__)
#include <malloc.h>
#define NS_PUNYA_MALLOC_TRIM 1
#endif

std::atomic<int> GC::liveGoroutines{0};

GC& GC::instance() {
    // Deliberately never deleted -- a detached goroutine can still be
    // running at process teardown; a plain `static GC gc;` would race
    // its own destructor (confirmed via ThreadSanitizer).
    static GC* gc = new GC();
    return *gc;
}

Environment* GC::alloc(Environment* parent) {
    // See the fast-path comment on pushRoot()/popRoot() in gc.hpp: with
    // no goroutine alive, no other thread can be touching envs_/gcMutex_
    // state, so the lock is provably redundant here too.
    if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
        envs_.push_back(std::make_unique<Environment>(parent));
        totalAllocated_++;
        allocSinceCollect_++;
        return envs_.back().get();
    }
    std::lock_guard<std::mutex> lock(gcMutex_);
    envs_.push_back(std::make_unique<Environment>(parent));
    totalAllocated_++;
    allocSinceCollect_++;
    return envs_.back().get();
}

Cell* GC::allocCell(Value v) {
    if (GC::liveGoroutines.load(std::memory_order_seq_cst) == 0) {
        cells_.push_back(std::make_unique<Cell>(Cell{std::move(v), false}));
        totalAllocated_++;
        allocSinceCollect_++;
        return cells_.back().get();
    }
    std::lock_guard<std::mutex> lock(gcMutex_);
    cells_.push_back(std::make_unique<Cell>(Cell{std::move(v), false}));
    totalAllocated_++;
    allocSinceCollect_++;
    return cells_.back().get();
}

void GC::registerThread(const int* depthPtr) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    threadDepths_.push_back(depthPtr);
}

void GC::unregisterThread(const int* depthPtr) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    auto it = std::find(threadDepths_.begin(), threadDepths_.end(), depthPtr);
    if (it != threadDepths_.end()) threadDepths_.erase(it);
}

bool GC::allThreadsAtSafePointLocked() const {
    for (const int* depth : threadDepths_) {
        if (*depth != 0) return false;
    }
    return true;
}

void GC::markValue(const Value& v) {
    if (v.type == ValueType::Fn && v.fn()) {
        markEnv(v.fn()->closure);
    } else if (v.type == ValueType::Array && v.array()) {
        for (const Value& el : *v.array()) markValue(el);
    } else if (v.type == ValueType::Map && v.map()) {
        for (const auto& [key, val] : *v.map()) markValue(val);
    } else if (v.type == ValueType::Channel && v.channel()) {
        std::lock_guard<std::mutex> chanLock(v.channel()->mu);
        for (const Value& item : v.channel()->queue) markValue(item);
    } else if (v.type == ValueType::Class && v.klass()) {
        for (ClassInfo* c = v.klass(); c; c = c->parent.get()) {
            for (const auto& [name, fn] : c->methods) markEnv(fn->closure);
        }
    } else if (v.type == ValueType::Instance && v.instance()) {
        if (v.instance()->fields) {
            for (const auto& [key, val] : *v.instance()->fields) markValue(val);
        }
        for (ClassInfo* c = v.instance()->classInfo.get(); c; c = c->parent.get()) {
            for (const auto& [name, fn] : c->methods) markEnv(fn->closure);
        }
    } else if (v.type == ValueType::VmFn && v.vmClosure()) {
        for (Cell* c : v.vmClosure()->upvalues) markCell(c);
    } else if (v.type == ValueType::VmArray && v.vmArray()) {
        VmArrayState* st = v.vmArray();
        if (!st->numeric && st->boxed) {
            for (const Value& el : *st->boxed) markValue(el);
        }
    }
}

void GC::markEnv(Environment* e) {
    // Iterative walk up the parent chain (rather than recursive) so a
    // long-lived, deeply nested scope chain can't blow the C++ stack
    // during a collection.
    while (e && !e->gcMarked_) {
        e->gcMarked_ = true;
        for (const auto& [name, value] : e->vars_) markValue(value);
        e = e->parent_;
    }
}

void GC::markCell(Cell* c) {
    if (!c || c->gcMarked_) return;
    c->gcMarked_ = true;
    markValue(c->value);
}

void GC::collectIfNeeded() {
    if (allocSinceCollect_ < kCollectThreshold) return;
    collectNow();
}

void GC::collectNow() {
    std::lock_guard<std::mutex> lock(gcMutex_);

    // A thread not yet at a safepoint may have an unrooted temporary on
    // its own stack -- defer, the next collectIfNeeded() will retry.
    if (!allThreadsAtSafePointLocked()) return;

    for (auto& e : envs_) e->gcMarked_ = false;
    for (auto& c : cells_) c->gcMarked_ = false;

    if (globals_) markEnv(globals_);
    for (const auto& [tid, stack] : rootsByThread_) {
        for (Environment* root : stack) markEnv(root);
    }
    for (const auto& [tid, frames] : vmRootsByThread_) {
        for (const VmFrameRoots& fr : frames) {
            if (fr.stack) for (const Value& v : *fr.stack) markValue(v);
            if (fr.locals) for (const Value& v : *fr.locals) markValue(v);
            if (fr.boxedLocals) {
                for (Cell* cell : *fr.boxedLocals) markCell(cell);
            }
        }
    }
    for (const auto& [tid, stack] : valueRootsByThread_) {
        for (const Value* v : stack) markValue(*v);
    }
    for (const auto& [tid, stack] : valueVectorRootsByThread_) {
        for (const std::vector<Value>* vec : stack) {
            for (const Value& v : *vec) markValue(v);
        }
    }

    size_t before = envs_.size();
    envs_.erase(std::remove_if(envs_.begin(), envs_.end(),
                                [](const std::unique_ptr<Environment>& e) { return !e->gcMarked_; }),
                envs_.end());
    size_t cellsBefore = cells_.size();
    cells_.erase(std::remove_if(cells_.begin(), cells_.end(),
                                 [](const std::unique_ptr<Cell>& c) { return !c->gcMarked_; }),
                 cells_.end());
    size_t dibebaskan = (before - envs_.size()) + (cellsBefore - cells_.size());
    totalFreed_ += dibebaskan;
    allocSinceCollect_ = 0;
    collections_++;

#ifdef NS_PUNYA_MALLOC_TRIM
    // `dibebaskan` counts Environments, not bytes -- a weak proxy, so also
    // trim periodically and always on an explicit gc_paksa() request.
    if (trimSekarang_ || dibebaskan >= 64 || (collections_ % 8) == 0) {
        malloc_trim(0);
    }
    trimSekarang_ = false;
#endif
}
