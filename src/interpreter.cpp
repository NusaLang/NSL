#include "interpreter.hpp"
#include "pynum.hpp"
#include "methods.hpp"
#include "value_eq.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#if !defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

// Goroutine stack size (default glibc thread stack is 8 MB virtual).
static const size_t kUkuranStackGoroutine = 512 * 1024;


#include <sys/stat.h>
#include <unistd.h>

#include "base64.hpp"
#include "gc.hpp"
#include "gil.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include "jwt.hpp"
#include "proc.hpp"
#include "sha256.hpp"
#include "lexer.hpp"
#include "net.hpp"
#include "parser.hpp"
#include "plugin.hpp"
#include "plugin_abi.h"
#include "sysmod.hpp"
#include "pylib.hpp"
#include "varargs.hpp"
#include "pystd.hpp"
#include "repeat.hpp"
#include "sysplugin.hpp"
#include "vm.hpp"

namespace {

// Used internally to unwind out of a function body on `hasil`/
// `berhenti`/`lanjut`.
// return/break/continue unwind via a pending flag checked by execBlock and
// the loops, not C++ exceptions (a throw costs microseconds per `hasil`).
enum : int { kPendNone = 0, kPendReturn, kPendBreak, kPendContinue };
static thread_local int g_pending = kPendNone;
static thread_local Value g_retVal;

// RAII tracker for Interpreter::exprDepth_ -- see interpreter.hpp and
// gc.hpp for why the GC needs to know whether we're mid-expression.
struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { depth++; }
    ~DepthGuard() { depth--; }
    DepthGuard(const DepthGuard&) = delete;
};

// Resets exprDepth_ to 0 around a called function's body, restores it on
// the way out (see gc.hpp) -- lets a long-running/never-returning
// function still get periodic GC safepoints.
struct DepthResetGuard {
    int& depth;
    int saved;
    explicit DepthResetGuard(int& d) : depth(d), saved(d) { d = 0; }
    ~DepthResetGuard() { depth = saved; }
    DepthResetGuard(const DepthResetGuard&) = delete;
};

bool valuesEqual(const Value& a, const Value& b) { return valuesDeepEqual(a, b); }

std::shared_ptr<Function> lookupMethod(const std::shared_ptr<ClassInfo>& start, const std::string& name,
                                        std::shared_ptr<ClassInfo>* ownerOut = nullptr) {
    for (std::shared_ptr<ClassInfo> c = start; c; c = c->parent) {
        auto it = c->methods.find(name);
        if (it != c->methods.end()) {
            if (ownerOut) *ownerOut = c;
            return it->second;
        }
    }
    return nullptr;
}

Interpreter* g_propInterpreter = nullptr;  // set by the constructor: property getters/setters run through it

Value indexGet(const Value& target, const Value& idx) {
    if (target.type == ValueType::VmArray) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        VmArrayState& st = *target.vmArray();
        size_t size = st.numeric ? st.nums.size() : st.boxed->size();
        if (i < 0) i += static_cast<long long>(size);  // Python: -1 is the last element
        if (i < 0 || static_cast<size_t>(i) >= size) {
            throw RuntimeError(i18n::tr("Index larik di luar batas: ", "Array index out of bounds: ") + std::to_string(i));
        }
        return st.numeric ? Value::fromNumber(st.nums[static_cast<size_t>(i)])
                          : (*st.boxed)[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Array) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        if (i < 0) i += static_cast<long long>(target.array()->size());
        if (i < 0 || static_cast<size_t>(i) >= target.array()->size()) {
            throw RuntimeError(i18n::tr("Index larik di luar batas: ", "Array index out of bounds: ") + std::to_string(i));
        }
        return (*target.array())[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Map) {
        std::string key = idx.type == ValueType::String ? idx.str() : idx.stringify();
        auto it = target.map()->find(key);
        if (it == target.map()->end()) {
            if (target.map()->deflt && g_propInterpreter) {  // defaultdict / Counter: create the missing entry
                std::vector<Value> none;
                Value f = *target.map()->deflt;
                Value made = vmIsActive() ? vmCallValue(f, none, g_propInterpreter) : g_propInterpreter->callValue(f, none, Span{});
                (*target.map())[key] = made;
                GC::instance().noteStore(target, made);
                return made;
            }
            return Value::null();
        }
        return it->second;
    }
    if (target.type == ValueType::String) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index teks harus angka", "String index must be a number"));
        long long i = static_cast<long long>(idx.number);
        if (i < 0) i += static_cast<long long>(target.str().size());
        if (i < 0 || static_cast<size_t>(i) >= target.str().size()) {
            throw RuntimeError(i18n::tr("Index teks di luar batas: ", "String index out of bounds: ") + std::to_string(i));
        }
        return Value::fromString(std::string(1, target.str()[static_cast<size_t>(i)]));
    }
    if (target.type == ValueType::Instance) {
        if (idx.type != ValueType::String) {
            std::shared_ptr<ClassInfo> owner;
            auto gi = lookupMethod(target.instance()->classInfo, "__getitem__", &owner);
            if (!gi || !g_propInterpreter) throw RuntimeError(i18n::tr("Kunci objek harus teks", "Object key must be a string"));
            std::vector<Value> a{idx};
            Value self = target;
            return g_propInterpreter->callFunction(gi, a, Span{}, &self, owner);
        }
        auto fit = target.instance()->fields->find(idx.str());
        if (fit != target.instance()->fields->end()) return fit->second;
        std::shared_ptr<ClassInfo> owner;
        auto method = lookupMethod(target.instance()->classInfo, idx.str(), &owner);
        if (method && target.instance()->classInfo->hasSpecial && methodKindOf(target.instance()->classInfo.get(), idx.str()) == 3 && g_propInterpreter) {
            std::vector<Value> none;
            Value self = target;
            return g_propInterpreter->callFunction(method, none, Span{}, &self, owner);
        }
        if (method) return Value::fromFunction(method);
        if (Value* attr = classAttrOf(target.instance()->classInfo.get(), idx.str())) return *attr;
        if (idx.str() == "__class__") return Value::fromClass(target.instance()->classInfo);
        return Value::null();
    }
    if (target.type == ValueType::Class && idx.type == ValueType::String) {
        if (idx.str() == "__name__") return Value::fromString(target.klass()->name);
        if (Value* attr = classAttrOf(target.klass(), idx.str())) return *attr;
        auto method = lookupMethod(target.klassShared(), idx.str());
        if (method) return Value::fromFunction(method);
        return Value::null();
    }
    throw RuntimeError(std::string(i18n::tr("Tipe '", "Type '")) + target.typeName() +
                        i18n::tr("' nggak bisa di-index pakai []", "' can't be indexed with []"));
}

void indexSet(Value& target, const Value& idx, const Value& value) {
    if (target.type == ValueType::VmArray) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        VmArrayState& st = *target.vmArray();
        size_t size = st.numeric ? st.nums.size() : st.boxed->size();
        if (i < 0) i += static_cast<long long>(size);  // Python: -1 is the last element
        if (i < 0) throw RuntimeError(i18n::tr("Index larik negatif nggak valid: ", "Negative array index is invalid: ") + std::to_string(i));
        bool needsGrow = static_cast<size_t>(i) >= size;
        if (st.numeric && value.type == ValueType::Number && !needsGrow) {
            st.nums[static_cast<size_t>(i)] = value.number;
        } else {
            if (st.numeric) {
                st.boxed = std::make_shared<std::vector<Value>>();
                st.boxed->reserve(st.nums.size());
                for (double d : st.nums) st.boxed->push_back(Value::fromNumber(d));
                st.numeric = false;
                st.nums.clear();
                st.nums.shrink_to_fit();
            }
            if (needsGrow) {
                st.boxed->resize(static_cast<size_t>(i) + 1, Value::null());
            }
            (*st.boxed)[static_cast<size_t>(i)] = value;
        }
        return;
    }
    if (target.type == ValueType::Array) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        if (i < 0) i += static_cast<long long>(target.array()->size());
        if (i < 0) throw RuntimeError(i18n::tr("Index larik negatif nggak valid: ", "Negative array index is invalid: ") + std::to_string(i));
        auto& vec = *target.array();
        if (static_cast<size_t>(i) >= vec.size()) vec.resize(static_cast<size_t>(i) + 1);
        vec[static_cast<size_t>(i)] = value;
        return;
    }
    if (target.type == ValueType::Map) {
        (*target.map())[idx.type == ValueType::String ? idx.str() : idx.stringify()] = value;
        return;
    }
    if (target.type == ValueType::Class) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Nama atribut kelas harus teks", "Class attribute name must be a string"));
        target.klass()->classAttrs[idx.str()] = value;
        return;
    }
    if (target.type == ValueType::Instance) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Kunci objek harus teks", "Object key must be a string"));
        if (target.instance()->classInfo->hasSpecial && g_propInterpreter &&
            methodKindOf(target.instance()->classInfo.get(), "__set_" + idx.str()) == 4) {
            std::shared_ptr<ClassInfo> owner;
            auto setter = lookupMethod(target.instance()->classInfo, "__set_" + idx.str(), &owner);
            if (setter) {
                std::vector<Value> a{value};
                g_propInterpreter->callFunction(setter, a, Span{}, &target, owner);
                return;
            }
        }
        (*target.instance()->fields)[idx.str()] = value;
        return;
    }
    throw RuntimeError(std::string(i18n::tr("Tipe '", "Type '")) + target.typeName() +
                        i18n::tr("' nggak bisa di-assign pakai []", "' can't be assigned with []"));
}

