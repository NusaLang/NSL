#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#endif
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "gil.hpp"
#include "value.hpp"
#include "plugin_abi.h"

NativePlugin::~NativePlugin() {
#ifndef __EMSCRIPTEN__
    if (handle) dlclose(handle);
#endif
}

namespace plugin {

namespace {

// Collects name->NsFn registrations during one ns_plugin_init() call.
// The plugin only ever sees this through the opaque `void* registry` +
// registerTrampoline function pointer pair (plugin_abi.h), never this
// type directly -- keeps the ABI stable even if this changes shape.
struct PlugFn { NsFn fn; int ver = 1; };
using Registry = std::unordered_map<std::string, PlugFn>;

extern "C" void registerTrampoline(void* registry, const char* name, NsFn fn) {
    if (!registry || !name || !fn) return;
    (*static_cast<Registry*>(registry))[name] = PlugFn{fn};
}

NsValue toNs(const Value& v) {
    NsValue out{};
    out.str_len = -1;
    switch (v.type) {
        case ValueType::Null:
            out.type = NS_NULL;
            return out;
        case ValueType::Bool:
            out.type = NS_BOOL;
            out.boolean = v.boolean() ? 1 : 0;
            return out;
        case ValueType::Number:
            out.type = NS_NUMBER;
            out.number = v.number;
            return out;
        case ValueType::String:
            out.type = NS_STRING;
            // Borrowed pointer into v.str's own buffer -- valid for the
            // duration of the call since `v` (via the caller's `args`
            // vector) lives on the stack through the whole call. Plugin
            // must not retain or free() this (see plugin_abi.h).
            out.str = const_cast<char*>(v.str().c_str());
            out.str_len = static_cast<int>(v.str().size());
            return out;
        default:
            throw std::runtime_error(
                "plugin: cuma null/boolean/angka/teks yang bisa dikirim ke fungsi native -- "
                "json_encode() dulu buat larik/peta");
    }
}

Value fromNs(const NsValue& v, int abiVer) {
    switch (v.type) {
        case NS_NULL:
            return Value::null();
        case NS_BOOL:
            return Value::fromBool(v.boolean != 0);
        case NS_NUMBER:
            return Value::fromNumber(v.number);
        case NS_STRING:
            if (abiVer >= 2 && v.str) {
                if (v.str_len >= 0) return Value::fromString(std::string(v.str, static_cast<size_t>(v.str_len)));
                return Value::fromString(std::string(v.str));
            }
            return Value::fromString(v.str ? std::string(v.str) : std::string());
    }
    throw std::runtime_error("plugin: fungsi native balikin NsValue.type yang nggak dikenal");
}

}  // namespace

Value load(const std::string& path) {
#ifdef __EMSCRIPTEN__
    (void)path;
    throw std::runtime_error("muat_plugin(): plugin native tidak didukung di WASM browser");
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        throw std::runtime_error("muat_plugin(): gagal buka '" + path + "': " +
                                  (err ? err : "unknown dlopen error"));
    }

    dlerror();  // clear any pending error before dlsym, per dlsym(3)
    void* sym = dlsym(handle, "ns_plugin_init");
    if (!sym) {
        // Copy dlerror()'s message before dlclose() -- it can embed
        // data tied to the module (e.g. its path) and dangle right
        // after dlclose(), even though it looks fine until then.
        const char* errRaw = dlerror();
        std::string err = errRaw ? errRaw : "unknown dlsym error";
        dlclose(handle);
        throw std::runtime_error("muat_plugin(): '" + path + "' nggak punya ns_plugin_init: " + err);
    }

    auto pluginState = std::make_shared<NativePlugin>();
    pluginState->handle = handle;
    pluginState->path = path;

    Registry registry;
    auto init = reinterpret_cast<NsPluginInit>(sym);
    init(&registry, registerTrampoline);

    int abiVer = 1;
    if (void* vsym = dlsym(handle, "nusa_abi_version")) {
        abiVer = reinterpret_cast<int (*)()>(vsym)();
        if (abiVer < 1 || abiVer > NS_PLUGIN_ABI_VERSION) abiVer = 1;
    }
    for (auto& [n, pf] : registry) pf.ver = abiVer;

    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    for (const auto& [name, pf] : registry) {
        auto nf = std::make_shared<NativeFunction>();
        nf->plugin = pluginState;
        nf->fnPtr = reinterpret_cast<void*>(pf.fn);
        nf->name = name;
        nf->abiVer = pf.ver;
        (*m)[name] = Value::fromNative(nf);
    }
    return Value::fromMap(m);
#endif
}

Value call(const NativeFunction& fn, std::vector<Value>& args) {
    std::vector<NsValue> argv;
    argv.reserve(args.size());
    for (const Value& a : args) argv.push_back(toNs(a));

    NsValue result{};
    {
        // Plugin code never touches interpreter/GC state, so this is a
        // safe GilRelease boundary -- other goroutines run while it's in flight.
        GilRelease release;
        auto call = reinterpret_cast<NsFn>(fn.fnPtr);
        result = call(static_cast<int>(argv.size()), argv.data());
    }

    Value out = fromNs(result, fn.abiVer);
    if (result.type == NS_STRING && result.str) free(result.str);
    return out;
}

}  // namespace plugin
