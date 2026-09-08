#!/usr/bin/env bash
# Regression / golden test runner buat interpreter Nusantara.
#
#   tests/run.sh              -> banding output vs golden, exit != 0 kalau beda
#   tests/run.sh --update     -> tulis ulang semua golden (pakai binary yang dipilih)
#   NUSA=path/ke/nusa tests/run.sh
#
# SELALU dijalanin dari root repo (path di pesan error itu relatif ke CWD,
# jadi CWD ikut ke-golden). Runner ini pindah sendiri ke root repo.
set -u

cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)

NUSA=${NUSA:-$ROOT/build-opt/nusa}
GOLDEN=$ROOT/tests/golden
TIMEOUT=${TIMEOUT:-120}

UPDATE=0
FILTER=""
for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        --filter=*) FILTER="${arg#--filter=}" ;;
        *) echo "argumen nggak dikenal: $arg" >&2; exit 2 ;;
    esac
done

if [ ! -x "$NUSA" ]; then
    echo "FATAL: binary '$NUSA' nggak ada / nggak executable." >&2
    echo "Build dulu: make -f Makefile build-opt/nusa   (atau set NUSA=...)" >&2
    exit 2
fi

# Golden dibikin dengan bahasa Indonesia + TZ tetap, biar pesan error stabil.
export NUSA_LANG=ind
export TZ=UTC
export LC_ALL=C

mkdir -p "$GOLDEN"

PASS=0; FAIL=0; UPDATED=0
FAILED_NAMES=()

# run_case <nama-test> <cmd...>
run_case() {
    local name="$1"; shift
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then return; fi

    local actual
    actual=$(timeout "$TIMEOUT" "$@" 2>&1)
    local code=$?
    actual="$actual
--- exit: $code"

    local gf="$GOLDEN/$name.out"
    if [ "$UPDATE" = "1" ]; then
        printf '%s\n' "$actual" > "$gf"
        UPDATED=$((UPDATED+1))
        printf 'UPDATE  %s\n' "$name"
        return
    fi

    if [ ! -f "$gf" ]; then
        printf 'FAIL    %s  (golden hilang: %s)\n' "$name" "$gf"
        FAIL=$((FAIL+1)); FAILED_NAMES+=("$name"); return
    fi

    if printf '%s\n' "$actual" | diff -q - "$gf" >/dev/null 2>&1; then
        PASS=$((PASS+1))
        printf 'ok      %s\n' "$name"
    else
        FAIL=$((FAIL+1)); FAILED_NAMES+=("$name")
        printf 'FAIL    %s\n' "$name"
        printf '%s\n' "$actual" | diff -u "$gf" - | sed 's/^/        /' | head -40
    fi
}

# --- 1. test case bahasa (tests/cases/*.ns) ---------------------------------
for f in "$ROOT"/tests/cases/*.ns; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .ns)
    run_case "case_$base" "$NUSA" run "tests/cases/$base.ns"
done

# --- 1b. test case JavaScript asli (tests/cases_js/*.js), lewat QuickJS ----
# Jalur eksekusi ini sepenuhnya terpisah dari lexer/parser/interpreter .ns
# di atas -- lihat src/js_runtime.cpp -- tapi tetep lewat binary `nusa` yang
# sama, jadi dites lewat runner golden yang sama juga.
for f in "$ROOT"/tests/cases_js/*.js; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .js)
    run_case "js_$base" "$NUSA" run "tests/cases_js/$base.js"
done

# --- 2. contoh yang deterministik (examples/) -------------------------------
# gc_demo/http_client_demo/sqlite_demo/stream_demo sengaja nggak dipakai:
# angka statistik GC, jaringan, file DB, dan server yang nggak pernah exit.
DETERMINISTIC_EXAMPLES="base64_demo contoh fib goroutine_demo hello larik_dan_perulangan main modul_util plugin_demo template_demo tipe_ketat"
for name in $DETERMINISTIC_EXAMPLES; do
    [ -f "$ROOT/examples/$name.ns" ] || continue
    run_case "example_$name" "$NUSA" run "examples/$name.ns"
done

# --- 3. syntax check (nusa -c) atas examples/ -------------------------------
for f in "$ROOT"/examples/*.ns; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .ns)
    run_case "check_example_$base" "$NUSA" -c "examples/$base.ns"
done

# --- 4. backend VM eksperimental (nusa --vm) --------------------------------
# vm.cpp punya representasi Value-nya sendiri (VmArray/VmClosure) yang ikut
# kena refactor sizeof(Value), jadi jalur ini dites terpisah dari interpreter
# pohon-AST. Sebagian besar builtin belum ada di --vm; test-nya sengaja mepet
# ke subset yang emang jalan, plus satu kasus yang error-nya juga di-golden.
for f in "$ROOT"/tests/vm/*.ns; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .ns)
    run_case "vm_$base" "$NUSA" --vm "tests/vm/$base.ns"
done

# --- 5. eval satu baris + versi + flag CLI ----------------------------------
run_case "cli_eval_aritmatika" "$NUSA" -e 'cetak(1 + 2 * 3);'
run_case "cli_eval_string"     "$NUSA" -e 'cetak(huruf_besar("nusantara"));'
run_case "cli_eval_larik"      "$NUSA" -e 'buat a = [1,2,3]; tambah(a, 4); cetak(a);'
run_case "cli_eval_error"      "$NUSA" -e 'cetak(tidak_ada_ini);'
run_case "cli_perintah_asing"  "$NUSA" perintah-ngawur

echo
if [ "$UPDATE" = "1" ]; then
    echo "golden diperbarui: $UPDATED"
    exit 0
fi
echo "lulus: $PASS   gagal: $FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo "yang gagal: ${FAILED_NAMES[*]}"
    exit 1
fi
exit 0
