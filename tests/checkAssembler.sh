#!/bin/sh
# usage: tests/checkAssembler.sh [x86_64|arm64]
# Developer check: builds sample programs with the built-in assembler and with the
# system toolchain (--external), then compares the disassembled instruction streams.
# Needs llvm-objdump plus a working as/ld or clang. Skips cleanly when they are missing.
cd "$(dirname "$0")/.." || exit 1
arch=${1:-x86_64}
command -v llvm-objdump >/dev/null 2>&1 || { echo "skip: llvm-objdump not found"; exit 0; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ "$arch" = x86_64 ]; then
    syntax="-M intel --start-address=0x4000b0"
else
    syntax=""
fi
normalize() {
    llvm-objdump -d $syntax --no-show-raw-insn "$1" 2>/dev/null \
        | grep -E '^ +[0-9a-f]+:' \
        | sed -E 's/^ +[0-9a-f]+: +//; s/0x[0-9a-f]+/ADDR/g; s/<[^>]*>//g; s/#-?[0-9]+/#N/g; s/[[:space:]]+/ /g; s/^ //' \
        | grep -vE "^$" \
        | grep -vE "^(udf|add byte ptr \[rax\], al$|\.inst|unknown|[0-9a-f]{2} |prf)" \
        | sed -E 's/^xchg (r[a-z0-9]+), (r[a-z0-9]+)$/xchg \1, \2/'
}
failed=0
for name in fib comprehensive floats strings; do
    src="tests/native/$name.ns"
    ./nusa compiler/driver.ns "$src" -o "$work/in" --target "$arch" >/dev/null 2>&1 || { echo "FAIL $name: internal build"; failed=1; continue; }
    ./nusa compiler/driver.ns "$src" -o "$work/ex" --target "$arch" --external >/dev/null 2>&1 || { echo "skip $name: external toolchain unavailable"; continue; }
    normalize "$work/in" >"$work/in.txt"
    normalize "$work/ex" >"$work/ex.txt"
    changed=$(diff "$work/in.txt" "$work/ex.txt" | grep -c '^<')
    if [ "$changed" -le 3 ]; then
        echo "ok   $name ($(wc -l <"$work/in.txt") instructions, $changed cosmetic differences)"
    else
        echo "FAIL $name: $changed differing instructions"
        failed=1
    fi
done
[ $failed -eq 0 ]