// Lexically collapses "." and ".." segments (no disk access), so
// moduleCache_ treats the same file as the same module regardless of
// which relative path reached it. Preserves a leading "/".
std::string collapseDotSegments(const std::string& path) {
    bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() || segment == ".") {
            // skip
        } else if (segment == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back("..");  // leading ".." on a relative path -- keep it, nothing to pop
            }
        } else {
            parts.push_back(segment);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += "/";
        out += parts[i];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

// Resolves `raw` against `dir` (the importing file's own directory, not
// cwd) the way impor() resolves its argument; absolute paths pass through.
std::string joinPath(const std::string& dir, const std::string& raw) {
    if (!raw.empty() && raw[0] == '/') return collapseDotSegments(raw);
    if (dir.empty() || dir == ".") return collapseDotSegments(raw);
    std::string joined = (dir.back() == '/') ? (dir + raw) : (dir + "/" + raw);
    return collapseDotSegments(joined);
}

std::string dirName(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

// ifstream(path).good() is true for directories too -- need this to
// tell them apart before falling through to <nama>/index.ns.
bool isRegularFile(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Bare module name check + system-plugin directory now live in
// sysplugin.hpp, shared with main.cpp (`nusa get` needs the http
// plugin's location too, for HTTPS registry calls).

// Canonical (Indonesian) builtin names registered into `globals_` at
// startup -- kept as one list so adding a new builtin only means: add
// it here, add its `if (name == ...)` branch in callBuiltin.
const std::vector<std::string>& builtinNames() {
    static const std::vector<std::string> names = {
        "cetak", "panjang", "tambah", "hapus_akhir", "potong", "gabung", "pisah",
        "huruf_besar", "huruf_kecil", "ke_teks", "ke_angka", "tipe", "waktu", "tidur", "latar", "iter", "next", "_callmeth", "_with_enter", "_with_exit", "getattr", "setattr", "hasattr", "delattr", "vars", "dir", "id", "hash", "issubclass", "__get", "_exc_match", "_exc_wrap", "_defaultdict", "_namedtuple", "_gennew", "_genresume", "_genclose", "_isvmgen", "pegang", "_peta", "_in", "_callkw", "_callkwm", "_close", "_go",
        "base64_encode", "base64_decode",
        "baca_file", "tulis_file", "file_ada",
        "tcp_konek", "tcp_kirim", "tcp_terima", "tcp_tutup",
        "http_get", "http_post", "http_dengar", "email_kirim",
        "qr_baca",
        "json_encode", "json_decode", "json_decode_aman", "peta_baru", "peta_kunci", "ambil_env",
        "jalan", "kanal_baru", "kanal_kirim", "kanal_terima", "kanal_tutup", "kanal_panjang",
        "jalankan_perintah", "proc_stream_mulai", "proc_stream_baca", "proc_stream_tutup",
        "wg_baru", "wg_tambah", "wg_selesai", "wg_tunggu", "pilih_kanal",
        "sha256_hex", "hmac_sha256_hex", "jwt_buat", "jwt_verifikasi",
        "muat_plugin",
        "gc_info", "gc_paksa", "impor",
        "byte_di", "teks_dari",
        "elemen", "teks", "baca_input", "input",
        "rentang", "__iter", "__iris",
    };
    return names;
}

// English aliases -> canonical name, registered as separate globals
// pointing at the same builtin dispatch; callBuiltin only sees canonical names.
const std::vector<std::pair<std::string, std::string>>& builtinAliases() {
    static const std::vector<std::pair<std::string, std::string>> aliases = {
        {"print", "cetak"}, {"length", "panjang"}, {"push", "tambah"}, {"pop", "hapus_akhir"},
        {"slice", "potong"}, {"join", "gabung"}, {"split", "pisah"},
        {"upper", "huruf_besar"}, {"lower", "huruf_kecil"},
        {"to_string", "ke_teks"}, {"to_number", "ke_angka"}, {"typeof", "tipe"}, {"time", "waktu"},
        {"read_file", "baca_file"}, {"write_file", "tulis_file"}, {"file_exists", "file_ada"},
        {"tcp_connect", "tcp_konek"}, {"tcp_send", "tcp_kirim"}, {"tcp_recv", "tcp_terima"},
        {"tcp_close", "tcp_tutup"}, {"send_email", "email_kirim"}, {"gc_collect", "gc_paksa"},
        {"import", "impor"}, {"new_map", "peta_baru"}, {"keys", "peta_kunci"}, {"env", "ambil_env"},
        {"json_decode_safe", "json_decode_aman"},
        {"byte_at", "byte_di"}, {"char_from", "teks_dari"},
        {"go", "jalan"}, {"channel", "kanal_baru"}, {"chan_send", "kanal_kirim"},
        {"chan_recv", "kanal_terima"}, {"chan_close", "kanal_tutup"}, {"chan_len", "kanal_panjang"},
        {"run_command", "jalankan_perintah"},
        {"proc_stream_start", "proc_stream_mulai"}, {"proc_stream_read", "proc_stream_baca"},
        {"proc_stream_close", "proc_stream_tutup"},
        {"wg_new", "wg_baru"}, {"wg_add", "wg_tambah"}, {"wg_done", "wg_selesai"}, {"wg_wait", "wg_tunggu"},
        {"jwt_create", "jwt_buat"}, {"jwt_verify", "jwt_verifikasi"},
        {"select", "pilih_kanal"},
        {"load_plugin", "muat_plugin"},
        {"len", "panjang"}, {"str", "ke_teks"}, {"float", "ke_angka"}, {"type", "tipe"},
        {"append", "tambah"}, {"range", "rentang"},
    };
    return aliases;
}

}  // namespace

alignas(64) thread_local int Interpreter::exprDepth_ = 0;
alignas(64) thread_local int Interpreter::callDepth_ = 0;

void Interpreter::registerCurrentThread() { GC::instance().registerThread(&exprDepth_); }
void Interpreter::unregisterCurrentThread() { GC::instance().unregisterThread(&exprDepth_); }

// inst.name(args) when the class has that method (VM or tree-walker); false otherwise.
static bool callInstMethod(Interpreter* in, const Value& inst, const char* name, std::vector<Value>& args, Value* out) {
    if (inst.type != ValueType::Instance) return false;
    auto ci = inst.instance()->classInfo;
    std::shared_ptr<ClassInfo> owner;
    auto m = lookupMethod(ci, name, &owner);
    if (vmIsActive()) {
        std::vector<std::string> names;
        if (!m && !vmMethodParamNames(ci.get(), name, names)) return false;
        Value o = inst;
        *out = vmCallMethod(o, name, args, in);
        return true;
    }
    if (!m) return false;
    Value self = inst;
    *out = in->callFunction(m, args, Span{}, &self, owner);
    return true;
}

Interpreter::Interpreter(std::string entryDir) {
    g_propInterpreter = this;
    instanceStrHook() = [this](const Value& inst, std::string& out) {
        for (const char* m : {"__str__", "__repr__"}) {
            std::vector<Value> none;
            Value r;
            if (callInstMethod(this, inst, m, none, &r)) {
                out = r.type == ValueType::String ? r.str() : r.stringify();
                return true;
            }
        }
        return false;
    };
    instanceOpHook() = [this](const char* dunder, const Value& a, const Value& b, Value& out) {
        std::vector<Value> args{b};
        if (callInstMethod(this, a, dunder, args, &out)) return true;
        // comparisons fall back to the reflected operation on the right operand: a > b == b < a
        static const std::pair<const char*, const char*> reflected[] = {
            {"__gt__", "__lt__"}, {"__lt__", "__gt__"}, {"__ge__", "__le__"}, {"__le__", "__ge__"}};
        for (const auto& [from, to] : reflected) {
            if (std::string(dunder) == from && b.type == ValueType::Instance) {
                std::vector<Value> rargs{a};
                return callInstMethod(this, b, to, rargs, &out);
            }
        }
        return false;
    };
    instanceBoolHook() = [this](const Value& inst) {
        std::vector<Value> none;
        Value r;
        if (callInstMethod(this, inst, "__bool__", none, &r)) return r.truthy();
        if (callInstMethod(this, inst, "__len__", none, &r)) return r.type == ValueType::Number ? r.number != 0.0 : r.truthy();
        return true;
    };
    pylib::setMethodHook([this](const Value& inst, const char* name, std::vector<Value>& args, Value* out) {
        return callInstMethod(this, inst, name, args, out);
    });
    globals_ = GC::instance().alloc(nullptr);
    GC::instance().setGlobals(globals_);
    for (const std::string& name : builtinNames()) {
        globals_->define(name, Value::builtin(name));
    }
    for (const std::string& name : pylib::builtinNames()) {
        globals_->define(name, Value::builtin(name));
    }
    for (const std::string& name : pystd::builtinNames()) {
        globals_->define(name, Value::builtin(name));
    }
    globals_->define("open", Value::builtin("open"));
    for (const auto& [aliasName, canonical] : builtinAliases()) {
        globals_->define(aliasName, Value::builtin(canonical));
    }
    importDirStack_.push_back(std::move(entryDir));
}

void Interpreter::run(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (exprDepth_ == 0) GC::instance().collectIfNeeded();
        exec(stmt.get(), globals_);
        if (g_pending != kPendNone) { g_pending = kPendNone; break; }  // top-level `hasil` ends the program
    }
    if (exprDepth_ == 0) GC::instance().collectIfNeeded();
}

Value Interpreter::evalGlobal(const Expr* expr) {
    if (exprDepth_ == 0) GC::instance().collectIfNeeded();
    return eval(expr, globals_);
}

// ---- statements ----

void Interpreter::exec(const Stmt* stmt, Environment* env) {
    try {
        execInner(stmt, env);
    } catch (RuntimeError& e) {
        e.attachLocation(stmt->span);
        throw;
    }
}

void Interpreter::execInner(const Stmt* stmt, Environment* env) {
    switch (stmt->kind) {
        case StmtKind::Let: {
            auto* node = static_cast<const LetStmt*>(stmt);
            env->define(node->name, eval(node->value.get(), env));
            return;
        }
        case StmtKind::FnDecl: {
            auto* node = static_cast<const FnDeclStmt*>(stmt);
            auto fn = std::make_shared<Function>();
            fn->decl = node;
            fn->closure = env;
            env->define(node->name, Value::fromFunction(fn));
            return;
        }
        case StmtKind::ClassDecl: {
            auto* node = static_cast<const ClassDeclStmt*>(stmt);
            auto info = std::make_shared<ClassInfo>();
            info->name = node->name;
            if (!node->parentName.empty()) {
                Value parentVal = env->get(node->parentName);
                if (parentVal.type != ValueType::Class) {
                    throw RuntimeError(i18n::tr("'", "'") + node->parentName +
                                        i18n::tr("' bukan kelas, nggak bisa di-turunan", "' is not a class, can't extend it"));
                }
                info->parent = parentVal.klassShared();
            }
            for (const auto& m : node->methods) {
                auto fn = std::make_shared<Function>();
                fn->decl = m.get();
                fn->closure = env;
                info->methods[m->name] = fn;
                if (m->kind) {
                    info->methodKind[m->name] = static_cast<uint8_t>(m->kind);
                    info->hasSpecial = true;
                }
            }
            if (info->parent && info->parent->hasSpecial) info->hasSpecial = true;
            env->define(node->name, Value::fromClass(info));
            return;
        }
        case StmtKind::StructDecl: {
            auto* node = static_cast<const StructDeclStmt*>(stmt);
            auto info = std::make_shared<ClassInfo>();
            info->name = node->name;
            info->isStruct = true;
            info->structFields = node->fields;
            env->define(node->name, Value::fromClass(info));
            return;
        }
        case StmtKind::EnumDecl: {
            auto* node = static_cast<const EnumDeclStmt*>(stmt);
            auto info = std::make_shared<ClassInfo>();
            info->name = node->name;
            info->isEnum = true;
            auto ns = std::make_shared<ValueMap>();
            for (const auto& variant : node->variants) {
                auto state = std::make_shared<InstanceState>();
                state->classInfo = info;
                state->fields = std::make_shared<ValueMap>();
                (*state->fields)["nama"] = Value::fromString(variant);
                (*ns)[variant] = Value::fromInstance(state);
            }
            env->define(node->name, Value::fromMap(ns));
            return;
        }
        case StmtKind::Try: {
            auto* node = static_cast<const TryStmt*>(stmt);
            try {
                try {
                    Environment* child = GC::instance().alloc(env);
                    GcRootGuard guard(child);
                    execBlock(node->tryBlock.get(), child);
                } catch (RuntimeError& e) {
                    Value caught;
                    if (auto* tv = dynamic_cast<ThrownValue*>(&e)) {
                        caught = tv->value();
                    } else {
                        auto m = std::make_shared<ValueMap>();
                        (*m)["pesan"] = Value::fromString(e.what());
                        caught = Value::fromMap(m);
                    }
                    Environment* child = GC::instance().alloc(env);
                    GcRootGuard guard(child);
                    child->define(node->catchVar, caught);
                    execBlock(node->catchBlock.get(), child);
                }
            } catch (...) {
                if (node->finallyBlock) {
                    Environment* child = GC::instance().alloc(env);
                    GcRootGuard guard(child);
                    execBlock(node->finallyBlock.get(), child);
                }
                throw;
            }
            if (node->finallyBlock) {
                // A return/break/continue in flight must survive the finally
                // block, unless the finally block itself diverts control.
                int savedPending = g_pending;
                Value savedRet = std::move(g_retVal);
                g_pending = kPendNone;
                Environment* child = GC::instance().alloc(env);
                GcRootGuard guard(child);
                execBlock(node->finallyBlock.get(), child);
                if (g_pending == kPendNone) {
                    g_pending = savedPending;
                    g_retVal = std::move(savedRet);
                }
            }
            return;
        }
        case StmtKind::Throw: {
            auto* node = static_cast<const ThrowStmt*>(stmt);
            Value thrown = eval(node->value.get(), env);
            if (thrown.type == ValueType::Class) {  // raise ValueError  ==  raise ValueError()
                std::vector<Value> none;
                thrown = callValue(thrown, none, node->span);
            }
            throw ThrownValue(std::move(thrown));
        }
        case StmtKind::Block: {
            auto* node = static_cast<const BlockStmt*>(stmt);
            Environment* child = GC::instance().alloc(env);
            GcRootGuard guard(child);
            execBlock(node, child);
            return;
        }
        case StmtKind::If: {
            auto* node = static_cast<const IfStmt*>(stmt);
            if (eval(node->condition.get(), env).truthy()) {
                Environment* child = GC::instance().alloc(env);
                GcRootGuard guard(child);
                execBlock(node->thenBranch.get(), child);
            } else if (node->elseBranch) {
                Environment* child = GC::instance().alloc(env);
                GcRootGuard guard(child);
                execBlock(node->elseBranch.get(), child);
            }
            return;
        }
        case StmtKind::While: {
            auto* node = static_cast<const WhileStmt*>(stmt);
            while (eval(node->condition.get(), env).truthy()) {
                Environment* child = GC::instance().alloc(env);
                GcRootGuard guard(child);
                execBlock(node->body.get(), child);
                if (g_pending != kPendNone) {
                    if (g_pending == kPendBreak) { g_pending = kPendNone; break; }
                    if (g_pending == kPendContinue) { g_pending = kPendNone; continue; }
                    return;  // kPendReturn keeps unwinding
                }
            }
            return;
        }
        case StmtKind::For: {
            auto* node = static_cast<const ForStmt*>(stmt);
            Environment* loopEnv = GC::instance().alloc(env);
            GcRootGuard loopGuard(loopEnv);
            if (node->init) exec(node->init.get(), loopEnv);
            while (!node->condition || eval(node->condition.get(), loopEnv).truthy()) {
                Environment* iterEnv = GC::instance().alloc(loopEnv);
                GcRootGuard iterGuard(iterEnv);
                execBlock(node->body.get(), iterEnv);
                if (g_pending != kPendNone) {
                    if (g_pending == kPendBreak) { g_pending = kPendNone; break; }
                    if (g_pending == kPendContinue) g_pending = kPendNone;  // still run post, like C's `for`
                    else return;  // kPendReturn keeps unwinding
                }
                if (node->post) eval(node->post.get(), loopEnv);
            }
            return;
        }
        case StmtKind::Return: {
            auto* node = static_cast<const ReturnStmt*>(stmt);
            Value value = node->value ? eval(node->value.get(), env) : Value::null();
            g_retVal = std::move(value);
            g_pending = kPendReturn;
            return;
        }
        case StmtKind::Break:
            g_pending = kPendBreak;
            return;
        case StmtKind::Continue:
            g_pending = kPendContinue;
            return;
        case StmtKind::ExprStmt: {
            auto* node = static_cast<const ExprStmtNode*>(stmt);
            eval(node->expr.get(), env);
            return;
        }
    }
}

void Interpreter::execBlock(const BlockStmt* block, Environment* env) {
    for (const auto& stmt : block->statements) {
        if (exprDepth_ == 0) GC::instance().collectIfNeeded();
        exec(stmt.get(), env);
        if (g_pending != kPendNone) return;  // g_retVal is unrooted: no GC while unwinding
    }
    if (exprDepth_ == 0) GC::instance().collectIfNeeded();
}

// ---- expressions ----

Value Interpreter::eval(const Expr* expr, Environment* env) {
    DepthGuard depthGuard(exprDepth_);
    try {
        return evalInner(expr, env);
    } catch (RuntimeError& e) {
        e.attachLocation(expr->span);
        throw;
    }
}

Value Interpreter::evalInner(const Expr* expr, Environment* env) {
    switch (expr->kind) {
        case ExprKind::Literal: {
            auto* node = static_cast<const LiteralExpr*>(expr);
            switch (node->litKind) {
                case LiteralExpr::Kind::Number: return Value::fromNumber(node->number);
                case LiteralExpr::Kind::String: return Value::fromString(node->str);
                case LiteralExpr::Kind::Bool: return Value::fromBool(node->boolean);
                case LiteralExpr::Kind::Null: return Value::null();
            }
            return Value::null();
        }
        case ExprKind::Identifier: {
            auto* node = static_cast<const IdentifierExpr*>(expr);
            return env->get(node->name);
        }
        case ExprKind::Assign: {
            auto* node = static_cast<const AssignExpr*>(expr);
            Value value = eval(node->value.get(), env);
            env->assign(node->name, value);
            return value;
        }
        case ExprKind::Unary: {
            auto* node = static_cast<const UnaryExpr*>(expr);
            Value val = eval(node->operand.get(), env);
            if (node->op == "-") {
                if (val.type != ValueType::Number) {
                    throw RuntimeError(i18n::tr("Operand '-' harus angka", "Operand of '-' must be a number"));
                }
                return Value::fromNumber(-val.number);
            }
            if (node->op == "!") {
                return Value::fromBool(!val.truthy());
            }
            throw RuntimeError(i18n::tr("Operator unary nggak dikenal ", "Unknown unary operator ") + node->op);
        }
        case ExprKind::Binary: {
            auto* node = static_cast<const BinaryExpr*>(expr);
            const std::string& op = node->op;

            if (op == "?") {  // cond ? (then : else)
                Value cond = eval(node->left.get(), env);
                auto* branches = static_cast<const BinaryExpr*>(node->right.get());
                return eval(cond.truthy() ? branches->left.get() : branches->right.get(), env);
            }
            if (op == "&&") {
                Value left = eval(node->left.get(), env);
                if (!left.truthy()) return left;
                return eval(node->right.get(), env);
            }
            if (op == "||") {
                Value left = eval(node->left.get(), env);
                if (left.truthy()) return left;
                return eval(node->right.get(), env);
            }

            Value left = eval(node->left.get(), env);
            ValueRootGuard leftGuard(left);
            Value right = eval(node->right.get(), env);

            if (op == "==") return Value::fromBool(valuesEqual(left, right));
            if (op == "!=") return Value::fromBool(!valuesEqual(left, right));

            if (op == "+") {
                if (left.type == ValueType::Number && right.type == ValueType::Number) {
                    return Value::fromNumber(left.number + right.number);
                }
                if (left.type == ValueType::String && right.type == ValueType::String) {
                    return Value::fromString(left.str() + right.str());
                }
                if (left.type == ValueType::Instance) {
                    Value r;
                    if (instanceOpHook()("__add__", left, right, r)) return r;
                }
                if ((left.type == ValueType::Array || left.type == ValueType::VmArray) && (right.type == ValueType::Array || right.type == ValueType::VmArray)) {
                    std::vector<Value> pa{left, right};
                    return pylib::call("_concat", pa, nullptr, nullptr);
                }
                throw RuntimeError(i18n::tr("Operand '+' harus dua angka atau dua teks", "Operands of '+' must be two numbers or two strings"));
            }

            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                if (left.type == ValueType::Number && right.type == ValueType::Number) {
                    if (op == "<") return Value::fromBool(left.number < right.number);
                    if (op == "<=") return Value::fromBool(left.number <= right.number);
                    if (op == ">") return Value::fromBool(left.number > right.number);
                    return Value::fromBool(left.number >= right.number);
                }
                if (left.type == ValueType::String && right.type == ValueType::String) {
                    if (op == "<") return Value::fromBool(left.str() < right.str());
                    if (op == "<=") return Value::fromBool(left.str() <= right.str());
                    if (op == ">") return Value::fromBool(left.str() > right.str());
                    return Value::fromBool(left.str() >= right.str());
                }
                if ((left.type == ValueType::Array || left.type == ValueType::VmArray) &&
                    (right.type == ValueType::Array || right.type == ValueType::VmArray)) {
                    int c;
                    try { c = pylib::compareValues(left, right); } catch (const pylib::PyError& e) { throw RuntimeError(e.what()); }
                    return Value::fromBool(op == "<" ? c < 0 : op == "<=" ? c <= 0 : op == ">" ? c > 0 : c >= 0);
                }
                if (left.type == ValueType::Instance) {
                    Value r;
                    const char* dn = op == "<" ? "__lt__" : op == "<=" ? "__le__" : op == ">" ? "__gt__" : "__ge__";
                    if (instanceOpHook()(dn, left, right, r)) return Value::fromBool(r.truthy());
                }
                throw RuntimeError(i18n::tr("Operand '", "Operands of '") + op +
                        i18n::tr("' harus dua angka atau dua teks", "' must be two numbers or two strings"));
            }

            if (left.type != ValueType::Number || right.type != ValueType::Number) {
                Value rep;
                if (op == "*" && repeatValue(left, right, rep)) return rep;
                if (op == "-" && (left.type == ValueType::Array || left.type == ValueType::VmArray) &&
                    (right.type == ValueType::Array || right.type == ValueType::VmArray)) {
                    std::vector<Value> pa{left, right};
                    return pylib::call("_setdiff", pa, nullptr, nullptr);
                }
                if (op == "%" && left.type == ValueType::String) {
                    std::vector<Value> pa{left, right};
                    try {
                        return pylib::call("_percent", pa, nullptr, nullptr);
                    } catch (const pylib::PyError& e) {
                        throw RuntimeError(e.what());
                    }
                }
                if (left.type == ValueType::Instance) {
                    Value r;
                    const char* dn = op == "-" ? "__sub__" : op == "*" ? "__mul__" : op == "/" ? "__truediv__" : "__mod__";
                    if (instanceOpHook()(dn, left, right, r)) return r;
                }
                throw RuntimeError(i18n::tr("Operand '", "Operands of '") + op + i18n::tr("' harus angka", "' must be numbers"));
            }
            if (op == "-") return Value::fromNumber(left.number - right.number);
            if (op == "*") return Value::fromNumber(left.number * right.number);
            if (op == "/") {
                if (right.number == 0) throw RuntimeError("ZeroDivisionError: division by zero");
                return Value::fromNumber(left.number / right.number);
            }
            if (op == "%") {
                if (right.number == 0) throw RuntimeError("ZeroDivisionError: modulo by zero");
                return Value::fromNumber(pyModulo(left.number, right.number));
            }

            throw RuntimeError(i18n::tr("Operator binary nggak dikenal ", "Unknown binary operator ") + op);
        }
        case ExprKind::Call: {
            auto* node = static_cast<const CallExpr*>(expr);
            if (node->callee->kind == ExprKind::Index) {
                auto* idxNode = static_cast<const IndexExpr*>(node->callee.get());
                Value target = eval(idxNode->target.get(), env);
                ValueRootGuard targetGuard(target);
                if (target.type == ValueType::Instance) {
                    Value keyVal = eval(idxNode->index.get(), env);
                    ValueRootGuard keyGuard(keyVal);
                    if (keyVal.type != ValueType::String) {
                        throw RuntimeError(i18n::tr("Kunci objek harus teks", "Object key must be a string"));
                    }
                    std::vector<Value> args;
                    args.reserve(node->args.size());
                    ValueVectorRootGuard argsGuard(args);
                    for (const auto& a : node->args) args.push_back(eval(a.get(), env));
                    std::shared_ptr<ClassInfo> owner;
                    auto method = lookupMethod(target.instance()->classInfo, keyVal.str(), &owner);
                    if (method && target.instance()->classInfo->hasSpecial) {
                        uint8_t kind = methodKindOf(target.instance()->classInfo.get(), keyVal.str());
                        if (kind == 1) return callFunction(method, args, node->span, nullptr, owner);
                        if (kind == 2) {
                            args.insert(args.begin(), Value::fromClass(target.instance()->classInfo));
                            return callFunction(method, args, node->span, nullptr, owner);
                        }
                    }
                    if (method) return callFunction(method, args, node->span, &target, owner);
                    auto fit = target.instance()->fields->find(keyVal.str());
                    Value fieldVal = fit != target.instance()->fields->end() ? fit->second : Value::null();
                    return callValue(fieldVal, args, node->span);
                }
                Value keyVal = eval(idxNode->index.get(), env);
                ValueRootGuard keyGuard(keyVal);
                if (target.type == ValueType::Class && keyVal.type == ValueType::String) {
                    std::vector<Value> args;
                    ValueVectorRootGuard argsGuard(args);
                    for (const auto& a : node->args) args.push_back(eval(a.get(), env));
                    std::shared_ptr<ClassInfo> owner;
                    auto method = lookupMethod(target.klassShared(), keyVal.str(), &owner);
                    if (method) {
                        if (methodKindOf(target.klass(), keyVal.str()) == 2) args.insert(args.begin(), target);
                        return callFunction(method, args, node->span, nullptr, owner);
                    }
                    throw RuntimeError(i18n::tr("kelas tidak punya metode statis '", "class has no static method '") + keyVal.str() + "'");
                }
                if (keyVal.type == ValueType::String) {
                    bool receiverLast = false;
                    if (const char* builtin = builtinMethodName(target, keyVal.str(), &receiverLast)) {
                        std::vector<Value> args;
                        args.reserve(node->args.size() + 1);
                        ValueVectorRootGuard argsGuard(args);
                        if (!receiverLast) args.push_back(target);
                        for (const auto& a : node->args) args.push_back(eval(a.get(), env));
                        if (receiverLast) args.push_back(target);  // sep.join(list) == gabung(list, sep)
                        return callBuiltin(builtin, args);
                    }
                }
                Value callee = indexGet(target, keyVal);
                ValueRootGuard calleeGuard(callee);
                std::vector<Value> args;
                args.reserve(node->args.size());
                ValueVectorRootGuard argsGuard(args);
                for (const auto& a : node->args) args.push_back(eval(a.get(), env));
                return callValue(callee, args, node->span);
            }
            Value callee = eval(node->callee.get(), env);
            ValueRootGuard calleeGuard(callee);
            std::vector<Value> args;
            args.reserve(node->args.size());
            ValueVectorRootGuard argsGuard(args);
            for (const auto& a : node->args) args.push_back(eval(a.get(), env));
            return callValue(callee, args, node->span);
        }
        case ExprKind::ArrayLit: {
            auto* node = static_cast<const ArrayLitExpr*>(expr);
            auto arr = std::make_shared<std::vector<Value>>();
            arr->reserve(node->elements.size());
            ValueVectorRootGuard arrGuard(*arr);
            for (const auto& e : node->elements) arr->push_back(eval(e.get(), env));
            return Value::fromArray(std::move(arr));
        }
        case ExprKind::Index: {
            auto* node = static_cast<const IndexExpr*>(expr);
            Value target = eval(node->target.get(), env);
            ValueRootGuard targetGuard(target);
            Value idx = eval(node->index.get(), env);
            return indexGet(target, idx);
        }
        case ExprKind::IndexAssign: {
            auto* node = static_cast<const IndexAssignExpr*>(expr);
            Value target = eval(node->target.get(), env);
            ValueRootGuard targetGuard(target);
            Value idx = eval(node->index.get(), env);
            ValueRootGuard idxGuard(idx);
            Value value = eval(node->value.get(), env);
            indexSet(target, idx, value);
            GC::instance().noteStore(target, value);
            return value;
        }
        case ExprKind::FnExpr: {
            auto* node = static_cast<const FnExprNode*>(expr);
            auto fn = std::make_shared<Function>();
            fn->decl = node->decl.get();
            fn->closure = env;
            return Value::fromFunction(fn);
        }
    }
    throw RuntimeError(i18n::tr("Jenis ekspresi nggak ke-handle", "Unhandled expression kind"));
}

