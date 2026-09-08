// Thin C++ front door to the vendored QuickJS engine (third_party/quickjs).
// Deliberately kept separate from Nusantara's own Value/Environment/GC --
// running a .js file is a totally independent execution engine living in
// the same binary, picked by file extension. No language-level interop
// with .ns is attempted here.
#pragma once

#include <string>

namespace jsrt {

// Runs a JavaScript source file through QuickJS. Returns the process exit
// code to use: 0 on success, 1 on a syntax/runtime error (already printed
// to stderr), matching how runFile()/reportError() behave for .ns files.
int runJsFile(const std::string& path);

}  // namespace jsrt
