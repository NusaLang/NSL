#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "value.hpp"

// Python-flavoured builtins (abs, min, max, sum, round, sorted, enumerate, zip, map, filter, ...),
// the methods of str / list / dict, and Python's string formatting (format specs, `%`, str.format).
// Written against Value only; anything that must call back into user code takes a CallFn.
namespace pylib {

struct PyError : std::runtime_error {
    explicit PyError(const std::string& m) : std::runtime_error(m) {}
};

// Calls `inst.name(args)` if the instance's class has that method (false otherwise). Installed by
// the interpreter; lets iterating builtins (list, sum, zip, ...) drain user iterators and generators.
using MethodHook = std::function<bool(const Value& inst, const char* name, std::vector<Value>& args, Value* out)>;
void setMethodHook(MethodHook hook);

using CallFn = std::function<Value(const Value& fn, std::vector<Value>& args)>;

// Builtins provided here (registered as globals). Method implementations are named `_m_s_*`
// (str), `_m_a_*` (list), `_m_d_*` (dict) and get their receiver as first argument.
const std::vector<std::string>& builtinNames();
bool handles(const std::string& name);

// `args` are the positional arguments; `kw` holds keyword arguments (or is null).
Value call(const std::string& name, std::vector<Value>& args, const ValueMap* kw, const CallFn& callFn);

// Canonical builtin implementing method `name` on `target`, or nullptr.
const char* methodBuiltin(const Value& target, const std::string& name);

// Python ordering of two values (numbers, strings, lists/tuples element-wise): <0, 0, >0. Throws PyError.
int compareValues(const Value& a, const Value& b);

std::string formatValue(const Value& v, const std::string& spec);

}  // namespace pylib
