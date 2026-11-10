#!/bin/sh
# usage: tests/runNative.sh [x86_64|arm64] [runner...]
# Builds every tests/native/*.ns, runs it (optionally through a runner such as
# qemu-aarch64-static) and compares stdout with tests/native/expected/<name>.out.
# Tests listed in tests/native/hostOnly spawn host programs and are skipped when a runner is given.
# UPDATE=1 rewrites the expected files from the current run.
cd "$(dirname "$0")/.." || exit 1
arch=${1:-x86_64}
[ $# -gt 0 ] && shift
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p tests/native/expected
failed=0
total=0
for src in tests/native/*.ns; do
    name=$(basename "$src" .ns)
    [ "$name" = helperModule ] && continue
    if [ $# -gt 0 ] && grep -qx "$name" tests/native/hostOnly; then
        echo "skip $name (needs host programs)"
        continue
    fi
    total=$((total + 1))
    bin="$work/$name"
    if [ "$arch" = x86_64 ]; then
        ./nusa build "$src" -o "$bin" >"$work/build.log" 2>&1
    else
        ./nusa compiler/driver.ns "$src" -o "$bin" --target "$arch" >"$work/build.log" 2>&1
    fi
    if [ $? -ne 0 ]; then
        echo "FAIL $name: build"
        sed 's/^/    /' "$work/build.log" | head -5
        failed=$((failed + 1))
        continue
    fi
    "$@" "$bin" >"$work/$name.out" 2>&1
    rc=$?
    expected="tests/native/expected/$name.out"
    if [ -n "$UPDATE" ] && [ $rc -eq 0 ]; then
        cp "$work/$name.out" "$expected"
    fi
    if [ $rc -ne 0 ]; then
        echo "FAIL $name: exit $rc"
        head -5 "$work/$name.out" | sed 's/^/    /'
        failed=$((failed + 1))
    elif [ -f "$expected" ] && ! cmp -s "$expected" "$work/$name.out"; then
        echo "FAIL $name: output differs"
        diff "$expected" "$work/$name.out" | head -6 | sed 's/^/    /'
        failed=$((failed + 1))
    else
        echo "ok   $name"
    fi
done
echo "$((total - failed))/$total native tests passed ($arch)"
[ $failed -eq 0 ]
