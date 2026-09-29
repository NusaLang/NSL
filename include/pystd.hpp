#pragma once

#include <string>
#include <vector>

#include "pylib.hpp"

// Native pieces behind the Python-style standard modules (math, json, os, sys, time, random, re).
// The modules themselves are NSL sources in stdlib.cpp that bind these builtins to friendly names.
namespace pystd {

const std::vector<std::string>& builtinNames();
bool handles(const std::string& name);
Value call(const std::string& name, std::vector<Value>& args, const ValueMap* kw, const pylib::CallFn& callFn);

// sys.argv (script path first, then its arguments).
void setArgv(const std::vector<std::string>& argv);

// NSL source of an embedded module (`import http`, `import math`, ...), or nullptr.
const char* embeddedModule(const std::string& name);

}  // namespace pystd
