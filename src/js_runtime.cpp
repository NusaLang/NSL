// QuickJS-backed .js execution path. Independent engine, independent
// object model/GC from the rest of Nusantara -- see js_runtime.hpp.
#include "js_runtime.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include <unistd.h>

extern "C" {
#include "quickjs.h"
}

namespace jsrt {
namespace {

// Node console.log-style formatting: objects/arrays via JSON.stringify
// instead of the default "[object Object]".
std::string formatArg(JSContext* ctx, JSValueConst v) {
    if (JS_IsObject(v) && !JS_IsFunction(ctx, v)) {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json) && !JS_IsUndefined(json)) {
            const char* s = JS_ToCString(ctx, json);
            std::string result = s ? s : "";
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, json);
            return result;
        }
        JS_FreeValue(ctx, json);
        JS_GetException(ctx);  // JSON.stringify can't handle it (cycle, etc.) -- drop, fall through
    }
    const char* s = JS_ToCString(ctx, v);
    std::string result = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return result;
}

// console.log/info/warn/error -- all four map to the same "stringify each
// arg, join with a space, newline, flush" behaviour a CLI needs (matches
// how `cetak()` output reaches the terminal for .ns).
JSValue jsConsoleLog(JSContext* ctx, JSValueConst /*thisVal*/, int argc, JSValueConst* argv) {
    FILE* out = stdout;
    for (int i = 0; i < argc; i++) {
        if (i > 0) std::fputc(' ', out);
        std::fputs(formatArg(ctx, argv[i]).c_str(), out);
    }
    std::fputc('\n', out);
    std::fflush(out);
    return JS_UNDEFINED;
}

JSValue jsConsoleError(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    FILE* out = stderr;
    for (int i = 0; i < argc; i++) {
        if (i > 0) std::fputc(' ', out);
        std::fputs(formatArg(ctx, argv[i]).c_str(), out);
    }
    std::fputc('\n', out);
    std::fflush(out);
    (void)thisVal;
    return JS_UNDEFINED;
}

// Replaces the current process image with argv[0] via execvp() --
// same PID, lets a thin .js entry point hand off to a long-running
// program. Never returns on success; throws only if exec fails to start.
JSValue jsExecProcess(JSContext* ctx, JSValueConst /*thisVal*/, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsArray(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "execProcess(args): args must be an array of strings, e.g. ['nusa', 'main.ns']");
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    int64_t len = 0;
    JS_ToInt64(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    if (len < 1) return JS_ThrowTypeError(ctx, "execProcess(args): need at least one argument (the program name)");

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
        const char* s = JS_ToCString(ctx, el);
        args.emplace_back(s ? s : "");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, el);
    }

    std::vector<char*> cargs;
    cargs.reserve(args.size() + 1);
    for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    execvp(cargs[0], cargs.data());
    // Only reached if exec itself failed to start the new program.
    return JS_ThrowTypeError(ctx, "execProcess: exec of '%s' failed: %s", cargs[0], std::strerror(errno));
}

void installConsole(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, jsConsoleLog, "log", 1));
    JS_SetPropertyStr(ctx, console, "info", JS_NewCFunction(ctx, jsConsoleLog, "info", 1));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, jsConsoleError, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, jsConsoleError, "error", 1));
    JS_SetPropertyStr(ctx, console, "debug", JS_NewCFunction(ctx, jsConsoleLog, "debug", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "execProcess", JS_NewCFunction(ctx, jsExecProcess, "execProcess", 1));
    JS_FreeValue(ctx, global);
}

// Prints a JS exception (syntax or runtime) the same "loud, non-zero exit"
// way reportError() does for .ns errors -- doesn't need identical
// formatting, just needs to fail clearly instead of exiting 0 or crashing.
void reportJsException(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    std::cerr << "nusa: js error: " << (msg ? msg : "(unknown)") << '\n';
    if (msg) JS_FreeCString(ctx, msg);

    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* stackStr = JS_ToCString(ctx, stack);
        if (stackStr && stackStr[0] != '\0') {
            std::cerr << stackStr << '\n';
        }
        if (stackStr) JS_FreeCString(ctx, stackStr);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

// Drains the microtask/job queue (Promise reactions, async fn resumes)
// until it's empty or a job itself throws. Returns false on an
// unhandled job exception (already reported).
bool drainJobs(JSRuntime* rt, JSContext* ctx) {
    for (;;) {
        JSContext* jobCtx;
        int r = JS_ExecutePendingJob(rt, &jobCtx);
        if (r == 0) break;  // no more jobs
        if (r < 0) {
            reportJsException(jobCtx ? jobCtx : ctx);
            return false;
        }
    }
    return true;
}

}  // namespace

int runJsFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "nusa: error: could not open file '" << path << "'\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    JSRuntime* rt = JS_NewRuntime();
    if (!rt) {
        std::cerr << "nusa: js error: failed to init QuickJS runtime\n";
        return 1;
    }
    // Generous but bounded, so a runaway script fails loudly instead of
    // taking down the host process (this binary also runs live services).
    JS_SetMemoryLimit(rt, static_cast<size_t>(512) * 1024 * 1024);
    JS_SetMaxStackSize(rt, 8 * 1024 * 1024);

    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        std::cerr << "nusa: js error: failed to init QuickJS context\n";
        JS_FreeRuntime(rt);
        return 1;
    }

    installConsole(ctx);

    int exitCode = 0;
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        reportJsException(ctx);
        exitCode = 1;
    } else if (!drainJobs(rt, ctx)) {
        exitCode = 1;
    }
    JS_FreeValue(ctx, result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return exitCode;
}

}  // namespace jsrt
