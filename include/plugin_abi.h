#ifndef NUSANTARA_PLUGIN_ABI_H
#define NUSANTARA_PLUGIN_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable, minimal C ABI between the `nusa` core binary and native
 * plugins (.so files, loaded at runtime via dlopen()/muat_plugin()).
 *
 * Deliberately tiny: only null/bool/number/string cross this boundary
 * directly. Arrays/maps go across as JSON text instead -- the plugin
 * side can use any JSON encoder it wants to build that text, and the
 * Nusantara side decodes it with the existing json_decode() builtin.
 * Keeping the boundary this narrow means the core `nusa` binary never
 * needs to link against (or even know about) a given plugin's own
 * dependencies -- e.g. the SQLite plugin links -lsqlite3 for ITSELF
 * only, `nusa` stays zero-external-dependency. See the language
 * README's "Plugin (native modules)" section for how this is used
 * from a .ns file, and plugin.hpp for the host-side loader.
 */
#define NS_PLUGIN_ABI_VERSION 2

typedef enum {
    NS_NULL = 0,
    NS_BOOL = 1,
    NS_NUMBER = 2,
    NS_STRING = 3,
} NsType;

/*
 * Argument values (host -> plugin, passed into an NsFn call): `str`
 * is BORROWED, valid only for the duration of the call. Do not free()
 * it and do not retain the pointer past the call returning.
 *
 * Return values (plugin -> host, returned from an NsFn call): if
 * `type == NS_STRING`, `str` MUST be heap-allocated with plain libc
 * malloc/strdup (both the host and every plugin dynamically link the
 * same libc, so this is safe on Linux/Android -- no separate
 * allocator/CRT-heap mismatch to worry about here). Ownership
 * transfers to the host, which free()s it right after copying the
 * contents out. Never return a pointer to a string literal or a stack
 * buffer as NS_STRING -- the host WILL call free() on it.
 */
typedef struct {
    NsType type;
    int boolean;
    double number;
    char* str;
    /* ABI v2: panjang str eksplisit (binary-safe, boleh mengandung \0).
     * Plugin v1 tidak mengisi field ini -- host memperlakukan sebagai
     * strlen() seperti dulu. Isi SELALU di sisi yang MENGINISIASI str:
     * host saat membuat argumen, plugin saat mengembalikan NS_STRING. */
    int str_len;
} NsValue;

/* A single native function exposed by a plugin. */
typedef NsValue (*NsFn)(int argc, const NsValue* argv);

/*
 * Called by a plugin's ns_plugin_init once per function name it wants
 * to expose to Nusantara code. `registry` is the opaque pointer
 * ns_plugin_init itself received -- pass it straight through, never
 * dereference it (its real type is a host-internal implementation
 * detail, not part of this ABI).
 */
typedef void (*NsRegisterFn)(void* registry, const char* name, NsFn fn);

/*
 * Every plugin .so MUST export exactly this symbol, with exactly this
 * name (the host looks it up with dlsym(handle, "ns_plugin_init")) and
 * signature, declared `extern "C"` if the plugin is written in C++
 * (otherwise C++ name-mangling hides it from dlsym).
 *
 * Called exactly once, immediately after a successful dlopen(), on
 * whichever thread called muat_plugin() -- with the Nusantara GIL
 * held. It should just register functions and return quickly: there
 * is no Nusantara/GC/GIL API exposed across this boundary to call
 * back into, and blocking here blocks the whole interpreter (same as
 * blocking inside any other builtin while holding the GIL).
 *
 * Minimal example plugin (see the SQLite plugin under plugins/ for a
 * complete one):
 *
 *   #include "plugin_abi.h"
 *   #include <string.h>
 *   #include <stdlib.h>
 *
 *   static NsValue hello(int argc, const NsValue* argv) {
 *       (void)argc; (void)argv;
 *       NsValue v; v.type = NS_STRING; v.boolean = 0; v.number = 0;
 *       v.str = strdup("halo dari plugin native!");
 *       return v;
 *   }
 *
 *   extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
 *       reg(registry, "hello", hello);
 *   }
 */
typedef void (*NsPluginInit)(void* registry, NsRegisterFn register_fn);

#ifdef __cplusplus
}
#endif

#endif /* NUSANTARA_PLUGIN_ABI_H */

/* Plugin ABI v2 wajib (opsional tapi disarankan) mengekspor versi:
 *   int nusa_abi_version(void) { return NS_PLUGIN_ABI_VERSION; }
 * Tanpa simbol ini, host memperlakukan plugin sebagai ABI v1. */