// ---- calls ----

Value Interpreter::callValue(const Value& callee, std::vector<Value>& args, Span callSite) {
    if (callee.type == ValueType::Fn) return callFunction(callee.fnShared(), args, callSite);
    if (callee.type == ValueType::VmFn) return vmCallValue(callee, args, this);
    if (callee.type == ValueType::Builtin) return callBuiltin(callee.builtinName(), args);
    if (callee.type == ValueType::Native) {
        // A native plugin call can block indefinitely -- reset exprDepth_
        // so it doesn't starve GC safepoints for the whole process.
        ValueVectorRootGuard argsRoot(args);
        DepthResetGuard depthReset(exprDepth_);
        try {
            return plugin::call(*callee.native(), args);
        } catch (const std::exception& e) {
            throw RuntimeError(e.what());
        }
    }
    if (callee.type == ValueType::Class) {
        auto state = std::make_shared<InstanceState>();
        state->classInfo = callee.klassShared();
        state->fields = std::make_shared<ValueMap>();
        GC::instance().trackInstance(state->fields);
        Value instanceVal = Value::fromInstance(state);
        if (callee.klass()->isStruct) {
            const auto& fields = callee.klass()->structFields;
            if (args.size() != fields.size()) {
                throw RuntimeError(i18n::tr("Bentuk '", "Struct '") + callee.klass()->name +
                                    i18n::tr("' butuh ", "' expects ") + std::to_string(fields.size()) +
                                    i18n::tr(" argumen, dapat ", " arg(s), got ") + std::to_string(args.size()));
            }
            for (size_t i = 0; i < fields.size(); i++) {
                (*state->fields)[fields[i]] = args[i];
            }
            return instanceVal;
        }
        std::shared_ptr<ClassInfo> owner;
        auto ctor = lookupMethod(callee.klassShared(), "konstruktor", &owner);
        if (!ctor) ctor = lookupMethod(callee.klassShared(), "constructor", &owner);
        if (ctor) callFunction(ctor, args, callSite, &instanceVal, owner);
        return instanceVal;
    }
    if (callee.type == ValueType::Instance) {
        Value r;
        if (callInstMethod(this, callee, "__call__", args, &r)) return r;
    }
    throw RuntimeError(i18n::tr("Coba manggil nilai yang bukan fungsi", "Attempted to call a non-function value"));
}

Value Interpreter::callFunction(const std::shared_ptr<Function>& fn, std::vector<Value>& args, Span callSite,
                                 const Value* boundThis, const std::shared_ptr<ClassInfo>& methodOwner) {
    const FnDeclStmt* decl = fn->decl;
    if (decl->variadic()) {
        std::string err = packVarargs(args, decl->restIndex, decl->kwIndex, decl->requiredArgs());
        if (!err.empty()) throw RuntimeError(i18n::tr("Fungsi '", "Function '") + decl->name + "': " + err);
    }
    if (static_cast<int>(args.size()) < decl->requiredArgs() || args.size() > decl->params.size()) {
        throw RuntimeError(i18n::tr("Fungsi '", "Function '") + decl->name +
                            i18n::tr("' butuh ", "' expects ") + std::to_string(decl->requiredArgs()) +
                            (decl->minArgs >= 0 ? ".." + std::to_string(decl->params.size()) : std::string()) +
                            i18n::tr(" argumen, dapat ", " arg(s), got ") + std::to_string(args.size()));
    }
    while (args.size() < decl->params.size()) args.push_back(Value::null());
    if (callDepth_ >= kMaxCallDepth) {
        throw RuntimeError(i18n::tr("Rekursi kelewat dalam (lebih dari ", "Recursion too deep (over ") +
                            std::to_string(kMaxCallDepth) + i18n::tr(" panggilan bersarang)", " nested calls)"));
    }
    DepthGuard callGuard(callDepth_);
    Environment* env = GC::instance().alloc(fn->closure);
    GcRootGuard guard(env);
    if (boundThis) {
        env->define("ini", *boundThis);
        if (methodOwner && methodOwner->parent && boundThis->type == ValueType::Instance) {
            auto superState = std::make_shared<InstanceState>();
            superState->classInfo = methodOwner->parent;
            superState->fields = boundThis->instance()->fields;
            env->define("induk", Value::fromInstance(superState));
        }
    }
    for (size_t i = 0; i < decl->params.size(); i++) {
        env->define(decl->params[i], args[i]);
    }
    try {
        DepthResetGuard depthReset(exprDepth_);
        execBlock(decl->body.get(), env);
        if (g_pending != kPendNone) {
            bool returned = g_pending == kPendReturn;
            g_pending = kPendNone;  // stray break/continue don't leak into the caller's loop
            if (returned) return std::move(g_retVal);
        }
    } catch (RuntimeError& e) {
        // Builds the call-chain trace one frame per unwind, innermost first.
        e.addFrame(decl->name, callSite);
        throw;
    }
    return Value::null();
}

void Interpreter::jalankanBadanGoroutine(Value fn, std::vector<Value> args, std::atomic<bool>* rootedFlag) {
#ifndef __EMSCRIPTEN__
    // Root the closure before touching the GIL (pushRoot has its own mutex).
    Environment* closure = fn.type == ValueType::Fn ? fn.fn()->closure : nullptr;
    GcRootGuard guard(closure);
    // Tell jalan() (spinning on the caller side) the closure is now safe.
    if (rootedFlag) rootedFlag->store(true, std::memory_order_release);

    GIL::instance().lock();
    Interpreter::registerCurrentThread();
    try {
        callValue(fn, args, Span{});
    } catch (const RuntimeError& e) {
        std::cerr << "[jalan] goroutine error: " << e.what() << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[jalan] goroutine error: " << e.what() << '\n';
    }
    Interpreter::unregisterCurrentThread();
    GIL::instance().unlock();
    GC::goroutineDone();
#else
    (void)fn; (void)args;
#endif
}

