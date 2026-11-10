#!/bin/sh
# usage: tests/runLang.sh
# Runs every tests/lang/*.ns through the interpreter, the bytecode VM and the native
# compiler and requires all three to print tests/lang/expected/<name>.out.
# UPDATE=1 rewrites the expected files from the interpreter.
cd "$(dirname "$0")/.." || exit 1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p tests/lang/expected
failed=0
total=0
for src in tests/lang/*.ns; do
    name=$(basename "$src" .ns)
    expected="tests/lang/expected/$name.out"
    ./nusa "$src" >"$work/interp.out" 2>&1
    [ -n "$UPDATE" ] && cp "$work/interp.out" "$expected"
    ./nusa --vm "$src" >"$work/vm.out" 2>&1
    if ./nusa build "$src" -o "$work/bin" >"$work/build.log" 2>&1; then
        "$work/bin" >"$work/native.out" 2>&1
    else
        cp "$work/build.log" "$work/native.out"
    fi
    for mode in interp vm native; do
        total=$((total + 1))
        if cmp -s "$expected" "$work/$mode.out"; then
            :
        else
            echo "FAIL $name ($mode)"
            diff "$expected" "$work/$mode.out" | head -6 | sed 's/^/    /'
            failed=$((failed + 1))
        fi
    done
done
echo "$((total - failed))/$total language checks passed"
[ $failed -eq 0 ]
