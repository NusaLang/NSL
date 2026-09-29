#pragma once

#include <string>

#include "value.hpp"

// Python-style methods on built-in types: `xs.append(v)`, `s.upper()`,
// `",".join(xs)`, `d.keys()`. Each maps to an existing builtin that takes the
// receiver as its first argument -- except join, whose receiver is the
// separator (second argument of gabung), see `receiverLast`.
//
// Returns the canonical builtin name, or nullptr if `name` isn't a method of
// this kind of value. A map's own entries always win (a map can be a module).
inline const char* builtinMethodName(const Value& target, const std::string& name, bool* receiverLast = nullptr) {
    if (receiverLast) *receiverLast = false;
    switch (target.type) {
        case ValueType::Array:
        case ValueType::VmArray:
            if (name == "append" || name == "tambah") return "tambah";
            if (name == "pop" || name == "hapus_akhir") return "hapus_akhir";
            return nullptr;
        case ValueType::String:
            if (name == "upper") return "huruf_besar";
            if (name == "lower") return "huruf_kecil";
            if (name == "split") return "pisah";
            if (name == "join") {
                if (receiverLast) *receiverLast = true;
                return "gabung";
            }
            return nullptr;
        case ValueType::Map:
            if (name == "keys") return target.map()->count(name) ? nullptr : "peta_kunci";
            return nullptr;
        default:
            return nullptr;
    }
}
