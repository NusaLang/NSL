#include "interpreter.hpp"

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
#include "qr.hpp"
#include "sysplugin.hpp"
#include "vm.hpp"

namespace {

// Used internally to unwind out of a function body on `hasil`/
// `berhenti`/`lanjut`.
struct ReturnSignal { Value value; };
struct BreakSignal {};
struct ContinueSignal {};

std::string thrownValueMessage(const Value& v) {
    if (v.type == ValueType::Map) {
        auto it = v.map()->find("pesan");
        if (it != v.map()->end() && it->second.type == ValueType::String) return it->second.str();
    }
    if (v.type == ValueType::String) return v.str();
    return i18n::tr("Error dilempar: ", "Thrown error: ") + v.stringify();
}

class ThrownValue : public RuntimeError {
public:
    explicit ThrownValue(Value v) : RuntimeError(thrownValueMessage(v)), value_(std::move(v)) {}
    const Value& value() const { return value_; }
private:
    Value value_;
};

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

bool valuesEqual(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case ValueType::Null: return true;
        case ValueType::Bool: return a.boolean() == b.boolean();
        case ValueType::Number: return a.number == b.number;
        case ValueType::String: return a.str() == b.str();
        case ValueType::Fn: return a.fn() == b.fn();
        case ValueType::Builtin: return a.builtinName() == b.builtinName();
        case ValueType::Array: return a.array() == b.array();
        case ValueType::Map: return a.map() == b.map();
        case ValueType::Channel: return a.channel() == b.channel();
        case ValueType::WaitGroup: return a.waitgroup() == b.waitgroup();
        case ValueType::Native: return a.native() == b.native();
        case ValueType::Class: return a.klass() == b.klass();
        case ValueType::Instance: return a.instance() == b.instance();
        case ValueType::VmFn: return a.vmClosure() == b.vmClosure();
        case ValueType::VmArray: return a.vmArray() == b.vmArray();
    }
    return false;
}

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

Value indexGet(const Value& target, const Value& idx) {
    if (target.type == ValueType::VmArray) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        VmArrayState& st = *target.vmArray();
        size_t size = st.numeric ? st.nums.size() : st.boxed->size();
        if (i < 0 || static_cast<size_t>(i) >= size) {
            throw RuntimeError(i18n::tr("Index larik di luar batas: ", "Array index out of bounds: ") + std::to_string(i));
        }
        return st.numeric ? Value::fromNumber(st.nums[static_cast<size_t>(i)])
                          : (*st.boxed)[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Array) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index larik harus angka", "Array index must be a number"));
        long long i = static_cast<long long>(idx.number);
        if (i < 0 || static_cast<size_t>(i) >= target.array()->size()) {
            throw RuntimeError(i18n::tr("Index larik di luar batas: ", "Array index out of bounds: ") + std::to_string(i));
        }
        return (*target.array())[static_cast<size_t>(i)];
    }
    if (target.type == ValueType::Map) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Kunci peta harus teks", "Map key must be a string"));
        auto it = target.map()->find(idx.str());
        if (it == target.map()->end()) return Value::null();
        return it->second;
    }
    if (target.type == ValueType::String) {
        if (idx.type != ValueType::Number) throw RuntimeError(i18n::tr("Index teks harus angka", "String index must be a number"));
        long long i = static_cast<long long>(idx.number);
        if (i < 0 || static_cast<size_t>(i) >= target.str().size()) {
            throw RuntimeError(i18n::tr("Index teks di luar batas: ", "String index out of bounds: ") + std::to_string(i));
        }
        return Value::fromString(std::string(1, target.str()[static_cast<size_t>(i)]));
    }
    if (target.type == ValueType::Instance) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Kunci objek harus teks", "Object key must be a string"));
        auto fit = target.instance()->fields->find(idx.str());
        if (fit != target.instance()->fields->end()) return fit->second;
        auto method = lookupMethod(target.instance()->classInfo, idx.str());
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
        if (i < 0) throw RuntimeError(i18n::tr("Index larik negatif nggak valid: ", "Negative array index is invalid: ") + std::to_string(i));
        VmArrayState& st = *target.vmArray();
        size_t size = st.numeric ? st.nums.size() : st.boxed->size();
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
        if (i < 0) throw RuntimeError(i18n::tr("Index larik negatif nggak valid: ", "Negative array index is invalid: ") + std::to_string(i));
        auto& vec = *target.array();
        if (static_cast<size_t>(i) >= vec.size()) vec.resize(static_cast<size_t>(i) + 1);
        vec[static_cast<size_t>(i)] = value;
        return;
    }
    if (target.type == ValueType::Map) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Kunci peta harus teks", "Map key must be a string"));
        (*target.map())[idx.str()] = value;
        return;
    }
    if (target.type == ValueType::Instance) {
        if (idx.type != ValueType::String) throw RuntimeError(i18n::tr("Kunci objek harus teks", "Object key must be a string"));
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
        "huruf_besar", "huruf_kecil", "ke_teks", "ke_angka", "tipe", "waktu",
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
    };
    return aliases;
}

}  // namespace