Value Interpreter::callBuiltin(const std::string& name, std::vector<Value>& args) {
    // Keyword arguments arrive as one trailing map tagged "__kw__" (see _callkw).
    const ValueMap* kw = nullptr;
    std::shared_ptr<ValueMap> kwHold;
    if (!args.empty() && args.back().type == ValueType::Map && args.back().map()->count("__kw__")) {
        kwHold = args.back().mapShared();
        kw = kwHold.get();
        args.pop_back();
    }
    if (pylib::handles(name)) {
        try {
            return pylib::call(name, args, kw, [this](const Value& fn, std::vector<Value>& a) { return callValue(fn, a, Span{}); });
        } catch (const pylib::PyError& e) {
            throw RuntimeError(e.what());
        }
    }
    if (pystd::handles(name)) {
        try {
            return pystd::call(name, args, kw, [this](const Value& fn, std::vector<Value>& a) { return callValue(fn, a, Span{}); });
        } catch (const pylib::PyError& e) {
            throw RuntimeError(e.what());
        }
    }
    if (name == "open") {
        // open(path, mode="r"): a File object from the embedded __io module.
        std::string mode = "r";
        if (args.size() > 1 && args[1].type == ValueType::String) mode = args[1].str();
        if (kw) { auto it = kw->find("mode"); if (it != kw->end() && it->second.type == ValueType::String) mode = it->second.str(); }
        if (args.empty()) throw RuntimeError("open() butuh path");
        Value mod = doImport("__io");
        Value cls = (*mod.map())["File"];
        std::vector<Value> ctorArgs{args[0], Value::fromString(mode)};
        if (vmIsActive()) return vmCallValue(cls, ctorArgs, this);
        return callValue(cls, ctorArgs, Span{});
    }
    if (name == "_with_enter") {
        Value r;
        std::vector<Value> none;
        if (!args.empty() && args[0].type == ValueType::Instance && callInstMethod(this, args[0], "__enter__", none, &r)) return r;
        return args.empty() ? Value::null() : args[0];
    }
    if (name == "_with_exit") {  // (manager, error-or-None) -> true when the error is swallowed
        if (args.size() != 2) throw RuntimeError("_with_exit() butuh 2 argumen");
        const Value& m = args[0];
        if (m.type != ValueType::Instance) return Value::fromBool(false);
        std::vector<Value> ea;
        if (args[1].type == ValueType::Null) {
            ea = {Value::null(), Value::null(), Value::null()};
        } else {
            std::vector<Value> wa{args[1]};
            Value err = callBuiltin("_exc_wrap", wa);  // __exit__ gets an exception object, as in Python
            Value t = err.type == ValueType::Instance ? Value::fromClass(err.instance()->classInfo) : Value::fromString(err.typeName());
            ea = {t, err, Value::null()};
        }
        Value r;
        if (callInstMethod(this, m, "__exit__", ea, &r)) return Value::fromBool(args[1].type != ValueType::Null && r.truthy());
        std::vector<Value> none;
        callInstMethod(this, m, "close", none, &r);
        return Value::fromBool(false);
    }
    if (name == "_close") {
        if (!args.empty() && args[0].type == ValueType::Instance) {
            std::vector<std::string> names;
            std::shared_ptr<ClassInfo> owner;
            bool has = (vmIsActive() && vmMethodParamNames(args[0].instance()->classInfo.get(), "close", names)) ||
                       lookupMethod(args[0].instance()->classInfo, "close", &owner) != nullptr;
            if (has) {
                Value obj = args[0];
                std::vector<Value> none;
                if (vmIsActive()) return vmCallMethod(obj, "close", none, this);
                return callFunction(lookupMethod(obj.instance()->classInfo, "close", &owner), none, Span{}, &obj, owner);
            }
        }
        return Value::null();
    }
    if (kw && name != "cetak" && name != "_callkw" && name != "_callkwm") {
        for (const auto& e : *kw) {
            if (e.first != "__kw__") throw RuntimeError(name + "(): argumen bernama '" + e.first + "' tidak didukung");
        }
    }
    auto need = [&](size_t n) {
        if (args.size() != n) {
            throw RuntimeError(name + i18n::tr("() butuh ", "() expects ") + std::to_string(n) +
                                i18n::tr(" argumen, dapat ", " arg(s), got ") + std::to_string(args.size()));
        }
    };
    auto expectType = [&](const Value& v, ValueType t) {
        if (v.type != t) {
            if (t == ValueType::Array && v.type == ValueType::VmArray) return;
            if (t == ValueType::Fn && (v.type == ValueType::VmFn || v.type == ValueType::Builtin)) return;
            throw RuntimeError(name + "(): tipe argumen salah");
        }
    };
    // expectType(Array) also accepts VmArray, but .array() returns null for
    // one -- use this after an Array-typed expectType() check instead.
    auto arrayElements = [&](const Value& v) -> std::vector<Value> {
        if (v.type == ValueType::VmArray) {
            auto* st = v.vmArray();
            std::vector<Value> out;
            if (st->numeric) {
                out.reserve(st->nums.size());
                for (double n : st->nums) out.push_back(Value::fromNumber(n));
            } else {
                out = *st->boxed;
            }
            return out;
        }
        auto* arr = v.array();
        if (arr) return *arr;
        return {};
    };

    if (name == "cetak") {
        std::ostream& os = outStream_ ? *outStream_ : std::cout;
        std::string sep = " ", end = "\n";
        if (kw) {
            if (auto it = kw->find("sep"); it != kw->end() && it->second.type != ValueType::Null) sep = it->second.stringify();
            if (auto it = kw->find("end"); it != kw->end() && it->second.type != ValueType::Null) end = it->second.stringify();
        }
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) os << sep;
            os << args[i].stringify();
        }
        os << end;
        os.flush();
        return Value::null();
    }

    // ke_teks/peta_baru/json_encode moved up front -- hottest builtins by
    // profile, dispatch is a linear name-compare chain.
    if (name == "ke_teks") {
        need(1);
        return Value::fromString(args[0].stringify());
    }

    if (name == "peta_baru") {
        need(0);
        return Value::newMap();
    }

    if (name == "json_encode") {
        need(1);
        try {
            return Value::fromString(json::encode(args[0]));
        } catch (const std::exception& e) {
            throw RuntimeError(e.what());
        }
    }

    if (name == "panjang") {
        need(1);
        const Value& v = args[0];
        if (v.type == ValueType::String) return Value::fromNumber(static_cast<double>(v.str().size()));
        if (v.type == ValueType::Array) return Value::fromNumber(static_cast<double>(v.array()->size()));
        if (v.type == ValueType::VmArray) return Value::fromNumber(static_cast<double>(v.vmArray()->numeric ? v.vmArray()->nums.size() : v.vmArray()->boxed->size()));
        if (v.type == ValueType::Map) return Value::fromNumber(static_cast<double>(v.map()->size()));
        if (v.type == ValueType::Instance) {
            if (vmIsActive()) { std::vector<Value> none; Value o = v; return vmCallMethod(o, "__len__", none, this); }
            std::shared_ptr<ClassInfo> owner;
            if (auto m = lookupMethod(v.instance()->classInfo, "__len__", &owner)) {
                std::vector<Value> none;
                Value self = v;
                return callFunction(m, none, Span{}, &self, owner);
            }
        }
        throw RuntimeError(i18n::tr("panjang(): butuh teks, larik, atau peta", "panjang(): needs a string, array, or map"));
    }

    if (name == "tambah") {
        need(2);
        if (args[0].type == ValueType::VmArray) {
            auto* st = args[0].vmArray();
            if (st->numeric) {
                if (args[1].type == ValueType::Number) {
                    st->nums.push_back(args[1].number);
                    return Value::fromNumber(static_cast<double>(st->nums.size()));
                } else {
                    st->numeric = false;
                    st->boxed = std::make_shared<std::vector<Value>>();
                    st->boxed->reserve(st->nums.size() + 1);
                    for (double d : st->nums) st->boxed->push_back(Value::fromNumber(d));
                    st->boxed->push_back(args[1]);
                    GC::instance().noteStore(args[0], args[1]);
                    st->nums.clear();
                    st->nums.shrink_to_fit();
                    return Value::fromNumber(static_cast<double>(st->boxed->size()));
                }
            } else {
                st->boxed->push_back(args[1]);
                GC::instance().noteStore(args[0], args[1]);
                return Value::fromNumber(static_cast<double>(st->boxed->size()));
            }
        }
        expectType(args[0], ValueType::Array);
        args[0].array()->push_back(args[1]);
        GC::instance().noteStore(args[0], args[1]);
        return Value::fromNumber(static_cast<double>(args[0].array()->size()));
    }

    if (name == "hapus_akhir") {
        need(1);
        if (args[0].type == ValueType::VmArray) {
            auto* st = args[0].vmArray();
            if (st->numeric) {
                if (st->nums.empty()) throw RuntimeError(i18n::tr("hapus_akhir(): larik kosong", "hapus_akhir(): empty array"));
                double last = st->nums.back();
                st->nums.pop_back();
                return Value::fromNumber(last);
            } else {
                if (st->boxed->empty()) throw RuntimeError(i18n::tr("hapus_akhir(): larik kosong", "hapus_akhir(): empty array"));
                Value last = st->boxed->back();
                st->boxed->pop_back();
                return last;
            }
        }
        expectType(args[0], ValueType::Array);
        auto& vec = *args[0].array();
        if (vec.empty()) throw RuntimeError(i18n::tr("hapus_akhir(): larik kosong", "hapus_akhir(): empty array"));
        Value last = vec.back();
        vec.pop_back();
        return last;
    }

    if (name == "potong") {
        need(3);
        if (args[1].type != ValueType::Number || args[2].type != ValueType::Number) {
            throw RuntimeError(i18n::tr("potong(): batas awal/akhir harus angka", "potong(): start/end bounds must be numbers"));
        }
        long long start = static_cast<long long>(args[1].number);
        long long end = static_cast<long long>(args[2].number);
        if (args[0].type == ValueType::String) {
            long long len = static_cast<long long>(args[0].str().size());
            start = std::max<long long>(0, std::min(start, len));
            end = std::max<long long>(start, std::min(end, len));
            return Value::fromString(args[0].str().substr(static_cast<size_t>(start),
                                                          static_cast<size_t>(end - start)));
        }
        if (args[0].type == ValueType::Array) {
            long long len = static_cast<long long>(args[0].array()->size());
            start = std::max<long long>(0, std::min(start, len));
            end = std::max<long long>(start, std::min(end, len));
            auto out = std::make_shared<std::vector<Value>>(
                args[0].array()->begin() + start, args[0].array()->begin() + end);
            return Value::fromArray(std::move(out));
        }
        if (args[0].type == ValueType::VmArray) {
            auto* st = args[0].vmArray();
            long long len = static_cast<long long>(st->numeric ? st->nums.size() : st->boxed->size());
            start = std::max<long long>(0, std::min(start, len));
            end = std::max<long long>(start, std::min(end, len));
            auto out = std::make_shared<VmArrayState>();
            if (st->numeric) {
                out->numeric = true;
                out->nums.assign(st->nums.begin() + start, st->nums.begin() + end);
            } else {
                out->numeric = false;
                out->boxed = std::make_shared<std::vector<Value>>(
                    st->boxed->begin() + start, st->boxed->begin() + end);
            }
            return Value::fromVmArray(out);
        }
        throw RuntimeError(i18n::tr("potong(): argumen pertama harus teks atau larik", "potong(): first argument must be a string or array"));
    }

    if (name == "gabung") {
        need(2);
        expectType(args[1], ValueType::String);
        std::string out;
        const std::string& sep = args[1].str();
        if (args[0].type == ValueType::VmArray) {
            auto* st = args[0].vmArray();
            if (st->numeric) {
                for (size_t i = 0; i < st->nums.size(); i++) {
                    if (i > 0) out += sep;
                    out += Value::fromNumber(st->nums[i]).stringify();
                }
            } else {
                for (size_t i = 0; i < st->boxed->size(); i++) {
                    if (i > 0) out += sep;
                    out += (*st->boxed)[i].stringify();
                }
            }
            return Value::fromString(out);
        }
        expectType(args[0], ValueType::Array);
        auto& arr = *args[0].array();
        for (size_t i = 0; i < arr.size(); i++) {
            if (i > 0) out += sep;
            out += arr[i].stringify();
        }
        return Value::fromString(out);
    }

    if (name == "pisah") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::String);
        auto out = std::make_shared<std::vector<Value>>();
        const std::string& s = args[0].str();
        const std::string& sep = args[1].str();
        if (sep.empty()) {
            for (char c : s) out->push_back(Value::fromString(std::string(1, c)));
        } else {
            size_t pos = 0;
            while (true) {
                size_t next = s.find(sep, pos);
                if (next == std::string::npos) {
                    out->push_back(Value::fromString(s.substr(pos)));
                    break;
                }
                out->push_back(Value::fromString(s.substr(pos, next - pos)));
                pos = next + sep.size();
            }
        }
        return Value::fromArray(std::move(out));
    }

    if (name == "huruf_besar" || name == "huruf_kecil") {
        need(1);
        expectType(args[0], ValueType::String);
        std::string out = args[0].str();
        bool upper = name == "huruf_besar";
        for (char& c : out) {
            c = static_cast<char>(upper ? std::toupper(static_cast<unsigned char>(c))
                                         : std::tolower(static_cast<unsigned char>(c)));
        }
        return Value::fromString(out);
    }

    if (name == "ke_angka") {
        need(1);
        if (args[0].type == ValueType::Number) return args[0];
        if (args[0].type == ValueType::Bool) return Value::fromNumber(args[0].boolean() ? 1 : 0);
        if (args[0].type == ValueType::String) {
            try {
                size_t consumed = 0;
                double d = std::stod(args[0].str(), &consumed);
                return Value::fromNumber(d);
            } catch (...) {
                throw RuntimeError(i18n::tr("ke_angka(): '", "ke_angka(): '") + args[0].str() +
                                    i18n::tr("' bukan angka valid", "' is not a valid number"));
            }
        }
        throw RuntimeError(i18n::tr("ke_angka(): nggak bisa dikonversi ke angka", "ke_angka(): can't be converted to a number"));
    }

    if (name == "tipe") {
        need(1);
        if (args[0].type == ValueType::Instance) return Value::fromClass(args[0].instance()->classInfo);  // type(obj).__name__
        return Value::fromString(args[0].typeName());
    }

    if (name == "waktu") {
        need(0);
        auto now = std::chrono::system_clock::now();
        double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
        return Value::fromNumber(secs);
    }

    if (name == "_go") {
        // _go(cfg, target, name, args...): calls a wrapped Go function. Scalar arguments and results
        // cross the plugin ABI directly (no JSON); anything else goes through cfg["panggil"].
        if (args.size() < 3 || args[0].type != ValueType::Map) throw RuntimeError("_go(): argumen tidak valid");
        ValueMap& cfg = *args[0].map();
        auto slow = [&]() -> Value {
            std::vector<Value> rest(args.begin() + 3, args.end());
            std::vector<Value> pa{args[1], args[2], Value::fromArray(std::make_shared<std::vector<Value>>(std::move(rest)))};
            return callValue(cfg.at("panggil"), pa, Span{});
        };
        bool scalar = true;
        for (size_t i = 1; i < args.size(); i++) {
            ValueType t = args[i].type;
            if (t != ValueType::Number && t != ValueType::String && t != ValueType::Bool && t != ValueType::Null) { scalar = false; break; }
        }
        auto pit = cfg.find("cepat");
        if (!scalar || pit == cfg.end() || !pit->second.native()) return slow();
        NsValue stackBuf[10];
        std::vector<NsValue> heapBuf;
        NsValue* argv = stackBuf;
        size_t n = args.size() - 1;
        if (n > 10) { heapBuf.resize(n); argv = heapBuf.data(); }
        for (size_t i = 0; i < n; i++) {
            const Value& v = args[i + 1];
            NsValue& a = argv[i];
            a = NsValue{};
            switch (v.type) {
                case ValueType::String:
                    a.type = NS_STRING;
                    a.str = const_cast<char*>(v.str().data());  // borrowed for the call
                    a.str_len = static_cast<int>(v.str().size());
                    break;
                case ValueType::Number: a.type = NS_NUMBER; a.number = v.number; break;
                case ValueType::Bool: a.type = NS_BOOL; a.boolean = v.boolean() ? 1 : 0; break;
                default: a.type = NS_NULL; break;
            }
        }
        NsValue result{};
        {
            ValueVectorRootGuard argsRoot(args);
            DepthResetGuard depthReset(exprDepth_);
            GilRelease release;
            result = reinterpret_cast<NsFn>(pit->second.native()->fnPtr)(static_cast<int>(n), argv);
        }
        Value out;
        switch (result.type) {
            case NS_NUMBER: return Value::fromNumber(result.number);
            case NS_BOOL: return Value::fromBool(result.boolean != 0);
            case NS_NULL: return Value::null();
            default: break;
        }
        std::string payload = result.str ? std::string(result.str, result.str_len >= 0 ? static_cast<size_t>(result.str_len) : std::strlen(result.str)) : std::string();
        if (result.str) free(result.str);
        if (payload.size() >= 2 && payload[0] == '\x01') {
            char kind = payload[1];
            if (kind == 'S') return slow();
            if (kind == 'E') throw ThrownValue(Value::fromString(payload.substr(2)));
            Value data = json::decode(payload.substr(2));
            if (kind == 'H') {
                std::vector<Value> ba{data};
                return callValue(cfg.at("bungkus"), ba, Span{});
            }
            return data;
        }
        return Value::fromString(std::move(payload));
    }

    if (name == "pegang") {
        // pegang(fungsi_bebas, kunci): a value that calls fungsi_bebas(kunci) when it is
        // collected. Keep it inside the object that owns the resource.
        need(2);
        if (args[0].type != ValueType::Native || !args[0].native()) {
            throw RuntimeError("pegang(): argumen pertama harus fungsi native");
        }
        expectType(args[1], ValueType::String);
        const NativeFunction* src = args[0].native();
        auto nf = std::make_shared<NativeFunction>();
        nf->plugin = src->plugin;
        nf->fnPtr = src->fnPtr;
        nf->name = src->name;
        nf->abiVer = src->abiVer;
        std::shared_ptr<NativePlugin> keep = src->plugin;
        void* fp = src->fnPtr;
        std::string key = args[1].str();
        nf->onRelease = [keep, fp, key]() {
            NsValue a{};
            a.type = NS_STRING;
            a.str = const_cast<char*>(key.c_str());
            a.str_len = static_cast<int>(key.size());
            NsValue r = reinterpret_cast<NsFn>(fp)(1, &a);
            if (r.type == NS_STRING && r.str) free(r.str);
        };
        return Value::fromNative(nf);
    }

    if (name == "_in") {
        // `a in b`: substring, array element, or map key.
        need(2);
        const Value& needle = args[0];
        const Value& hay = args[1];
        if (hay.type == ValueType::String) {
            if (needle.type != ValueType::String) throw RuntimeError(i18n::tr("'in' pada teks butuh teks di kiri", "'in' on a string needs a string on the left"));
            return Value::fromBool(hay.str().find(needle.str()) != std::string::npos);
        }
        if (hay.type == ValueType::Map) {
            std::string key = needle.type == ValueType::String ? needle.str() : needle.stringify();
            return Value::fromBool(hay.map()->find(key) != hay.map()->end());
        }
        if (hay.type == ValueType::Array) {
            for (const Value& el : *hay.array()) if (valuesEqual(el, needle)) return Value::fromBool(true);
            return Value::fromBool(false);
        }
        if (hay.type == ValueType::VmArray) {
            auto* st = hay.vmArray();
            if (st->numeric) {
                if (needle.type != ValueType::Number) return Value::fromBool(false);
                for (double d : st->nums) if (d == needle.number) return Value::fromBool(true);
                return Value::fromBool(false);
            }
            for (const Value& el : *st->boxed) if (valuesEqual(el, needle)) return Value::fromBool(true);
            return Value::fromBool(false);
        }
        if (hay.type == ValueType::Instance) {
            std::vector<Value> a{needle};
            Value r;
            if (callInstMethod(this, hay, "__contains__", a, &r)) return Value::fromBool(r.truthy());
            std::vector<Value> lst{hay};
            Value arr = pylib::call("list", lst, nullptr, [this](const Value& fn, std::vector<Value>& aa) { return callValue(fn, aa, Span{}); });
            for (const Value& el : arrayElements(arr)) if (valuesEqual(el, needle)) return Value::fromBool(true);
            return Value::fromBool(false);
        }
        throw RuntimeError(i18n::tr("'in' butuh teks, larik, atau peta di kanan", "'in' needs a string, array, or map on the right"));
    }

    if (name == "_callkw" || name == "_callkwm") {
        // Keyword arguments. _callkw(fn, [pos...], {k: v}) / _callkwm(obj, "name", [pos...], {k: v})
        const bool method = name == "_callkwm";
        need(method ? 4 : 3);
        Value obj = method ? args[0] : Value::null();
        Value fn = method ? Value::null() : args[0];
        std::string mname = method ? args[1].str() : std::string();
        std::vector<Value> pos = arrayElements(args[method ? 2 : 1]);
        ValueMap kwMap = *args[method ? 3 : 2].map();

        auto tagged = [&](std::vector<Value> p) {  // positional + "__kw__"-tagged map, for builtins
            auto m = std::make_shared<ValueMap>();
            (*m)["__kw__"] = Value::fromBool(true);
            for (const auto& e : kwMap) (*m)[e.first] = e.second;
            p.push_back(Value::fromMap(m));
            return p;
        };
        int bindRest = -1, bindKw = -1;  // *args / **kw positions of the target, when it has them
        auto bind = [&](const std::vector<std::string>& names, std::vector<Value> p, const std::string& who) {
            size_t maxIdx = p.size();
            std::vector<Value> out = p;
            auto extraKw = std::make_shared<ValueMap>();
            size_t ordinary = names.size();
            if (bindRest >= 0) ordinary = std::min<size_t>(ordinary, static_cast<size_t>(bindRest));
            if (bindKw >= 0) ordinary = std::min<size_t>(ordinary, static_cast<size_t>(bindKw));
            for (const auto& e : kwMap) {
                size_t idx = names.size();
                for (size_t k = 0; k < ordinary; k++) if (names[k] == e.first) { idx = k; break; }
                if (idx == names.size()) {
                    if (bindKw >= 0) { (*extraKw)[e.first] = e.second; continue; }
                    throw RuntimeError(who + "(): argumen bernama '" + e.first + "' tidak dikenal");
                }
                if (idx < p.size()) throw RuntimeError(who + "(): argumen '" + e.first + "' diberikan dua kali");
                if (out.size() <= idx) out.resize(idx + 1, Value::null());
                out[idx] = e.second;
                maxIdx = std::max(maxIdx, idx + 1);
            }
            out.resize(maxIdx, Value::null());
            if (!extraKw->empty()) {
                (*extraKw)["__kw__"] = Value::fromBool(true);
                out.push_back(Value::fromMap(extraKw));  // packed into **kw by the callee
            }
            return out;
        };
        auto astParams = [](const std::shared_ptr<Function>& f) { return f->decl->params; };

        if (method) {
            if (obj.type == ValueType::Instance) {
                std::vector<std::string> names;
                bool astShadow = false;
                (void)astShadow;
                bool haveVm = vmIsActive() && vmMethodParamNames(obj.instance()->classInfo.get(), mname, names, &bindRest, &bindKw);
                std::shared_ptr<ClassInfo> owner;
                auto astMethod = haveVm ? nullptr : lookupMethod(obj.instance()->classInfo, mname, &owner);
                if (!haveVm && astMethod) names = astParams(astMethod);
                if (haveVm || astMethod) {
                    if (astMethod) { bindRest = astMethod->decl->restIndex; bindKw = astMethod->decl->kwIndex; }
                    std::vector<Value> bound = bind(names, pos, mname);
                    if (haveVm) return vmCallMethod(obj, mname, bound, this);
                    return callFunction(astMethod, bound, Span{}, &obj, owner);
                }
                auto fit = obj.instance()->fields->find(mname);
                if (fit == obj.instance()->fields->end()) throw RuntimeError("objek tidak punya metode '" + mname + "'");
                fn = fit->second;
            } else {
                bool receiverLast = false;
                if (const char* b = builtinMethodName(obj, mname, &receiverLast)) {
                    std::vector<Value> full;
                    full.push_back(obj);
                    for (auto& p : pos) full.push_back(p);
                    std::vector<Value> t = tagged(full);
                    return callBuiltin(b, t);
                }
                fn = indexGet(obj, Value::fromString(mname));
            }
        }
        if (fn.type == ValueType::Builtin) {
            std::vector<Value> t = tagged(pos);
            return callBuiltin(fn.builtinName(), t);
        }
        std::vector<std::string> names;
        if (fn.type == ValueType::Fn) { names = astParams(fn.fnShared()); bindRest = fn.fn()->decl->restIndex; bindKw = fn.fn()->decl->kwIndex; }
        else if (fn.type == ValueType::VmFn) { vmParamNames(fn, names); vmVarargInfo(fn, bindRest, bindKw); }
        else if (fn.type == ValueType::Class) {
            std::shared_ptr<ClassInfo> owner;
            bool found = false;
            for (const char* ctorName : {"konstruktor", "constructor"}) {
                if (vmIsActive() && vmMethodParamNames(fn.klass(), ctorName, names, &bindRest, &bindKw)) { found = true; break; }
                auto ctor = lookupMethod(fn.klassShared(), ctorName, &owner);
                if (ctor) { names = astParams(ctor); found = true; break; }
            }
            if (!found && fn.klass()->isStruct) names = fn.klass()->structFields;
            else if (!found) throw RuntimeError("kelas tidak punya konstruktor yang menerima argumen bernama");
        } else {
            throw RuntimeError("argumen bernama tidak didukung untuk tipe ini");
        }
        std::vector<Value> bound = bind(names, pos, "fungsi");
        if (fn.type == ValueType::Class && vmIsActive()) return vmCallValue(fn, bound, this);
        return callValue(fn, bound, Span{});
    }

    if (name == "_peta") {
        // Dict literal {k: v, ...}: alternating key/value arguments.
        Value m = Value::newMap();
        for (size_t i = 0; i + 1 < args.size(); i += 2) {
            (*m.map())[args[i].type == ValueType::String ? args[i].str() : args[i].stringify()] = args[i + 1];
            GC::instance().noteStore(m, args[i + 1]);
        }
        return m;
    }

    if (name == "getattr" || name == "hasattr") {
        if (args.size() < 2 || args.size() > 3) throw RuntimeError(name + "() butuh 2 atau 3 argumen");
        expectType(args[1], ValueType::String);
        const Value& o = args[0];
        bool found = false;
        Value got;
        if (o.type == ValueType::Instance) {
            const std::string& k = args[1].str();
            std::vector<std::string> pn;
            found = o.instance()->fields->count(k) != 0 || vmMethodParamNames(o.instance()->classInfo.get(), k, pn) || k == "__class__" ||
                    lookupMethod(o.instance()->classInfo, k) != nullptr || classAttrOf(o.instance()->classInfo.get(), k) != nullptr ||
                    methodKindOf(o.instance()->classInfo.get(), k) != 0;
            if (found) got = vmIsActive() ? vmIndexGet(o, args[1]) : indexGet(o, args[1]);
            if (found && name == "getattr" && (got.type == ValueType::Fn || got.type == ValueType::VmFn) &&
                !o.instance()->fields->count(k) && methodKindOf(o.instance()->classInfo.get(), k) == 0) {
                Value mod = doImport("__gen");  // a method fetched by name stays bound to its object
                std::vector<Value> ba{o, args[1]};
                got = vmIsActive() ? vmCallValue((*mod.map())["bound"], ba, this) : callValue((*mod.map())["bound"], ba, Span{});
            }
        } else if (o.type == ValueType::Map) {
            auto it = o.map()->find(args[1].str());
            found = it != o.map()->end();
            if (found) got = it->second;
        } else if (o.type == ValueType::Class) {
            const std::string& k = args[1].str();
            found = classAttrOf(o.klass(), k) != nullptr || lookupMethod(o.klassShared(), k) != nullptr || k == "__name__";
            if (found) got = vmIsActive() ? vmIndexGet(o, args[1]) : indexGet(o, args[1]);
        }
        if (name == "hasattr") return Value::fromBool(found);
        if (found) return got;
        if (args.size() == 3) return args[2];
        throw RuntimeError("AttributeError: '" + std::string(o.typeName()) + "' object has no attribute '" + args[1].str() + "'");
    }
    if (name == "_callmeth") {  // (object, name, [args]) -> object.name(*args)
        need(3);
        std::vector<Value> a = arrayElements(args[2]);
        Value r;
        if (!callInstMethod(this, args[0], args[1].str().c_str(), a, &r)) throw RuntimeError("AttributeError: objek tidak punya metode '" + args[1].str() + "'");
        return r;
    }
    if (name == "setattr") {
        need(3);
        expectType(args[1], ValueType::String);
        Value o = args[0];
        if (o.type == ValueType::Instance || o.type == ValueType::Map || o.type == ValueType::Class) {
            indexSet(o, args[1], args[2]);
            return Value::null();
        }
        throw RuntimeError("setattr(): objek tidak bisa diberi atribut");
    }
    if (name == "delattr") {
        need(2);
        expectType(args[1], ValueType::String);
        if (args[0].type == ValueType::Instance) args[0].instance()->fields->erase(args[1].str());
        else if (args[0].type == ValueType::Map) args[0].map()->erase(args[1].str());
        return Value::null();
    }
    if (name == "vars" || name == "dir") {
        need(1);
        auto out = std::make_shared<std::vector<Value>>();
        auto m = std::make_shared<ValueMap>();
        if (args[0].type == ValueType::Instance) {
            for (const auto& [k, v] : *args[0].instance()->fields) { (*m)[k] = v; out->push_back(Value::fromString(k)); }
            if (name == "dir") {
                std::vector<std::string> seen;
                for (ClassInfo* c = args[0].instance()->classInfo.get(); c; c = c->parent.get()) {
                    for (const auto& [k, f] : c->methods) out->push_back(Value::fromString(k));
                    for (const auto& [k, f] : c->vmMethods) out->push_back(Value::fromString(k));
                }
            }
        } else if (args[0].type == ValueType::Map) {
            for (const auto& [k, v] : *args[0].map()) { (*m)[k] = v; out->push_back(Value::fromString(k)); }
        }
        if (name == "vars") return Value::fromMap(m);
        return Value::fromArray(out);
    }
    if (name == "id") {
        need(1);
        return Value::fromNumber(static_cast<double>(reinterpret_cast<uintptr_t>(args[0].ref.get())));
    }
    if (name == "hash") {
        need(1);
        const Value& v = args[0];
        if (v.type == ValueType::Number) return Value::fromNumber(v.number == std::floor(v.number) ? v.number : static_cast<double>(std::hash<double>{}(v.number) % 1000000007ULL));
        return Value::fromNumber(static_cast<double>(std::hash<std::string>{}(v.stringify()) % 2305843009213693951ULL));
    }
    if (name == "issubclass") {
        need(2);
        if (args[0].type != ValueType::Class) return Value::fromBool(false);
        std::vector<Value> targets = (args[1].type == ValueType::Array || args[1].type == ValueType::VmArray) ? arrayElements(args[1]) : std::vector<Value>{args[1]};
        for (const Value& t : targets) {
            if (t.type != ValueType::Class) continue;
            for (ClassInfo* c = args[0].klass(); c; c = c->parent.get()) if (c == t.klass() || c->name == t.klass()->name) return Value::fromBool(true);
        }
        return Value::fromBool(false);
    }
    if (name == "_exc_match" || name == "_exc_wrap") {
        need(name == "_exc_match" ? 2 : 1);
        const Value& e = args[0];
        // Parent of each builtin exception class (the tree the __exc module defines).
        static const std::unordered_map<std::string, std::string> parents = {
            {"Exception", "BaseException"}, {"ArithmeticError", "Exception"}, {"ZeroDivisionError", "ArithmeticError"},
            {"OverflowError", "ArithmeticError"}, {"LookupError", "Exception"}, {"IndexError", "LookupError"},
            {"KeyError", "LookupError"}, {"ValueError", "Exception"}, {"UnicodeError", "ValueError"},
            {"TypeError", "Exception"}, {"NameError", "Exception"}, {"AttributeError", "Exception"},
            {"RuntimeError", "Exception"}, {"NotImplementedError", "RuntimeError"}, {"RecursionError", "RuntimeError"},
            {"OSError", "Exception"}, {"IOError", "OSError"}, {"FileNotFoundError", "OSError"},
            {"PermissionError", "OSError"}, {"TimeoutError", "OSError"}, {"ConnectionError", "OSError"},
            {"StopIteration", "Exception"}, {"StopAsyncIteration", "Exception"}, {"AssertionError", "Exception"},
            {"ImportError", "Exception"}, {"ModuleNotFoundError", "ImportError"}, {"EOFError", "Exception"},
            {"Warning", "Exception"}, {"UserWarning", "Warning"}, {"DeprecationWarning", "Warning"},
            {"FileExistsError", "OSError"}, {"IsADirectoryError", "OSError"}, {"NotADirectoryError", "OSError"},
            {"BrokenPipeError", "OSError"}, {"UnicodeDecodeError", "UnicodeError"}, {"UnicodeEncodeError", "UnicodeError"},
            {"KeyboardInterrupt", "BaseException"}, {"SystemExit", "BaseException"}, {"GeneratorExit", "BaseException"}};
        // What kind of error a thrown non-object is: a plain string counts as Exception; the interpreter's
        // own errors ({pesan: "..."}) are told apart by their message.
        auto classify = [](const Value& v) -> std::string {
            if (v.type != ValueType::Map) return "Exception";
            auto it = v.map()->find("pesan");
            if (it == v.map()->end() || it->second.type != ValueType::String) return "Exception";
            const std::string& m = it->second.str();
            auto has = [&](const char* sub) { return m.find(sub) != std::string::npos; };
            if (has("ZeroDivisionError") || has("bagi dengan nol") || has("division by zero") || has("modulo by zero")) return "ZeroDivisionError";
            if (has("StopIteration")) return "StopIteration";
            if (has("di luar batas") || has("out of bounds") || has("out of range") || has("IndexError")) return "IndexError";
            if (has("KeyError") || has("kunci") ) return "KeyError";
            if (has("Undefined variable") || has("belum didefinisikan") || has("NameError")) return "NameError";
            if (has("FileNotFoundError") || has("nggak bisa buka") || has("tidak bisa membuka") || has("file tidak ada")) return "FileNotFoundError";
            if (has("rekursi") || has("recursion")) return "RecursionError";
            if (has("AssertionError")) return "AssertionError";
            if (has("nggak bisa dikonversi") || has("invalid literal") || has("ValueError")) return "ValueError";
            if (has("harus ") || has("bukan fungsi") || has("nggak bisa di-") || has("butuh ") || has("TypeError")) return "TypeError";
            return "RuntimeError";
        };
        if (name == "_exc_match") {
            std::vector<Value> classes;
            if (args[1].type == ValueType::Array || args[1].type == ValueType::VmArray) classes = arrayElements(args[1]);
            else classes.push_back(args[1]);
            for (const Value& c : classes) {
                if (c.type != ValueType::Class) continue;
                const std::string& want = c.klass()->name;
                if (e.type == ValueType::Instance) {
                    for (ClassInfo* k = e.instance()->classInfo.get(); k; k = k->parent.get()) {
                        if (k == c.klass() || k->name == want) return Value::fromBool(true);
                    }
                } else {
                    for (std::string k = classify(e); !k.empty();) {
                        if (k == want) return Value::fromBool(true);
                        auto p = parents.find(k);
                        k = p == parents.end() ? "" : p->second;
                    }
                }
            }
            return Value::fromBool(false);
        }
        // _exc_wrap: hand `except X as e` an exception object
        if (e.type == ValueType::Instance) return e;
        bool ours = e.type == ValueType::String ||
                    (e.type == ValueType::Map && e.map()->count("pesan") && e.map()->size() == 1);
        if (!ours) return e;
        Value mod = doImport("__exc");
        Value cls = (*mod.map())[classify(e)];
        std::string msg = e.type == ValueType::String ? e.str() : (*e.map())["pesan"].stringify();
        {  // "ZeroDivisionError: division by zero (line 3, col 5)" -> "division by zero"
            std::string prefix = classify(e) + ": ";
            if (msg.compare(0, prefix.size(), prefix) == 0) msg = msg.substr(prefix.size());
            size_t at = msg.rfind(" (line ");
            if (at != std::string::npos && msg.back() == ')') msg = msg.substr(0, at);
        }
        std::vector<Value> ctorArgs{Value::fromString(msg)};
        if (vmIsActive()) return vmCallValue(cls, ctorArgs, this);
        return callValue(cls, ctorArgs, Span{});
    }
    if (name == "_defaultdict") {  // (factory) -> dict that fills in missing keys by calling factory()
        Value m = Value::newMap();
        if (!args.empty() && args[0].type != ValueType::Null) m.map()->deflt = std::make_shared<Value>(args[0]);
        return m;
    }
    if (name == "_namedtuple") {  // (name, [fields]) -> a struct class: P(1, 2).x
        need(2);
        expectType(args[0], ValueType::String);
        auto info = std::make_shared<ClassInfo>();
        info->name = args[0].str();
        info->isStruct = true;
        std::vector<Value> fs = args[1].type == ValueType::String ? std::vector<Value>{} : arrayElements(args[1]);
        if (args[1].type == ValueType::String) {  // "x y" / "x, y"
            std::string cur;
            for (char c : args[1].str() + " ") {
                if (c == ' ' || c == ',') { if (!cur.empty()) fs.push_back(Value::fromString(cur)); cur.clear(); }
                else cur += c;
            }
        }
        for (const Value& f : fs) info->structFields.push_back(f.str());
        return Value::fromClass(info);
    }
    if (name == "_isvmgen") {
        need(1);
        return Value::fromBool(vmIsGenFn(args[0]));
    }
    if (name == "_gennew") {
        need(1);
        return vmGenNew(args[0]);
    }
    if (name == "_genclose") {
        need(1);
        vmGenClose(args[0]);
        return Value::null();
    }
    if (name == "_genresume") {  // (handle, kind, sent) -> [ok, value]
        need(3);
        bool ok = false;
        Value v = vmGenResume(args[0], static_cast<int>(args[1].number), args[2], &ok);
        auto pair = std::make_shared<std::vector<Value>>();
        pair->push_back(Value::fromBool(ok));
        if (ok) pair->push_back(std::move(v));
        return Value::fromArray(pair);
    }
    if (name == "iter") {
        need(1);
        Value r;
        std::vector<Value> none;
        if (args.size() == 2) {  // iter(callable, sentinel)
            Value mod = doImport("__gen");
            std::vector<Value> a{args[0], args[1]};
            return callValue((*mod.map())["calliter"], a, Span{});
        }
        if (args[0].type == ValueType::Instance) {
            if (callInstMethod(this, args[0], "__iter__", none, &r)) return r;
            return args[0];
        }
        std::vector<Value> lst{args[0]};
        Value items = pylib::call("list", lst, nullptr, [this](const Value& fn, std::vector<Value>& a) { return callValue(fn, a, Span{}); });
        Value mod = doImport("__gen");
        std::vector<Value> a{items};
        return callValue((*mod.map())["listiter"], a, Span{});
    }
    if (name == "next") {
        if (args.empty() || args.size() > 2) throw RuntimeError("next() butuh 1 atau 2 argumen");
        std::vector<Value> none;
        Value r;
        if (args[0].type == ValueType::Instance) {
            if (callInstMethod(this, args[0], "_nx", none, &r)) {
                std::vector<Value> pair = arrayElements(r);
                if (pair.size() == 2 && pair[0].truthy()) return pair[1];
            } else {
                try {
                    if (!callInstMethod(this, args[0], "__next__", none, &r)) throw RuntimeError("objek bukan iterator");
                    return r;
                } catch (const std::exception& e) {
                    if (std::string(e.what()).find("StopIteration") == std::string::npos) throw;
                }
            }
        } else {
            throw RuntimeError("next(): argumen harus iterator (pakai iter(...) dulu)");
        }
        if (args.size() == 2) return args[1];
        throw RuntimeError("StopIteration");
    }

    if (name == "latar") {
        GC::markDaemonThread();
        return Value::null();
    }

    if (name == "tidur") {
        need(1);
        expectType(args[0], ValueType::Number);
        long long ms = static_cast<long long>(args[0].number);
        if (ms > 0) {
            ValueVectorRootGuard argsRoot(args);
            DepthResetGuard depthReset(exprDepth_);
            GilRelease release;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        return Value::null();
    }

    if (name == "base64_encode") {
        need(1);
        expectType(args[0], ValueType::String);
        return Value::fromString(base64::encode(args[0].str()));
    }

    if (name == "base64_decode") {
        need(1);
        expectType(args[0], ValueType::String);
        try {
            return Value::fromString(base64::decode(args[0].str()));
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("base64_decode(): ") + e.what());
        }
    }

    if (name == "baca_input" || name == "input") {
        if (args.size() >= 1) {
            expectType(args[0], ValueType::String);
            std::cout << args[0].str();
            std::cout.flush();
        }
        std::string line;
        if (std::getline(std::cin, line)) {
            return Value::fromString(line);
        }
        return Value::fromString("");
    }

    if (name == "baca_file") {
        need(1);
        expectType(args[0], ValueType::String);
        std::ifstream f(args[0].str(), std::ios::binary);
        if (!f)
            throw RuntimeError(i18n::tr("baca_file(): nggak bisa buka '", "baca_file(): can't open '") + args[0].str() + "'");
        std::ostringstream buf;
        buf << f.rdbuf();
        return Value::fromString(buf.str());
    }

    if (name == "tulis_file") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::String);
        std::string filePath = args[0].str();
        size_t slashPos = filePath.rfind('/');
        if (slashPos != std::string::npos) {
            std::string dir = filePath.substr(0, slashPos);
            std::string current;
            for (char c : dir) {
                current += c;
                if (c == '/') {
                    struct stat st {};
                    if (stat(current.c_str(), &st) != 0) {
                        mkdir(current.c_str(), 0755);
                    }
                }
            }
            struct stat st {};
            if (stat(dir.c_str(), &st) != 0) {
                mkdir(dir.c_str(), 0755);
            }
        }
        std::ofstream f(filePath, std::ios::binary | std::ios::trunc);
        if (!f) return Value::fromBool(false);
        f << args[1].str();
        return Value::fromBool(static_cast<bool>(f));
    }

    if (name == "file_ada") {
        need(1);
        expectType(args[0], ValueType::String);
        std::ifstream f(args[0].str());
        return Value::fromBool(f.good());
    }

    if (name == "tcp_konek") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::Number);
        int fd = net::tcpConnect(args[0].str(), static_cast<int>(args[1].number));
        if (fd < 0) throw RuntimeError(i18n::tr("tcp_konek(): gagal konek ke ", "tcp_konek(): failed to connect to ") + args[0].str());
        return Value::fromNumber(fd);
    }

    if (name == "tcp_kirim") {
        need(2);
        expectType(args[0], ValueType::Number);
        expectType(args[1], ValueType::String);
        // Returns -1 on failure instead of throwing -- no try/catch in the
        // language yet, callers need to detect a mid-stream send failure inline.
        long sent = net::tcpSend(static_cast<int>(args[0].number), args[1].str());
        return Value::fromNumber(static_cast<double>(sent));
    }

    if (name == "tcp_terima") {
        need(2);
        expectType(args[0], ValueType::Number);
        expectType(args[1], ValueType::Number);
        std::string data = net::tcpRecv(static_cast<int>(args[0].number), static_cast<int>(args[1].number));
        return Value::fromString(data);
    }

    if (name == "tcp_tutup") {
        need(1);
        expectType(args[0], ValueType::Number);
        net::tcpClose(static_cast<int>(args[0].number));
        return Value::null();
    }

    if (name == "http_get") {
        need(1);
        expectType(args[0], ValueType::String);
        net::HttpResponse resp;
        try {
            resp = net::httpRequest("GET", args[0].str(), "", "");
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("http_get(): ") + e.what());
        }
        auto m = std::make_shared<ValueMap>();
        (*m)["status"] = Value::fromNumber(resp.status);
        (*m)["tubuh"] = Value::fromString(resp.body);
        return Value::fromMap(m);
    }

    if (name == "http_post") {
        need(3);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::String);
        expectType(args[2], ValueType::String);
        net::HttpResponse resp;
        try {
            resp = net::httpRequest("POST", args[0].str(), args[1].str(), args[2].str());
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("http_post(): ") + e.what());
        }
        auto m = std::make_shared<ValueMap>();
        (*m)["status"] = Value::fromNumber(resp.status);
        (*m)["tubuh"] = Value::fromString(resp.body);
        return Value::fromMap(m);
    }

    if (name == "qr_baca") {
        need(1);
        expectType(args[0], ValueType::String);
        Value packed;
        try {
            packed = sysmod::call("qr", "qr_baca", {args[0]});
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("qr_baca(): ") + e.what());
        }
        // "<panjang>:<byte>" berurutan.
        auto arr = std::make_shared<std::vector<Value>>();
        const std::string& data = packed.str();
        size_t pos = 0;
        while (pos < data.size()) {
            size_t colon = data.find(':', pos);
            if (colon == std::string::npos) break;
            size_t len = static_cast<size_t>(std::stoul(data.substr(pos, colon - pos)));
            arr->push_back(Value::fromString(data.substr(colon + 1, len)));
            pos = colon + 1 + len;
        }
        return Value::fromArray(arr);
    }

    if (name == "http_dengar") {
        need(2);
        expectType(args[0], ValueType::Number);
        if (!args[1].callable())
            throw RuntimeError(i18n::tr("http_dengar(): argumen ke-2 harus fungsi", "http_dengar(): argument 2 must be a function"));
        int port = static_cast<int>(args[0].number);
        Value handler = args[1];

        // Rooted for the server's whole lifetime -- httpServe() never returns.
        Environment* handlerClosure = handler.type == ValueType::Fn ? handler.fn()->closure : nullptr;
        GcRootGuard handlerGuard(handlerClosure);

        // Every connection runs on its own thread now (net.cpp); this thread
        // (sitting in accept()) never touches Nusantara state again.
        Interpreter::unregisterCurrentThread();

        try {
            net::httpServe(port, [this, handler](const net::HttpRequestIn& req) -> net::HttpResponseOut {
                // Copied by value into a fresh per-connection thread; GIL is
                // already held by the time this runs (net.cpp).
                Interpreter::registerCurrentThread();

                auto headerMap = std::make_shared<ValueMap>();
                for (const auto& [k, v] : req.headers) (*headerMap)[k] = Value::fromString(v);
                auto reqMap = std::make_shared<ValueMap>();
                (*reqMap)["metode"] = Value::fromString(req.method);
                (*reqMap)["path"] = Value::fromString(req.path);
                (*reqMap)["header"] = Value::fromMap(headerMap);
                (*reqMap)["tubuh"] = Value::fromString(req.body);
                (*reqMap)["ip"] = Value::fromString(req.ip);
                (*reqMap)["koneksi"] = Value::fromNumber(req.fd);
                std::vector<Value> handlerArgs{Value::fromMap(reqMap)};

                net::HttpResponseOut out;
                try {
                    Value result = callValue(handler, handlerArgs, Span{});
                    // A handler that streamed its own response (tcp_kirim/
                    // tcp_tutup) signals that via a truthy `_streamed` key.
                    if (result.type == ValueType::Map) {
                        auto streamedIt = result.map()->find("_streamed");
                        if (streamedIt != result.map()->end() && streamedIt->second.truthy()) {
                            out.streamed = true;
                            Interpreter::unregisterCurrentThread();
                            return out;
                        }
                    }
                    if (result.type == ValueType::String) {
                        out.status = 200;
                        out.contentType = "text/plain";
                        out.body = result.str();
                    } else if (result.type == ValueType::Map) {
                        auto& m = *result.map();
                        auto tubuhIt = m.find("tubuh");
                        if (tubuhIt != m.end()) {
                            // Response-descriptor map: {status, tubuh, tipe} --
                            // explicit control over status/content-type.
                            if (auto it = m.find("status"); it != m.end() && it->second.type == ValueType::Number) {
                                out.status = static_cast<int>(it->second.number);
                            }
                            bool hasExplicitType = false;
                            if (auto it = m.find("tipe"); it != m.end() && it->second.type == ValueType::String) {
                                out.contentType = it->second.str();
                                hasExplicitType = true;
                            }
                            if (tubuhIt->second.type == ValueType::String) {
                                out.body = tubuhIt->second.str();
                                if (!hasExplicitType) out.contentType = "text/plain";
                            } else {
                                out.body = json::encode(tubuhIt->second);
                                if (!hasExplicitType) out.contentType = "application/json";
                            }
                        } else {
                            // No "tubuh" key -- the map itself is auto-JSON-encoded.
                            out.contentType = "application/json";
                            out.body = json::encode(result);
                        }
                    } else {
                        out.contentType = "application/json";
                        out.body = json::encode(result);
                    }
                } catch (const std::exception& e) {
                    // Must never let an exception escape this thread's entry
                    // point -- that would std::terminate the whole process.
                    out.status = 500;
                    out.contentType = "text/plain";
                    out.body = std::string("Internal Server Error: ") + e.what();
                    std::cerr << "[http_dengar] " << e.what() << '\n';
                }

                Interpreter::unregisterCurrentThread();
                // GIL stays held on return -- net.cpp unlocks it after using `out`.
                return out;
            });
        } catch (const std::exception& e) {
            throw RuntimeError(e.what());
        }
        return Value::null();  // unreachable -- httpServe() only returns by throwing
    }

    if (name == "email_kirim") {
        need(6);
        expectType(args[0], ValueType::String);  // host
        expectType(args[1], ValueType::Number);  // port
        expectType(args[2], ValueType::String);  // dari
        expectType(args[3], ValueType::String);  // ke
        expectType(args[4], ValueType::String);  // subjek
        expectType(args[5], ValueType::String);  // isi
        bool ok = net::smtpSend(args[0].str(), static_cast<int>(args[1].number), args[2].str(),
                                 args[3].str(), args[4].str(), args[5].str());
        return Value::fromBool(ok);
    }

    if (name == "jalankan_perintah") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::Array);
        std::vector<Value> argsArr = arrayElements(args[1]);
        std::vector<std::string> cmdArgs;
        cmdArgs.reserve(argsArr.size());
        for (const Value& v : argsArr) {
            if (v.type != ValueType::String) {
                throw RuntimeError(i18n::tr("jalankan_perintah(): semua elemen larik argumen harus teks",
                                        "jalankan_perintah(): every element of the args array must be a string"));
            }
            cmdArgs.push_back(v.str());
        }
        proc::ExecResult result;
        try {
            // Long-running child would otherwise pin exprDepth_ above 0 for
            // the duration and starve GC safepoints process-wide.
            ValueVectorRootGuard argsRoot(args);
            DepthResetGuard depthReset(exprDepth_);
            result = proc::run(args[0].str(), cmdArgs);
        } catch (const std::exception& e) {
            throw RuntimeError(e.what());
        }
        auto m = std::make_shared<ValueMap>();
        (*m)["status"] = Value::fromNumber(result.exitCode);
        (*m)["keluaran"] = Value::fromString(result.out);
        (*m)["error"] = Value::fromString(result.err);
        return Value::fromMap(m);
    }

    if (name == "proc_stream_mulai") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::Array);
        std::vector<Value> argsArr = arrayElements(args[1]);
        std::vector<std::string> cmdArgs;
        cmdArgs.reserve(argsArr.size());
        for (const Value& v : argsArr) {
            if (v.type != ValueType::String) {
                throw RuntimeError(i18n::tr("proc_stream_mulai(): semua elemen larik argumen harus teks",
                                        "proc_stream_mulai(): every element of the args array must be a string"));
            }
            cmdArgs.push_back(v.str());
        }
        proc::StreamHandle h = proc::startStream(args[0].str(), cmdArgs);
        if (h.pid < 0) return Value::null();
        auto m = std::make_shared<ValueMap>();
        (*m)["pid"] = Value::fromNumber(h.pid);
        (*m)["fd"] = Value::fromNumber(h.fd);
        return Value::fromMap(m);
    }

    if (name == "proc_stream_baca") {
        need(2);
        expectType(args[0], ValueType::Number);
        expectType(args[1], ValueType::Number);
        int fd = static_cast<int>(args[0].number);
        int maxLen = static_cast<int>(args[1].number);
        // Same safepoint-starvation guard as jalankan_perintah above.
        ValueVectorRootGuard argsRoot(args);
        DepthResetGuard depthReset(exprDepth_);
        return Value::fromString(proc::readStream(fd, maxLen));
    }

    if (name == "proc_stream_tutup") {
        need(2);
        expectType(args[0], ValueType::Number);
        expectType(args[1], ValueType::Number);
        proc::closeStream(static_cast<pid_t>(args[0].number), static_cast<int>(args[1].number));
        return Value::null();
    }

    if (name == "sha256_hex") {
        need(1);
        expectType(args[0], ValueType::String);
        return Value::fromString(sha256::hexDigest(args[0].str()));
    }

    if (name == "hmac_sha256_hex") {
        need(2);
        expectType(args[0], ValueType::String);  // kunci
        expectType(args[1], ValueType::String);  // pesan
        return Value::fromString(hmac::sha256Hex(args[0].str(), args[1].str()));
    }

    if (name == "jwt_buat") {
        if (args.size() != 2 && args.size() != 3) {
            throw RuntimeError(i18n::tr("jwt_buat() butuh 2 atau 3 argumen (payload, secret, [detik_kedaluwarsa]), dapat ",
                                        "jwt_buat() expects 2 or 3 args (payload, secret, [expiry_seconds]), got ") +
                                std::to_string(args.size()));
        }
        expectType(args[0], ValueType::Map);
        expectType(args[1], ValueType::String);
        Value payload = args[0];
        if (args.size() == 3) {
            expectType(args[2], ValueType::Number);
            // Copy-on-write: don't mutate the caller's own map, build a
            // fresh one with "exp" added.
            auto withExp = std::make_shared<ValueMap>(*args[0].map());
            double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            (*withExp)["exp"] = Value::fromNumber(now + args[2].number);
            payload = Value::fromMap(withExp);
        }
        try {
            return Value::fromString(jwt::create(payload, args[1].str()));
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("jwt_buat(): ") + e.what());
        }
    }

    if (name == "jwt_verifikasi") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::String);
        return jwt::verify(args[0].str(), args[1].str());
    }

    if (name == "muat_plugin") {
        need(1);
        expectType(args[0], ValueType::String);
        std::string path;
        if (sysplugin::isBareName(args[0].str())) {
            Value builtin;
            if (plugin::loadBuiltin(args[0].str(), builtin)) return builtin;
            // Bare name (e.g. "http") -- a system module, resolved next to
            // the nusa binary itself.
            std::string dir = sysplugin::systemPluginDir();
            if (dir.empty()) {
                throw RuntimeError(i18n::tr("muat_plugin('", "muat_plugin('") + args[0].str() +
                                    i18n::tr("'): nggak bisa nemuin lokasi modul sistem (gagal baca /proc/self/exe)",
                                              "'): can't find system module location (failed to read /proc/self/exe)"));
            }
            path = dir + "/" + args[0].str() + ".so";
        } else {
            // Explicit path -- resolved against the calling file's own
            // directory (like impor()), not the process cwd.
            path = joinPath(importDirStack_.back(), args[0].str());
        }
        try {
            return plugin::load(path);
        } catch (const std::exception& e) {
            throw RuntimeError(e.what());
        }
    }

    if (name == "elemen") {
        if (args.size() < 1) throw RuntimeError("elemen() butuh minimal 1 argumen");
        Value tag = args[0];
        Value props = args.size() >= 2 ? args[1] : Value::newMap();
        Value children = args.size() >= 3 ? args[2] : Value::fromArray(std::make_shared<std::vector<Value>>());

        if (tag.type == ValueType::Fn) {
            std::vector<Value> callArgs = { props };
            return callFunction(tag.fnShared(), callArgs, Span{});
        }

        Value m = Value::newMap();
        (*m.map())["tipe"] = Value::fromString("elemen");
        (*m.map())["tag"] = tag;
        (*m.map())["props"] = props;
        (*m.map())["children"] = children;
        return m;
    }

    if (name == "teks") {
        need(1);
        Value m = Value::newMap();
        (*m.map())["tipe"] = Value::fromString("teks");
        (*m.map())["isi"] = Value::fromString(args[0].stringify());
        return m;
    }

    if (name == "byte_di") {
        need(2);
        expectType(args[0], ValueType::String);
        expectType(args[1], ValueType::Number);
        long idx = static_cast<long>(args[1].number);
        if (idx < 0 || idx >= static_cast<long>(args[0].str().size()))
            throw RuntimeError(i18n::tr("byte_di(): indeks di luar jangkauan", "byte_di(): index out of range"));
        return Value::fromNumber(static_cast<unsigned char>(args[0].str()[static_cast<size_t>(idx)]));
    }

    if (name == "teks_dari") {
        need(1);
        expectType(args[0], ValueType::Number);
        long b = static_cast<long>(args[0].number);
        if (b < 0 || b > 255)
            throw RuntimeError(i18n::tr("teks_dari(): byte harus 0-255", "teks_dari(): byte must be 0-255"));
        return Value::fromString(std::string(1, static_cast<char>(static_cast<unsigned char>(b))));
    }

    // range(stop) / range(start, stop[, step]) as an array of numbers. The
    // parser turns `for i in range(...)` into a counting loop, so this is
    // only for range() used as a plain value.
    if (name == "rentang") {
        if (args.empty() || args.size() > 3) {
            throw RuntimeError(i18n::tr("rentang() butuh 1 sampai 3 argumen", "range() expects 1 to 3 arguments"));
        }
        for (const auto& a : args) expectType(a, ValueType::Number);
        double start = args.size() >= 2 ? args[0].number : 0.0;
        double stop = args.size() >= 2 ? args[1].number : args[0].number;
        double step = args.size() == 3 ? args[2].number : 1.0;
        if (step == 0) throw RuntimeError(i18n::tr("rentang(): langkah nggak boleh 0", "range(): step must not be 0"));
        auto out = std::make_shared<std::vector<Value>>();
        for (double v = start; step > 0 ? v < stop : v > stop; v += step) {
            out->push_back(Value::fromNumber(v));
            if (out->size() > 100000000) throw RuntimeError(i18n::tr("rentang(): kegedean", "range(): too large"));
        }
        return Value::fromArray(out);
    }

    // x[a:b] with Python rules: missing bound = start/end, negative bound counts from the end.
    if (name == "__iris") {
        if (args.size() != 3 && args.size() != 4) need(3);
        const Value& v = args[0];
        long long len;
        bool isStr = v.type == ValueType::String;
        if (isStr) len = static_cast<long long>(v.str().size());
        else if (v.type == ValueType::Array) len = static_cast<long long>(v.array()->size());
        else if (v.type == ValueType::VmArray) len = static_cast<long long>(v.vmArray()->numeric ? v.vmArray()->nums.size() : v.vmArray()->boxed->size());
        else throw RuntimeError(std::string(i18n::tr("Tipe '", "Type '")) + v.typeName() + i18n::tr("' nggak bisa di-slice", "' can't be sliced"));
        long long step = 1;
        if (args.size() == 4 && args[3].type != ValueType::Null) {
            if (args[3].type != ValueType::Number) throw RuntimeError(i18n::tr("Langkah slice harus angka", "Slice step must be a number"));
            step = static_cast<long long>(args[3].number);
            if (step == 0) throw RuntimeError(i18n::tr("Langkah slice nggak boleh 0", "Slice step cannot be zero"));
        }
        auto bound = [&](const Value& b, long long dflt, long long lo, long long hi) {
            if (b.type == ValueType::Null) return dflt;
            if (b.type != ValueType::Number) throw RuntimeError(i18n::tr("Batas slice harus angka", "Slice bounds must be numbers"));
            long long i = static_cast<long long>(b.number);
            if (i < 0) i += len;
            return std::max<long long>(lo, std::min(i, hi));
        };
        if (step == 1) {
            std::vector<Value> sliceArgs = {v, Value::fromNumber(static_cast<double>(bound(args[1], 0, 0, len))),
                                            Value::fromNumber(static_cast<double>(bound(args[2], len, 0, len)))};
            return callBuiltin("potong", sliceArgs);
        }
        long long lo, hi;
        if (step > 0) { lo = bound(args[1], 0, 0, len); hi = bound(args[2], len, 0, len); }
        else { lo = bound(args[1], len - 1, -1, len - 1); hi = bound(args[2], -1, -1, len - 1); }
        if (isStr) {
            std::string out;
            const std::string& s = v.str();
            for (long long k = lo; step > 0 ? k < hi : k > hi; k += step) out += s[static_cast<size_t>(k)];
            return Value::fromString(out);
        }
        std::vector<Value> src = arrayElements(v);
        auto out = std::make_shared<std::vector<Value>>();
        for (long long k = lo; step > 0 ? k < hi : k > hi; k += step) out->push_back(src[static_cast<size_t>(k)]);
        return Value::fromArray(out);
    }

    // What `for x in <expr>` walks: arrays and strings as they are, maps as
    // their key list.
    if (name == "__iter") {
        need(1);
        const Value& v = args[0];
        if (v.type == ValueType::Array || v.type == ValueType::VmArray || v.type == ValueType::String) return v;
        if (v.type == ValueType::Instance) {
            // Lazy iteration: a generator (or anything with __len__/__getitem__ semantics) is walked in place.
            std::vector<Value> none;
            Value r = v;
            if (callInstMethod(this, v, "__iter__", none, &r) && r.type != ValueType::Instance) {
                std::vector<Value> a{r};
                return callBuiltin("__iter", a);
            }
            if (r.type == ValueType::Instance) {
                std::shared_ptr<ClassInfo> owner;
                if (!lookupMethod(r.instance()->classInfo, "__len__", &owner)) {
                    Value mod = doImport("__gen");
                    std::vector<Value> a{r};
                    return callValue((*mod.map())["adapt"], a, Span{});
                }
            }
            return r;
        }
        if (v.type == ValueType::Map) {
            auto keys = std::make_shared<std::vector<Value>>();
            for (const auto& [k, val] : *v.map()) keys->push_back(Value::fromString(k));
            return Value::fromArray(keys);
        }
        throw RuntimeError(i18n::tr("Nilai bertipe '", "A value of type '") + std::string(v.typeName()) +
                            i18n::tr("' nggak bisa diiterasi", "' can't be iterated"));
    }

    if (name == "peta_kunci") {
        need(1);
        expectType(args[0], ValueType::Map);
        auto keys = std::make_shared<std::vector<Value>>();
        keys->reserve(args[0].map()->size());
        for (const auto& [k, v] : *args[0].map()) keys->push_back(Value::fromString(k));
        return Value::fromArray(keys);
    }

    if (name == "ambil_env") {
        if (args.size() != 1 && args.size() != 2) {
            throw RuntimeError(i18n::tr("ambil_env() butuh 1 atau 2 argumen (kunci, [default]), dapat ",
                                        "ambil_env() expects 1 or 2 args (key, [default]), got ") +
                                std::to_string(args.size()));
        }
        expectType(args[0], ValueType::String);
        const char* val = std::getenv(args[0].str().c_str());
        if (val && val[0] != '\0') return Value::fromString(val);
        if (args.size() == 2) return args[1];
        return Value::null();
    }

    if (name == "json_decode") {
        need(1);
        expectType(args[0], ValueType::String);
        try {
            return json::decode(args[0].str());
        } catch (const std::exception& e) {
            throw RuntimeError(std::string("json_decode(): ") + e.what());
        }
    }

    if (name == "json_decode_aman") {
        need(1);
        expectType(args[0], ValueType::String);
        // Non-throwing json_decode(): null on any parse failure instead of
        // a RuntimeError -- for bodies that might not be JSON at all.
        try {
            return json::decode(args[0].str());
        } catch (const std::exception&) {
            return Value::null();
        }
    }

    if (name == "jalan") {
#ifdef __EMSCRIPTEN__
        throw RuntimeError("jalan(): goroutine belum didukung di build WASM browser");
#else
        if (args.empty() || !args[0].callable()) {
            throw RuntimeError(i18n::tr("jalan(): argumen pertama harus fungsi", "jalan(): first argument must be a function"));
        }
        Value fn = args[0];
        std::vector<Value> goroutineArgs(args.begin() + 1, args.end());

        GC::liveGoroutines++;

        // Heap-allocated to hand off to pthread via void*; std::thread has
        // no way to set stack size, hence raw pthread.
        struct ArgGoroutine {
            Interpreter* interp;
            Value fn;
            std::vector<Value> args;
            // Set by the new thread right after it roots fn's closure; jalan()
            // waits on this before returning so the closure is never unprotected.
            std::atomic<bool> rooted{false};
        };
        auto* muatan = new ArgGoroutine{this, fn, goroutineArgs, {}};

        auto jalankanGoroutine = [](void* p) -> void* {
            std::unique_ptr<ArgGoroutine> m(static_cast<ArgGoroutine*>(p));
            m->interp->jalankanBadanGoroutine(m->fn, m->args, &m->rooted);
            return nullptr;
        };

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kUkuranStackGoroutine);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        pthread_t tid;
        int rc = pthread_create(&tid, &attr, jalankanGoroutine, muatan);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete muatan;
            GC::liveGoroutines--;
            throw RuntimeError("jalan(): gagal bikin thread goroutine");
        }
        // Don't return until the new thread has rooted fn's closure itself --
        // otherwise a GC checkpoint right after this call unwinds could sweep
        // it before the new thread gets a chance to protect it.
        while (!muatan->rooted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Value::null();
#endif
    }

    if (name == "kanal_baru") {
        if (args.size() > 1) {
            throw RuntimeError(i18n::tr("kanal_baru() butuh 0 atau 1 argumen (kapasitas), dapat ",
                                        "kanal_baru() expects 0 or 1 args (capacity), got ") +
                                std::to_string(args.size()));
        }
        auto chan = std::make_shared<ChannelState>();
        if (args.size() == 1) {
            expectType(args[0], ValueType::Number);
            if (args[0].number > 0) chan->capacity = static_cast<size_t>(args[0].number);
        }
        return Value::fromChannel(chan);
    }

    if (name == "kanal_kirim") {
        need(2);
        if (args[0].type != ValueType::Channel)
            throw RuntimeError(i18n::tr("kanal_kirim(): argumen ke-1 harus kanal", "kanal_kirim(): argument 1 must be a channel"));
        auto chan = args[0].channelShared();
        // Order matters -- argsRoot/depthReset before chan->mu, GilRelease
        // outliving chan->mu (dtors run in reverse): real, reproduced
        // AB-BA deadlocks against GC::collectNow() and the GIL. Do not reorder.
        ValueVectorRootGuard argsRoot(args);
        DepthResetGuard depthReset(exprDepth_);
        GilRelease release;
        std::unique_lock<std::mutex> lock(chan->mu);
        if (chan->closed)
            throw RuntimeError(i18n::tr("kanal_kirim(): nggak bisa kirim ke kanal yang udah ditutup",
                                          "kanal_kirim(): can't send to a closed channel"));
        if (chan->capacity > 0 && chan->queue.size() >= chan->capacity) {
            chan->notFull.wait(lock, [&] { return chan->queue.size() < chan->capacity || chan->closed; });
            if (chan->closed)
                throw RuntimeError(i18n::tr("kanal_kirim(): kanal ditutup pas nunggu ngirim",
                                              "kanal_kirim(): channel was closed while waiting to send"));
        }
        chan->queue.push_back(args[1]);
        lock.unlock();
        chan->notEmpty.notify_one();
        return Value::null();
    }

    if (name == "kanal_terima") {
        need(1);
        if (args[0].type != ValueType::Channel)
            throw RuntimeError(i18n::tr("kanal_terima(): argumen harus kanal", "kanal_terima(): argument must be a channel"));
        auto chan = args[0].channelShared();
        // Same lock-ordering rule as kanal_kirim above.
        ValueVectorRootGuard argsRoot(args);
        DepthResetGuard depthReset(exprDepth_);
        GilRelease release;
        std::unique_lock<std::mutex> lock(chan->mu);
        if (chan->queue.empty() && !chan->closed) {
            chan->notEmpty.wait(lock, [&] { return !chan->queue.empty() || chan->closed; });
        }
        if (chan->queue.empty()) return Value::null();  // closed and drained
        Value v = chan->queue.front();
        chan->queue.pop_front();
        lock.unlock();
        chan->notFull.notify_one();
        return v;
    }

    if (name == "kanal_tutup") {
        need(1);
        if (args[0].type != ValueType::Channel)
            throw RuntimeError(i18n::tr("kanal_tutup(): argumen harus kanal", "kanal_tutup(): argument must be a channel"));
        auto chan = args[0].channelShared();
        std::lock_guard<std::mutex> lock(chan->mu);
        chan->closed = true;
        chan->notEmpty.notify_all();
        chan->notFull.notify_all();
        return Value::null();
    }

    if (name == "kanal_panjang") {
        need(1);
        if (args[0].type != ValueType::Channel)
            throw RuntimeError(i18n::tr("kanal_panjang(): argumen harus kanal", "kanal_panjang(): argument must be a channel"));
        auto chan = args[0].channelShared();
        std::lock_guard<std::mutex> lock(chan->mu);
        return Value::fromNumber(static_cast<double>(chan->queue.size()));
    }

    if (name == "pilih_kanal") {
        need(1);
        std::vector<Value> chans;
        if (args[0].type == ValueType::VmArray) {
            auto* st = args[0].vmArray();
            if (st->numeric) throw RuntimeError(i18n::tr("pilih_kanal(): semua elemen larik harus kanal", "pilih_kanal(): every element of the array must be a channel"));
            for (const auto& v : *st->boxed) chans.push_back(v);
        } else {
            expectType(args[0], ValueType::Array);
            auto arr = args[0].arrayShared();
            for (const auto& v : *arr) chans.push_back(v);
        }
        for (const Value& c : chans) {
            if (c.type != ValueType::Channel)
                throw RuntimeError(i18n::tr("pilih_kanal(): semua elemen larik harus kanal",
                                              "pilih_kanal(): every element of the array must be a channel"));
        }
        if (chans.empty())
            throw RuntimeError(i18n::tr("pilih_kanal(): larik kanal kosong", "pilih_kanal(): empty channel array"));
        while (true) {
            for (size_t i = 0; i < chans.size(); i++) {
                auto* chan = chans[i].channel();
                std::unique_lock<std::mutex> lock(chan->mu);
                if (!chan->queue.empty()) {
                    Value v = chan->queue.front();
                    chan->queue.pop_front();
                    lock.unlock();
                    chan->notFull.notify_one();
                    auto m = std::make_shared<ValueMap>();
                    (*m)["indeks"] = Value::fromNumber(static_cast<double>(i));
                    (*m)["nilai"] = v;
                    return Value::fromMap(m);
                }
                if (chan->closed) {
                    auto m = std::make_shared<ValueMap>();
                    (*m)["indeks"] = Value::fromNumber(static_cast<double>(i));
                    (*m)["nilai"] = Value::null();
                    return Value::fromMap(m);
                }
            }
            // Nothing ready -- release GIL, sleep briefly, poll again. Not
            // event-driven (unlike Go's select) but no missed-wakeup window
            // since every channel's state is re-checked each pass.
            ValueVectorRootGuard argsRoot(args);
            DepthResetGuard depthReset(exprDepth_);
            GilRelease release;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (name == "wg_baru") {
        need(0);
        return Value::fromWaitGroup(std::make_shared<WaitGroupState>());
    }

    if (name == "wg_tambah") {
        need(2);
        if (args[0].type != ValueType::WaitGroup)
            throw RuntimeError(i18n::tr("wg_tambah(): argumen ke-1 harus waitgroup", "wg_tambah(): argument 1 must be a waitgroup"));
        expectType(args[1], ValueType::Number);
        auto wg = args[0].waitgroupShared();
        {
            std::lock_guard<std::mutex> lock(wg->mu);
            wg->counter += static_cast<long>(args[1].number);
            if (wg->counter < 0)
                throw RuntimeError(i18n::tr("wg_tambah(): counter waitgroup jadi negatif", "wg_tambah(): waitgroup counter went negative"));
        }
        wg->cv.notify_all();
        return Value::null();
    }

    if (name == "wg_selesai") {
        need(1);
        if (args[0].type != ValueType::WaitGroup)
            throw RuntimeError(i18n::tr("wg_selesai(): argumen harus waitgroup", "wg_selesai(): argument must be a waitgroup"));
        auto wg = args[0].waitgroupShared();
        {
            std::lock_guard<std::mutex> lock(wg->mu);
            wg->counter -= 1;
            if (wg->counter < 0) {
                throw RuntimeError(i18n::tr("wg_selesai(): counter waitgroup jadi negatif (kelebihan manggil wg_selesai)",
                                          "wg_selesai(): waitgroup counter went negative (wg_selesai called too many times)"));
            }
        }
        wg->cv.notify_all();
        return Value::null();
    }

    if (name == "wg_tunggu") {
        need(1);
        if (args[0].type != ValueType::WaitGroup)
            throw RuntimeError(i18n::tr("wg_tunggu(): argumen harus waitgroup", "wg_tunggu(): argument must be a waitgroup"));
        auto wg = args[0].waitgroupShared();
        // Same GIL/mutex ordering rule as channels above.
        GilRelease release;
        std::unique_lock<std::mutex> lock(wg->mu);
        ValueVectorRootGuard argsRoot(args);
        DepthResetGuard depthReset(exprDepth_);
        wg->cv.wait(lock, [&] { return wg->counter <= 0; });
        return Value::null();
    }

    if (name == "gc_info") {
        need(0);
        GC& gc = GC::instance();
        std::ostringstream oss;
        oss << "hidup=" << gc.liveCount() << " total_alokasi=" << gc.totalAllocated()
            << " total_bebas=" << gc.totalFreed() << " koleksi=" << gc.collections();
        return Value::fromString(oss.str());
    }

    if (name == "gc_paksa") {
        need(0);
        GC::instance().mintaTrim();
        GC::instance().requestCollection();
        return Value::null();
    }

    if (name == "impor") {
        need(1);
        expectType(args[0], ValueType::String);
        return doImport(args[0].str());
    }

    throw RuntimeError(i18n::tr("Builtin nggak dikenal '", "Unknown builtin '") + name + "'");
}

// ---- module system ----

namespace {
// `base` as a module: the file itself, base.ns, or a package directory with
// __init__.ns (Python) / index.ns (older nusantara_modules layout).
std::string resolveModuleAt(const std::string& base) {
    if (isRegularFile(base)) return base;
    if (std::ifstream(base + ".ns").good()) return base + ".ns";
    if (std::ifstream(base + "/__init__.ns").good()) return base + "/__init__.ns";
    if (std::ifstream(base + "/index.ns").good()) return base + "/index.ns";
    return "";
}

std::string tryModulesDirAt(const std::string& dir, const std::string& rawPath) {
    return resolveModuleAt(dir.empty() ? ("nusantara_modules/" + rawPath)
                                        : (dir + "/nusantara_modules/" + rawPath));
}

std::string toAbsoluteDir(const std::string& dir) {
    if (!dir.empty() && dir[0] == '/') return dir;
    char buf[4096];
    if (getcwd(buf, sizeof buf) == nullptr) return dir;
    std::string cwd(buf);
    if (dir.empty() || dir == ".") return cwd;
    return cwd + "/" + dir;
}
}  // namespace

Value Interpreter::doImport(const std::string& rawPath) {
    std::string path;

    bool isExplicitRelativeOrAbs = (!rawPath.empty() && (rawPath[0] == '/' || rawPath.rfind("./", 0) == 0 || rawPath.rfind("../", 0) == 0));

    if (!isExplicitRelativeOrAbs) {
        std::string dir = toAbsoluteDir(importDirStack_.empty() ? "." : importDirStack_.back());
        // Python-style: a sibling module or package next to the importing file wins.
        path = resolveModuleAt(dir + "/" + rawPath);
        while (path.empty()) {
            std::string found = tryModulesDirAt(dir, rawPath);
            if (!found.empty()) { path = found; break; }
            if (dir.empty() || dir == "/") break;
            std::string parent = dirName(dir);
            if (parent == dir) break;
            dir = parent;
        }
        if (path.empty()) {
            std::string globalDir = sysplugin::globalModulesDir();
            if (!globalDir.empty()) path = resolveModuleAt(globalDir + "/" + rawPath);
        }
    }

    if (path.empty()) {
        path = joinPath(importDirStack_.back(), rawPath);
        if (!isRegularFile(path)) {
            std::string fallback = "nusantara_modules/" + rawPath;
            if (isRegularFile(fallback)) path = fallback;
            else if (std::ifstream(fallback + ".ns").good()) path = fallback + ".ns";
            else if (std::ifstream(fallback + "/index.ns").good()) path = fallback + "/index.ns";
        }
    }

    const char* embedded = nullptr;
    if (path.empty() || (!isRegularFile(path) && !std::ifstream(path).good())) {
        embedded = pystd::embeddedModule(rawPath);
        if (embedded) path = "<std:" + rawPath + ">";
    }

    auto cached = moduleCache_.find(path);
    if (cached != moduleCache_.end()) return cached->second;

    for (const std::string& active : importStack_) {
        if (active == path)
            throw RuntimeError(i18n::tr("impor melingkar (circular import) terdeteksi: '",
                                          "circular import detected: '") + path + "'");
    }

    std::string source;
    if (embedded) {
        source = embedded;
    } else {
        std::ifstream file(path);
        if (!file)
            throw RuntimeError(i18n::tr("impor(): nggak bisa buka '", "impor(): can't open '") + path +
                                i18n::tr("' (dari '", "' (from '") + rawPath + "')");
        std::ostringstream buf;
        buf << file.rdbuf();
        source = buf.str();
    }

    std::unique_ptr<Program> program;
    try {
        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        program = parser.parse();
    } catch (const LexError& e) {
        throw RuntimeError("impor('" + path + "'): " + e.what());
    } catch (const ParseError& e) {
        throw RuntimeError("impor('" + path + "'): " + e.what());
    }

    // Module scope is a child of globals_ (so it sees all builtins),
    // but otherwise isolated -- nothing it defines leaks back into the
    // importer except what's explicitly returned below.
    Environment* modEnv = GC::instance().alloc(globals_);
    GcRootGuard guard(modEnv);

    // Prefer running the module on the bytecode VM (its functions then stay VM
    // functions, not slow AST ones) whenever a VM program is driving the script.
    std::unique_ptr<VmProgram> vmModule;
    if (vmIsActive()) {
        try {
            vmModule = vmCompile(*program);
        } catch (const VmCompileError&) {
            vmModule.reset();  // something the VM can't compile yet: interpret this module
        }
    }

    importStack_.push_back(path);
    importDirStack_.push_back(dirName(path));
    if (vmModule) {
        GC::instance().addPermanentRoot(modEnv);  // VM closures reach it via a raw pointer
        vmRunModule(*vmModule, modEnv, this);
        importedVmPrograms_.push_back(std::move(vmModule));
    } else {
        for (const auto& stmt : program->statements) {
            if (exprDepth_ == 0) GC::instance().collectIfNeeded();
            exec(stmt.get(), modEnv);
            if (g_pending != kPendNone) { g_pending = kPendNone; break; }
        }
    }
    importDirStack_.pop_back();
    importStack_.pop_back();

    auto exported = std::make_shared<ValueMap>(modEnv->vars());
    Value result = Value::fromMap(std::move(exported));

    // FnDeclStmt* pointers inside any exported closures point into
    // `program`'s AST -- keep it alive for the rest of the process,
    // same reasoning as the REPL's `history` vector in main.cpp.
    importedPrograms_.push_back(std::move(program));
    moduleCache_[path] = result;
    return result;
}
