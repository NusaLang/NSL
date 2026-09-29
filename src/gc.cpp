#include "gc.hpp"

#include <algorithm>

#include "vm.hpp"  // for VmClosure -- see markValue's ValueType::VmFn case

// malloc_trim() returns freed pages to the OS (glibc-only).
#if defined(__GLIBC__)
#include <malloc.h>
#define NS_PUNYA_MALLOC_TRIM 1
#endif

std::atomic<int> GC::liveGoroutines{0};
std::atomic<int> GC::daemonGoroutines{0};
static thread_local bool tlDaemon = false;

void GC::markDaemonThread() {
    if (tlDaemon) return;
    tlDaemon = true;
    daemonGoroutines++;
}

void GC::goroutineDone() {
    if (tlDaemon) { tlDaemon = false; daemonGoroutines--; }
    liveGoroutines--;
}

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

GC::VmFrameRoots** GC::vmHeadSlow() {
    std::lock_guard<std::mutex> lock(gcMutex_);
    VmFrameRoots**& slot = vmHeads_[std::this_thread::get_id()];
    if (!slot) slot = new VmFrameRoots*(nullptr);
    return slot;
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

void GC::trackInstance(const std::shared_ptr<std::unordered_map<std::string, Value>>& fields) {
    std::unique_lock<std::mutex> lock(gcMutex_, std::defer_lock);
    if (GC::liveGoroutines.load(std::memory_order_seq_cst) != 0) lock.lock();
    instances_.push_back(fields);
    instancesSinceCollect_++;
}

void GC::noteStoreSlow(const Value& c) {
    if (c.type == ValueType::Array) trackVector(c.arrayShared());
    else if (c.type == ValueType::Map) trackMap(c.mapShared());
    else if (c.type == ValueType::VmArray) {
        VmArrayState* st = c.vmArray();
        if (st && !st->numeric && st->boxed) trackVector(st->boxed);
    }
}

void GC::trackVector(const std::shared_ptr<std::vector<Value>>& v) {
    std::unique_lock<std::mutex> lock(gcMutex_, std::defer_lock);
    if (GC::liveGoroutines.load(std::memory_order_seq_cst) != 0) lock.lock();
    if (trackedContainers_.insert(v.get()).second) { vectors_.push_back(v); instancesSinceCollect_++; }
}

void GC::trackMap(const std::shared_ptr<std::unordered_map<std::string, Value>>& m) {
    std::unique_lock<std::mutex> lock(gcMutex_, std::defer_lock);
    if (GC::liveGoroutines.load(std::memory_order_seq_cst) != 0) lock.lock();
    if (trackedContainers_.insert(m.get()).second) { maps_.push_back(m); instancesSinceCollect_++; }
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
        if (!markedVectors_.insert(v.array()).second) return;
        for (const Value& el : *v.array()) markValue(el);
    } else if (v.type == ValueType::Map && v.map()) {
        if (!markedFields_.insert(v.map()).second) return;
        for (const auto& [key, val] : *v.map()) markValue(val);
    } else if (v.type == ValueType::Channel && v.channel()) {
        std::lock_guard<std::mutex> chanLock(v.channel()->mu);
        for (const Value& item : v.channel()->queue) markValue(item);
    } else if (v.type == ValueType::Class && v.klass()) {
        for (ClassInfo* c = v.klass(); c; c = c->parent.get()) {
            for (const auto& [name, fn] : c->methods) markEnv(fn->closure);
        }
    } else if (v.type == ValueType::Instance && v.instance()) {
        // The field table is the identity (super views share it); the set also
        // stops a walk that goes around an object cycle.
        if (v.instance()->fields) {
            if (!markedFields_.insert(v.instance()->fields.get()).second) return;
            for (const auto& [key, val] : *v.instance()->fields) markValue(val);
        }
        for (ClassInfo* c = v.instance()->classInfo.get(); c; c = c->parent.get()) {
            for (const auto& [name, fn] : c->methods) markEnv(fn->closure);
        }
    } else if (v.type == ValueType::VmFn && v.vmClosure()) {
        for (Cell* c : v.vmClosure()->upvalues) markCell(c);
    } else if (v.type == ValueType::VmArray && v.vmArray()) {
        VmArrayState* st = v.vmArray();
        if (!st->numeric && st->boxed && markedVectors_.insert(st->boxed.get()).second) {
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
        e->forEachValue([this](const Value& v) { markValue(v); });
        e = e->parent_;
    }
}

void GC::markCell(Cell* c) {
    if (!c || c->gcMarked_) return;
    c->gcMarked_ = true;
    markValue(c->value);
}

void GC::collectIfNeeded() {
    if (allocSinceCollect_ < collectThreshold_ && instancesSinceCollect_ < instanceThreshold_) return;
    collectNow();
}

void GC::collectNow() {
    std::unique_lock<std::mutex> lock(gcMutex_);

    // A thread not yet at a safepoint may have an unrooted temporary on
    // its own stack -- defer, the next collectIfNeeded() will retry.
    if (!allThreadsAtSafePointLocked()) return;

    markedFields_.clear();
    markedVectors_.clear();
    for (auto& e : envs_) e->gcMarked_ = false;
    for (auto& c : cells_) c->gcMarked_ = false;

    if (globals_) markEnv(globals_);
    for (Environment* root : permanentRoots_) markEnv(root);
    for (const auto& [tid, stack] : rootsByThread_) {
        for (Environment* root : stack) markEnv(root);
    }
    for (const auto& [tid, head] : vmHeads_) {
        for (const VmFrameRoots* fr = *head; fr; fr = fr->prev) {
            if (fr->stack) for (const Value& v : *fr->stack) markValue(v);
            if (fr->locals) for (const Value& v : *fr->locals) markValue(v);
            if (fr->boxedLocals) {
                for (Cell* cell : *fr->boxedLocals) markCell(cell);
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

    // Instances nothing reaches: take their fields out (breaking cycles) and let
    // them die after the lock is released.
    std::vector<std::unordered_map<std::string, Value>> deadFields;
    std::vector<std::vector<Value>> deadVectors;
    {
        size_t keep = 0;
        for (size_t i = 0; i < instances_.size(); i++) {
            auto f = instances_[i].lock();
            if (!f) continue;
            if (markedFields_.count(f.get())) {
                instances_[keep++] = std::move(instances_[i]);
            } else if (!f->empty()) {
                deadFields.emplace_back(std::move(*f));
                f->clear();
            }
        }
        instances_.resize(keep);
        trackedContainers_.clear();
        keep = 0;
        for (size_t i = 0; i < maps_.size(); i++) {
            auto m = maps_[i].lock();
            if (!m) continue;
            if (markedFields_.count(m.get())) {
                trackedContainers_.insert(m.get());
                maps_[keep++] = std::move(maps_[i]);
            } else if (!m->empty()) {
                deadFields.emplace_back(std::move(*m));
                m->clear();
            }
        }
        maps_.resize(keep);
        keep = 0;
        for (size_t i = 0; i < vectors_.size(); i++) {
            auto v = vectors_[i].lock();
            if (!v) continue;
            if (markedVectors_.count(v.get())) {
                trackedContainers_.insert(v.get());
                vectors_[keep++] = std::move(vectors_[i]);
            } else if (!v->empty()) {
                deadVectors.emplace_back(std::move(*v));
                v->clear();
            }
        }
        vectors_.resize(keep);
        keep = instances_.size();
        instancesSinceCollect_ = 0;
        instanceThreshold_ = std::max<size_t>(8192, (keep + maps_.size() + vectors_.size()) * 2);
        totalFreed_ += deadFields.size();
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
    collectThreshold_ = std::max<size_t>(kMinCollectThreshold, envs_.size() + cells_.size());
    collections_++;

#ifdef NS_PUNYA_MALLOC_TRIM
    // `dibebaskan` counts Environments, not bytes -- a weak proxy, so also
    // trim periodically and always on an explicit gc_paksa() request.
    if (trimSekarang_ || dibebaskan >= 64 || (collections_ % 8) == 0) {
        malloc_trim(0);
    }
    trimSekarang_ = false;
#endif
    lock.unlock();
    deadFields.clear();
    deadVectors.clear();
}