thread_local int Interpreter::exprDepth_ = 0;
thread_local int Interpreter::callDepth_ = 0;

void Interpreter::registerCurrentThread() { GC::instance().registerThread(&exprDepth_); }
void Interpreter::unregisterCurrentThread() { GC::instance().unregisterThread(&exprDepth_); }

Interpreter::Interpreter(std::string entryDir) {
    globals_ = GC::instance().alloc(nullptr);
    GC::instance().setGlobals(globals_);
    for (const std::string& name : builtinNames()) {
        globals_->define(name, Value::builtin(name));
    }
    for (const auto& [aliasName, canonical] : builtinAliases()) {
        globals_->define(aliasName, Value::builtin(canonical));
    }
    importDirStack_.push_back(std::move(entryDir));
}

void Interpreter::run(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (exprDepth_ == 0) GC::instance().collectIfNeeded();
        exec(stmt.get(), globals_);
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
            }
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
            auto ns = std::make_shared<std::unordered_map<std::string, Value>>();
            for (const auto& variant : node->variants) {
                auto state = std::make_shared<InstanceState>();
                state->classInfo = info;
                state->fields = std::make_shared<std::unordered_map<std::string, Value>>();
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
                        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
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
                Environment* child = GC::instance().alloc(env);
                GcRootGuard guard(child);
                execBlock(node->finallyBlock.get(), child);
            }
            return;
        }
        case StmtKind::Throw: {
            auto* node = static_cast<const ThrowStmt*>(stmt);
            throw ThrownValue(eval(node->value.get(), env));
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
                try {
                    execBlock(node->body.get(), child);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
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
                bool doBreak = false;
                try {
                    execBlock(node->body.get(), iterEnv);
                } catch (BreakSignal&) {
                    doBreak = true;
                } catch (ContinueSignal&) {
                    // fall through to the post-expression, like C's `for`
                }
                if (doBreak) break;
                if (node->post) eval(node->post.get(), loopEnv);
            }
            return;
        }
        case StmtKind::Return: {
            auto* node = static_cast<const ReturnStmt*>(stmt);
            Value value = node->value ? eval(node->value.get(), env) : Value::null();
            throw ReturnSignal{value};
        }
        case StmtKind::Break:
            throw BreakSignal{};
        case StmtKind::Continue:
            throw ContinueSignal{};
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
                throw RuntimeError(i18n::tr("Operand '", "Operands of '") + op +
                        i18n::tr("' harus dua angka atau dua teks", "' must be two numbers or two strings"));
            }

            if (left.type != ValueType::Number || right.type != ValueType::Number) {
                throw RuntimeError(i18n::tr("Operand '", "Operands of '") + op + i18n::tr("' harus angka", "' must be numbers"));
            }
            if (op == "-") return Value::fromNumber(left.number - right.number);
            if (op == "*") return Value::fromNumber(left.number * right.number);
            if (op == "/") return Value::fromNumber(left.number / right.number);
            if (op == "%") return Value::fromNumber(std::fmod(left.number, right.number));

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
                    if (method) return callFunction(method, args, node->span, &target, owner);
                    auto fit = target.instance()->fields->find(keyVal.str());
                    Value fieldVal = fit != target.instance()->fields->end() ? fit->second : Value::null();
                    return callValue(fieldVal, args, node->span);
                }
                Value callee = indexGet(target, eval(idxNode->index.get(), env));
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
        state->fields = std::make_shared<std::unordered_map<std::string, Value>>();
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
    throw RuntimeError(i18n::tr("Coba manggil nilai yang bukan fungsi", "Attempted to call a non-function value"));
}

Value Interpreter::callFunction(const std::shared_ptr<Function>& fn, std::vector<Value>& args, Span callSite,
                                 const Value* boundThis, const std::shared_ptr<ClassInfo>& methodOwner) {
    const FnDeclStmt* decl = fn->decl;
    if (args.size() != decl->params.size()) {
        throw RuntimeError(i18n::tr("Fungsi '", "Function '") + decl->name +
                            i18n::tr("' butuh ", "' expects ") + std::to_string(decl->params.size()) +
                            i18n::tr(" argumen, dapat ", " arg(s), got ") + std::to_string(args.size()));
    }
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
    } catch (ReturnSignal& r) {
        return r.value;
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
    GC::liveGoroutines--;
#else
    (void)fn; (void)args;
#endif
}

