#!/bin/sh
# usage: launcher.sh <runtimeDir> [nusa arguments]
# `nusa file.ns` and `nusa run file.ns` compile to a cached native binary and run it,
# like `go run`. Anything the native compiler cannot build falls back to the interpreter.
# NUSA_INTERP=1 forces the interpreter. NUSA_CACHE overrides the cache directory.
DIR=$1
shift
ENGINE="$DIR/nusa-engine"
export NUSA_HOME="$DIR"

runInterp() {
    if [ -x "$ENGINE" ]; then
        exec "$ENGINE" "$DIR/core/main.ns" "$@"
    fi
    exec nusa "$DIR/core/main.ns" "$@"
}

src=""
skip=0
case "$1" in
    run) case "$2" in *.ns) src=$2; skip=2 ;; esac ;;
    *.ns) src=$1; skip=1 ;;
esac
if [ -z "$src" ] || [ ! -f "$src" ] || [ -n "$NUSA_INTERP" ] || [ ! -x "$ENGINE" ] || ! command -v cksum >/dev/null 2>&1; then
    runInterp "$@"
fi

abs="$(cd "$(dirname "$src")" && pwd)/$(basename "$src")"
cache="${NUSA_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/nusa}"
bin="$cache/$(printf '%s' "$abs" | cksum | tr ' ' '-')"

stale() {
    [ -e "$1" ] || return 0
    for f in "$abs" "$DIR"/compiler/*.ns "$DIR"/compiler/backend/*/*.ns "$DIR"/compiler/assembler/*.ns "$DIR"/core/parser/*.ns "$DIR"/core/resolver.ns; do
        [ "$f" -nt "$1" ] && return 0
    done
    if [ -f "$bin.deps" ]; then
        while IFS= read -r dep; do
            [ -e "$dep" ] || return 0
            [ "$dep" -nt "$1" ] && return 0
        done <"$bin.deps"
    fi
    return 1
}

if stale "$bin"; then
    if [ -e "$bin.fail" ] && ! stale "$bin.fail"; then
        runInterp "$@"
    fi
    mkdir -p "$cache" 2>/dev/null || runInterp "$@"
    tmp="$bin.$$"
    if NUSA_DEPS="$tmp.deps" "$ENGINE" "$DIR/core/main.ns" build "$src" -o "$tmp" >/dev/null 2>&1 && [ -x "$tmp" ]; then
        mv -f "$tmp.deps" "$bin.deps" 2>/dev/null
        mv -f "$tmp" "$bin"
        [ -e "$bin.fail" ] && unlink "$bin.fail"
    else
        for leftover in "$tmp" "$tmp.s" "$tmp.o" "$tmp.b64" "$tmp.deps"; do
            [ -e "$leftover" ] && unlink "$leftover"
        done
        : >"$bin.fail"
        runInterp "$@"
    fi
fi
shift $skip
exec "$bin" "$@"
