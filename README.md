<p align="center">
  <img src="docs/hero.svg" width="720" alt="NSL" />
</p>

# Nusantara

Nusantara is a programming language that compiles and runs itself — the lexer, parser, interpreter, bytecode VM, and native x86-64 compiler are all written in Nusantara, bootstrapped once by a small C++ host. You can write keywords in English (`func`, `if`, `return`) or Indonesian (`fungsi`, `jika`, `hasil`) and mix them freely in the same file; blocks can use colons or braces.

```python
func fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

for i in range(10):
    print(fib(i))
```

## Three ways to run a program

```bash
./nusa run main.ns                    # tree-walking interpreter (reference implementation)
./nusa --vm main.ns                   # stack-based bytecode VM
./nusa build --native main.ns -o app  # compile straight to a native ELF binary
./app                                 # no runtime, no dependencies
```

The native AOT compiler is the one that matters for speed. It turns `.ns` source into x86-64 assembly by hand — tagged 63-bit integers, fixed-offset structs for classes, inline caches for field and method lookups — and hands the result to the system `as` and `ld`. The interpreter and VM are slower, simpler references that exist mainly to check the language behaves the same way everywhere.

## Benchmarks

Five workloads — `fib(20)`, a 20k-element array build, a 10k-entry map insert+lookup, a 5k-iteration string build, and a 10k object create+method call loop — run in every language below, min-of-N wall-clock milliseconds, log scale:

![benchmark results](docs/benchmark.png)

Native AOT lands within ~2x of Go on `fib20`, `array`, and `oop`, and ahead of every other language here except LuaJIT on `oop`. `map` and `string` are the current weak points.

## Install

```bash
make build    # bootstrap core/main.ns into ./nusa
make test     # run the interpreter, VM, and native AOT test suites
make install  # copy the binary to ~/.local/bin
```

## The language

| Indonesian | English | | Indonesian | English |
|---|---|---|---|---|
| `buat` | `let` / `var` | | `kelas` | `class` |
| `fungsi` | `func` | | `turunan` | `extends` |
| `jika` / `lain` / `kalau` | `if` / `else` / `elif` | | `dalam` | `in` |
| `selama` | `while` | | `coba` / `tangkap` | `try` / `except` |
| `untuk` | `for` | | `lempar` | `throw` |
| `hasil` | `return` | | `benar` / `salah` | `true` / `false` |
| `berhenti` / `lanjut` | `break` / `continue` | | `kosong` | `null` / `None` |
| `dan` / `atau` / `bukan` | `and` / `or` / `not` | | | |

Classes support default parameter values, inheritance, and three interchangeable names for the instance reference (`self`, `this`, `ini`):

```python
class Animal:
    func __init__(self, name, sound = "a sound"):
        self.name = name
        self.sound = sound
    func speak(self):
        print(self.name, "makes", self.sound)

class Dog(Animal):
    func __init__(self, name):
        self.name = name
        self.sound = "a bark"

Dog("Rex").speak()  # Rex makes a bark
```

## Standard library highlights

`runCommand(cmd, args)` spawns a process straight through `fork`/`execve`/`pipe`/`waitpid` in the native backend, with no shell in between — argument lists can't be reinterpreted as shell syntax. File access is `readFile`, `writeFile`, and `fileExists`. `impor()` pulls in another `.ns` file, and the native compiler resolves and inlines it statically at compile time. `gcInfo()` and `gcForce()` let you inspect and trigger the garbage collector directly.

## Repository layout

```
core/        lexer, parser, AST, interpreter, bytecode VM, CLI entry point
compiler/    native x86-64 AOT compiler and its hand-written runtime
std/         standard library (process, filesystem, i18n, ...)
net/         HTTP and socket modules
crypto/      hashing and crypto modules
media/       audio/image processing modules
lib/         additional pure-.ns utility modules
bench/       cross-language benchmark suite and native micro-benchmark
tests/       interpreter/VM/native test suites
```

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
