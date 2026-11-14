#!/bin/sh
# usage: launcher.sh <runtimeDir> [nusa arguments]
# `nusa file.ns` and `nusa run file.ns` compile to a cached native binary and run it,
# like `go run`. Anything the native compiler cannot build falls back to the interpreter.
# NusaInterp=1 forces the interpreter. NusaCache overrides the cache directory.
# `nusa why file.ns` says which engine a program gets and why, without running it.
# The older spellings NUSA_INTERP, NUSA_CACHE, NUSA_HOME are still read.
DIR=$1
shift
ENGINE="$DIR/nusa-engine"
NusaInterp="${NusaInterp:-$NUSA_INTERP}"
NusaCache="${NusaCache:-$NUSA_CACHE}"
NusaHome="$DIR"
export NusaHome
export NUSA_HOME="$DIR" # the bootstrap engine reads this spelling

runInterp() {
    if [ -n "$whyOnly" ]; then
        exit 0
    fi
    if [ -x "$ENGINE" ]; then
        exec "$ENGINE" "$DIR/core/main.ns" "$@"
    fi
    exec nusa "$DIR/core/main.ns" "$@"
}

explain() {
    [ -n "$whyOnly" ] && printf 'nusa: %s\n' "$1" >&2
}

whyOnly=""
if [ "$1" = "why" ]; then
    whyOnly=1
    shift
    NusaInterp=""
fi

src=""
skip=0
case "$1" in
    run) case "$2" in *.ns) src=$2; skip=2 ;; esac ;;
    *.ns) src=$1; skip=1 ;;
esac
if [ -n "$whyOnly" ] && { [ -z "$src" ] || [ ! -f "$src" ]; }; then
    echo "usage: nusa why <file.ns>" >&2
    exit 2
fi
if [ -n "$NusaInterp" ]; then
    explain "interpreter (NusaInterp is set)"
    runInterp "$@"
fi
if [ -z "$src" ] || [ ! -f "$src" ] || [ ! -x "$ENGINE" ] || ! command -v cksum >/dev/null 2>&1; then
    runInterp "$@"
fi

abs="$(cd "$(dirname "$src")" && pwd)/$(basename "$src")"
# Where compiled programs are kept. NusaCache wins; otherwise the first of these
# that can be written: the system's own cache folder (XDG on Linux, Library/Caches
# on macOS, LOCALAPPDATA on Windows shells), Termux's private tmp, then the
# temporary folder with a per-user name.
usable() {
    [ -n "$1" ] || return 1
    mkdir -p "$1" 2>/dev/null && [ -d "$1" ] && [ -w "$1" ]
}

pickCache() {
    if [ -n "$NusaCache" ]; then
        printf '%s' "$NusaCache"
        return
    fi
    system="$(uname -s 2>/dev/null)"
    who="$(id -u 2>/dev/null || echo user)"
    for dir in \
        "${XDG_CACHE_HOME:+$XDG_CACHE_HOME/nusa}" \
        "$(case "$system" in
            Darwin) [ -n "$HOME" ] && printf '%s' "$HOME/Library/Caches/nusa" ;;
            MINGW*|MSYS*|CYGWIN*) [ -n "$LOCALAPPDATA" ] && printf '%s' "$LOCALAPPDATA/nusa/cache" ;;
        esac)" \
        "${HOME:+$HOME/.cache/nusa}" \
        "${PREFIX:+$PREFIX/tmp/nusa}" \
        "${TMPDIR:+$TMPDIR/nusa-$who}" \
        "/tmp/nusa-$who" \
        "/data/local/tmp/nusa-$who" \
        "$DIR/.cache"; do
        if usable "$dir"; then
            printf '%s' "$dir"
            return
        fi
    done
    printf '%s' "/tmp/nusa-$who"
}

cache="$(pickCache)"
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
        explain "interpreter, $(head -n 1 "$bin.fail" 2>/dev/null)"
        runInterp "$@"
    fi
    mkdir -p "$cache" 2>/dev/null || runInterp "$@"
    tmp="$bin.$$"
    log="$tmp.log"
    if NusaDeps="$tmp.deps" "$ENGINE" "$DIR/core/main.ns" build "$src" -o "$tmp" >"$log" 2>&1 && [ -x "$tmp" ]; then
        unlink "$log" 2>/dev/null
        mv -f "$tmp.deps" "$bin.deps" 2>/dev/null
        mv -f "$tmp" "$bin"
        [ -e "$bin.fail" ] && unlink "$bin.fail"
    else
        reason="$(grep -m 1 -E '^(native|nusa|nusantara):|rror' "$log" 2>/dev/null | cut -c1-200)"
        [ -n "$reason" ] || reason="the native compiler could not build it"
        for leftover in "$tmp" "$tmp.s" "$tmp.o" "$tmp.b64" "$tmp.deps" "$log"; do
            [ -e "$leftover" ] && unlink "$leftover"
        done
        printf '%s\n' "$reason" >"$bin.fail"
        explain "interpreter, $reason"
        runInterp "$@"
    fi
fi
explain "native"
if [ -n "$whyOnly" ]; then
    exit 0
fi
shift $skip
exec "$bin" "$@"
