#pragma once

#include <string>
#include <vector>

#include "value.hpp"

// Optional system plugins (nusantara-plugins/<name>.so next to the binary) that the core calls on
// demand -- the vendored C engines (QuickJS, quirc/stb) live there, not in the core binary.
namespace sysmod {

// Loads the plugin once, then calls its function. Throws std::runtime_error with a hint
// ("make plugins") when the plugin isn't installed.
Value call(const std::string& plugin, const std::string& function, std::vector<Value> args);

}  // namespace sysmod
