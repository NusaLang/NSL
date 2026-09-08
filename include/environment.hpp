#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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

class GC;

// A lexical scope. Owned by the GC (gc.hpp), never shared_ptr -- closures
// make Environment<->Function reference cycles unavoidable.
class Environment {
public:
    explicit Environment(Environment* parent = nullptr) : parent_(parent) {}

    void define(const std::string& name, const Value& value) { vars_[name] = value; }


    // Returns a reference, not a copy -- get() runs on every variable read.
    const Value& get(const std::string& name) const {
        auto it = vars_.find(name);
        if (it != vars_.end()) return it->second;
        if (parent_) return parent_->get(name);
        throw RuntimeError("Undefined variable '" + name + "'");
    }

    bool isDefined(const std::string& name) const {
        if (vars_.find(name) != vars_.end()) return true;
        if (parent_) return parent_->isDefined(name);
        return false;
    }

    // Pointer variant of get(): one scope-chain walk instead of a separate
    // isDefined()+get(). Same lifetime contract as get().
    Value* find(const std::string& name) {
        auto it = vars_.find(name);
        if (it != vars_.end()) return &it->second;
        if (parent_) return parent_->find(name);
        return nullptr;
    }

    void assign(const std::string& name, const Value& value) {
        auto it = vars_.find(name);
        if (it != vars_.end()) {
            it->second = value;
            return;
        }
        if (parent_) {
            parent_->assign(name, value);
            return;
        }
        throw RuntimeError("Undefined variable '" + name + "'");
    }

    Environment* parent() const { return parent_; }
    // Used by the module system (`impor`) to snapshot a loaded module's
    // top-level bindings into an exported Map value.
    const std::unordered_map<std::string, Value>& vars() const { return vars_; }

private:
    std::unordered_map<std::string, Value> vars_;
    Environment* parent_;

    // GC bookkeeping -- only GC touches these.
    bool gcMarked_ = false;
    friend class GC;
};
