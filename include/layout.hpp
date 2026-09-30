#pragma once

#include <string>
#include <vector>

#include "lexer.hpp"

// Python-style layout -> the brace/semicolon token stream the parser expects.
//
//   def f(x):            def f(x) {
//       y = x + 1    ->      let y = x + 1;
//       return y             return y;
//                        }
//
// A file is treated as layout-style when it has no braces and either a
// block-opening ':' (at end of line, or after if/else/while/for/def/class/
// try/except/finally) or no ';' at all across several lines. Everything else
// (including every existing brace-style program) is returned untouched.
//
// Besides blocks and statement ends, this pass also gives assignment its
// Python meaning: `x = 1` declares `x` in the current function scope when it
// isn't declared yet (`global` / `nonlocal` opt out).
std::vector<Token> applyLayout(std::vector<Token> tokens, const std::string& source);
