#pragma once

#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

// Python sequence repetition: "ab" * 3, 3 * "ab", [0] * 5. Returns false when the operands are
// not one of those combinations (the caller then reports the usual arithmetic error).
inline bool repeatValue(const Value& a, const Value& b, Value& out) {
    const Value* seq = nullptr;
    const Value* cnt = nullptr;
    if (b.type == ValueType::Number) { seq = &a; cnt = &b; }
    else if (a.type == ValueType::Number) { seq = &b; cnt = &a; }
    else return false;
    if (seq->type != ValueType::String && seq->type != ValueType::Array && seq->type != ValueType::VmArray) return false;
    long long n = static_cast<long long>(cnt->number);
    if (n < 0) n = 0;
    if (seq->type == ValueType::String) {
        std::string r;
        r.reserve(seq->str().size() * static_cast<size_t>(n));
        for (long long i = 0; i < n; i++) r += seq->str();
        out = Value::fromString(std::move(r));
        return true;
    }
    if (seq->type == ValueType::Array) {
        auto arr = std::make_shared<std::vector<Value>>();
        for (long long i = 0; i < n; i++) arr->insert(arr->end(), seq->array()->begin(), seq->array()->end());
        out = Value::fromArray(arr);
        return true;
    }
    VmArrayState* st = seq->vmArray();
    auto ns = std::make_shared<VmArrayState>();
    ns->numeric = st->numeric;
    if (st->numeric) {
        for (long long i = 0; i < n; i++) ns->nums.insert(ns->nums.end(), st->nums.begin(), st->nums.end());
    } else {
        ns->boxed = std::make_shared<std::vector<Value>>();
        for (long long i = 0; i < n; i++) ns->boxed->insert(ns->boxed->end(), st->boxed->begin(), st->boxed->end());
    }
    out = Value::fromVmArray(ns);
    return true;
}
