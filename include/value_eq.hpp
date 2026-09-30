#pragma once

#include "value.hpp"

// `==` as Python users expect it: numbers, strings, booleans and null by value; arrays
// and maps by contents (recursively); everything else (instances, functions, classes,
// channels, ...) by identity. Shared by the tree-walker and the VM so both agree.
inline bool valuesDeepEqual(const Value& a, const Value& b, int depth = 0) {
    auto isSeq = [](const Value& v) { return v.type == ValueType::Array || v.type == ValueType::VmArray; };
    auto seqLen = [](const Value& v) -> size_t {
        if (v.type == ValueType::Array) return v.array()->size();
        const VmArrayState* st = v.vmArray();
        return st->numeric ? st->nums.size() : st->boxed->size();
    };
    auto seqAt = [](const Value& v, size_t i) -> Value {
        if (v.type == ValueType::Array) return (*v.array())[i];
        const VmArrayState* st = v.vmArray();
        return st->numeric ? Value::fromNumber(st->nums[i]) : (*st->boxed)[i];
    };
    if (isSeq(a) && isSeq(b)) {
        if (a.ref.get() == b.ref.get()) return true;
        size_t n = seqLen(a);
        if (n != seqLen(b) || depth > 64) return false;
        for (size_t i = 0; i < n; i++) {
            if (!valuesDeepEqual(seqAt(a, i), seqAt(b, i), depth + 1)) return false;
        }
        return true;
    }
    if (a.type == ValueType::Instance && instanceOpHook()) {
        Value r;
        if (instanceOpHook()("__eq__", a, b, r)) return r.truthy();
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case ValueType::Null: return true;
        case ValueType::Bool: return a.boolean() == b.boolean();
        case ValueType::Number: return a.number == b.number;
        case ValueType::String: return a.str() == b.str();
        case ValueType::Builtin: return a.builtinName() == b.builtinName();
        case ValueType::Map: {
            if (a.ref.get() == b.ref.get()) return true;
            const auto& ma = *a.map();
            const auto& mb = *b.map();
            if (ma.size() != mb.size() || depth > 64) return false;
            for (const auto& [k, v] : ma) {
                auto it = mb.find(k);
                if (it == mb.end() || !valuesDeepEqual(v, it->second, depth + 1)) return false;
            }
            return true;
        }
        default: return a.ref.get() == b.ref.get();
    }
}