Value Interpreter::callBuiltin(const std::string& name, std::vector<Value>& args) {
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
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) os << ' ';
            os << args[i].stringify();
        }
        os << '\n';
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
                    st->nums.clear();
                    st->nums.shrink_to_fit();
                    return Value::fromNumber(static_cast<double>(st->boxed->size()));
                }
            } else {
                st->boxed->push_back(args[1]);
                return Value::fromNumber(static_cast<double>(st->boxed->size()));
            }
        }
        expectType(args[0], ValueType::Array);
        args[0].array()->push_back(args[1]);
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
        return Value::fromString(args[0].typeName());
    }

    if (name == "waktu") {
        need(0);
        auto now = std::chrono::system_clock::now();
        double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
        return Value::fromNumber(secs);
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
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
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
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
        (*m)["status"] = Value::fromNumber(resp.status);
        (*m)["tubuh"] = Value::fromString(resp.body);
        return Value::fromMap(m);
    }

    if (name == "qr_baca") {
        need(1);
        expectType(args[0], ValueType::String);
        std::vector<std::string> payloads = qr::decode(args[0].str());
        auto arr = std::make_shared<std::vector<Value>>();
        for (const auto& s : payloads) arr->push_back(Value::fromString(s));
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

                auto headerMap = std::make_shared<std::unordered_map<std::string, Value>>();
                for (const auto& [k, v] : req.headers) (*headerMap)[k] = Value::fromString(v);
                auto reqMap = std::make_shared<std::unordered_map<std::string, Value>>();
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
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
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
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
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
            auto withExp = std::make_shared<std::unordered_map<std::string, Value>>(*args[0].map());
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
                    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
                    (*m)["indeks"] = Value::fromNumber(static_cast<double>(i));
                    (*m)["nilai"] = v;
                    return Value::fromMap(m);
                }
                if (chan->closed) {
                    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
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

Value Interpreter::doImport(const std::string& rawPath) {
    std::string path;

    bool isExplicitRelativeOrAbs = (!rawPath.empty() && (rawPath[0] == '/' || rawPath.rfind("./", 0) == 0 || rawPath.rfind("../", 0) == 0));

    if (!isExplicitRelativeOrAbs) {
        std::string localPath = "nusantara_modules/" + rawPath;
        if (isRegularFile(localPath)) {
            path = localPath;
        } else if (std::ifstream(localPath + ".ns").good()) {
            path = localPath + ".ns";
        } else if (std::ifstream(localPath + "/index.ns").good()) {
            path = localPath + "/index.ns";
        } else {
            std::string globalDir = sysplugin::globalModulesDir();
            if (!globalDir.empty()) {
                std::string gPath = globalDir + "/" + rawPath;
                if (isRegularFile(gPath)) path = gPath;
                else if (std::ifstream(gPath + ".ns").good()) path = gPath + ".ns";
                else if (std::ifstream(gPath + "/index.ns").good()) path = gPath + "/index.ns";
            }
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

    auto cached = moduleCache_.find(path);
    if (cached != moduleCache_.end()) return cached->second;

    for (const std::string& active : importStack_) {
        if (active == path)
            throw RuntimeError(i18n::tr("impor melingkar (circular import) terdeteksi: '",
                                          "circular import detected: '") + path + "'");
    }

    std::ifstream file(path);
    if (!file)
        throw RuntimeError(i18n::tr("impor(): nggak bisa buka '", "impor(): can't open '") + path +
                            i18n::tr("' (dari '", "' (from '") + rawPath + "')");
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

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

    importStack_.push_back(path);
    importDirStack_.push_back(dirName(path));
    for (const auto& stmt : program->statements) {
        if (exprDepth_ == 0) GC::instance().collectIfNeeded();
        exec(stmt.get(), modEnv);
    }
    importDirStack_.pop_back();
    importStack_.pop_back();

    auto exported = std::make_shared<std::unordered_map<std::string, Value>>(modEnv->vars());
    Value result = Value::fromMap(std::move(exported));

    // FnDeclStmt* pointers inside any exported closures point into
    // `program`'s AST -- keep it alive for the rest of the process,
    // same reasoning as the REPL's `history` vector in main.cpp.
    importedPrograms_.push_back(std::move(program));
    moduleCache_[path] = result;
    return result;
}
