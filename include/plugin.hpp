#pragma once

#include <vector>

#include "value.hpp"

// Host-side loader + call-dispatcher for native plugins (.so, dlopen-based
// -- see plugin_abi.h for the C ABI). What `muat_plugin()` is built on.
namespace plugin {

// dlopen(path), dlsym "ns_plugin_init", collect every registered function
// into a `peta` Value of name -> NativeFunction. Throws std::runtime_error
// on any failure -- a plugin that fails to load is a startup config bug,
// fails loudly rather than surfacing three lines later as a confusing
// field-access-on-null.
Value load(const std::string& path);

// Marshals `args` into NsValue[], releases the GIL for the native call,
// reacquires it, converts the result back. Only null/bool/number/string
// cross directly -- an array/map argument throws (json_encode() it
// yourself first); same for the return value (json_decode() on the way back).
Value call(const NativeFunction& fn, std::vector<Value>& args);

}  // namespace plugin
