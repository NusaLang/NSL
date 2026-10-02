#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "i18n.hpp"
#include "lexer.hpp"  // for Span
#include "value.hpp"

// Thrown with no span in hand; Interpreter::eval/exec attach the nearest
// enclosing span on the way up (attachLocation() is a no-op after the
// first call, so the innermost span wins), and callFunction() calls
// addFrame() per unwound call for a full call-chain trace.
class RuntimeError : public std::runtime_error {
public:
    explicit RuntimeError(const std::string& msg) : std::runtime_error(msg), baseMessage_(msg), display_(msg) {}

    void attachLocation(Span s) {
        if (located_) return;
        located_ = true;
        span_ = s;
        rebuild();
    }

    void addFrame(const std::string& functionName, Span callSite) {
        totalFrames_++;
        if (frames_.size() < kMaxFrames) {
            frames_.push_back("  di dalam fungsi '" + functionName + "' (dipanggil dari line " +
                               std::to_string(callSite.line) + ", col " + std::to_string(callSite.column) + ")");
        }
        rebuild();
    }

    const char* what() const noexcept override { return display_.c_str(); }

private:
    static constexpr size_t kMaxFrames = 20;

    void rebuild() {
        display_ = baseMessage_;
        if (located_) {
            display_ += " (line " + std::to_string(span_.line) + ", col " + std::to_string(span_.column) + ")";
        }
        for (const std::string& frame : frames_) display_ += "\n" + frame;
        if (totalFrames_ > kMaxFrames) {
            display_ += "\n  ... (" + std::to_string(totalFrames_ - kMaxFrames) + " frame lagi disembunyikan)";
        }
    }

    std::string baseMessage_;
    std::string display_;
    std::vector<std::string> frames_;
    size_t totalFrames_ = 0;
    Span span_{};
    bool located_ = false;
};

// A `lempar`-thrown value travelling as a RuntimeError, so both the
// tree-walker and the VM can catch it and hand the original value to `tangkap`.
inline std::string thrownValueMessage(const Value& v) {
    if (v.type == ValueType::Map) {
        auto it = v.map()->find("pesan");
        if (it != v.map()->end() && it->second.type == ValueType::String) return it->second.str();
    }
    if (v.type == ValueType::String) return v.str();
    if (v.type == ValueType::Instance && v.instance() && v.instance()->classInfo) {  // ValueError: boom
        std::string text = v.stringify();
        const std::string& cls = v.instance()->classInfo->name;
        return text.empty() ? cls : cls + ": " + text;
    }
    return i18n::tr("Error dilempar: ", "Thrown error: ") + v.stringify();
}

class ThrownValue : public RuntimeError {
public:
    explicit ThrownValue(Value v) : RuntimeError(thrownValueMessage(v)), value_(std::move(v)) {}
    const Value& value() const { return value_; }

private:
    Value value_;
};

class GC;

// A lexical scope. Owned by the GC (gc.hpp), never shared_ptr -- closures
// make Environment<->Function reference cycles unavoidable.
class Environment {
public:
    explicit Environment(Environment* parent = nullptr) : parent_(parent) {}

    void define(const std::string& name, const Value& value) {
        if (Value* v = findLocal(name)) { *v = value; return; }
        if (large_) { map_.emplace(name, value); return; }
        if (small_.size() >= kSmallMax) {
            for (auto& e : small_) map_.emplace(std::move(e.name), std::move(e.value));
            small_.clear();
            large_ = true;
            map_.emplace(name, value);
            return;
        }
        if (small_.capacity() == 0) small_.reserve(kSmallMax);  // never relocates below kSmallMax
        small_.push_back(Slot{name, value});
    }

    // Returns a reference, not a copy -- get() runs on every variable read.
    const Value& get(const std::string& name) const {
        for (const Environment* e = this; e; e = e->parent_) {
            if (const Value* v = e->findLocal(name)) return *v;
        }
        throw RuntimeError("Undefined variable '" + name + "'");
    }

    bool isDefined(const std::string& name) const { return const_cast<Environment*>(this)->find(name) != nullptr; }

    // Pointer variant of get(): one scope-chain walk instead of a separate
    // isDefined()+get(). Same lifetime contract as get().
    Value* find(const std::string& name) {
        for (Environment* e = this; e; e = e->parent_) {
            if (Value* v = e->findLocal(name)) return v;
        }
        return nullptr;
    }

    void assign(const std::string& name, const Value& value) {
        if (Value* v = find(name)) { *v = value; return; }
        throw RuntimeError("Undefined variable '" + name + "'");
    }

    template <class F> void forEachValue(F&& f) const {
        if (large_) { for (const auto& kv : map_) f(kv.second); }
        else { for (const auto& e : small_) f(e.value); }
    }

    // True once slot addresses returned by find() can't move any more
    // (hash-map mode; entries are never erased).
    bool stableSlots() const { return large_; }

    Environment* parent() const { return parent_; }
    // Used by the module system (`impor`) to snapshot a loaded module's
    // top-level bindings into an exported Map value.
    std::unordered_map<std::string, Value> vars() const {
        if (large_) return map_;
        std::unordered_map<std::string, Value> out;
        for (const auto& e : small_) out.emplace(e.name, e.value);
        return out;
    }

private:
    // Function scopes hold a handful of names: a linear scan over a small
    // vector beats hashing + a node allocation per variable. Scopes that
    // outgrow kSmallMax (globals, big modules) switch to a hash map.
    static constexpr size_t kSmallMax = 6;
    struct Slot { std::string name; Value value; };

    Value* findLocal(const std::string& name) {
        if (large_) {
            auto it = map_.find(name);
            return it == map_.end() ? nullptr : &it->second;
        }
        for (auto& e : small_) if (e.name == name) return &e.value;
        return nullptr;
    }
    const Value* findLocal(const std::string& name) const {
        return const_cast<Environment*>(this)->findLocal(name);
    }

    std::vector<Slot> small_;
    std::unordered_map<std::string, Value> map_;
    bool large_ = false;
    Environment* parent_;

    // GC bookkeeping -- only GC touches these.
    bool gcMarked_ = false;
    friend class GC;
};
