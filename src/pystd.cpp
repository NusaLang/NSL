#include "pystd.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <sys/wait.h>
#include <functional>
#include <mutex>
#include <random>
#include <regex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "json.hpp"

extern char** environ;

namespace pystd {
namespace {

using pylib::PyError;

[[noreturn]] void fail(const std::string& m) { throw PyError(m); }

bool isSeq(const Value& v) { return v.type == ValueType::Array || v.type == ValueType::VmArray; }

std::vector<Value> elems(const Value& v) {
    if (v.type == ValueType::Array) return *v.array();
    if (v.type == ValueType::VmArray) {
        auto* st = v.vmArray();
        if (!st->numeric) return *st->boxed;
        std::vector<Value> out;
        for (double d : st->nums) out.push_back(Value::fromNumber(d));
        return out;
    }
    fail("butuh larik");
}

Value mkArr(std::vector<Value> v) { return Value::fromArray(std::make_shared<std::vector<Value>>(std::move(v))); }

double num(const std::vector<Value>& a, size_t i, const char* who) {
    if (i >= a.size() || (a[i].type != ValueType::Number && a[i].type != ValueType::Bool)) fail(std::string(who) + "(): butuh angka");
    return a[i].number;
}

const std::string& str(const std::vector<Value>& a, size_t i, const char* who) {
    if (i >= a.size() || a[i].type != ValueType::String) fail(std::string(who) + "(): butuh teks");
    return a[i].str();
}

std::vector<std::string> g_argv;
std::mt19937_64& rng() {
    static std::mt19937_64 gen{std::random_device{}()};
    return gen;
}

// ------------------------------------------------------------------ math
using Fn1 = double (*)(double);
const std::unordered_map<std::string, Fn1>& mathUnary() {
    static const std::unordered_map<std::string, Fn1> t = {
        {"sqrt", [](double x) { if (x < 0) fail("math domain error"); return std::sqrt(x); }},
        {"sin", [](double x) { return std::sin(x); }}, {"cos", [](double x) { return std::cos(x); }},
        {"tan", [](double x) { return std::tan(x); }}, {"asin", [](double x) { return std::asin(x); }},
        {"acos", [](double x) { return std::acos(x); }}, {"atan", [](double x) { return std::atan(x); }},
        {"sinh", [](double x) { return std::sinh(x); }}, {"cosh", [](double x) { return std::cosh(x); }},
        {"tanh", [](double x) { return std::tanh(x); }}, {"exp", [](double x) { return std::exp(x); }},
        {"log2", [](double x) { if (x <= 0) fail("math domain error"); return std::log2(x); }},
        {"log10", [](double x) { if (x <= 0) fail("math domain error"); return std::log10(x); }},
        {"floor", [](double x) { return std::floor(x); }}, {"ceil", [](double x) { return std::ceil(x); }},
        {"trunc", [](double x) { return std::trunc(x); }}, {"fabs", [](double x) { return std::fabs(x); }},
        {"degrees", [](double x) { return x * 180.0 / M_PI; }}, {"radians", [](double x) { return x * M_PI / 180.0; }},
        {"erf", [](double x) { return std::erf(x); }}, {"gamma", [](double x) { return std::tgamma(x); }},
        {"cbrt", [](double x) { return std::cbrt(x); }},
    };
    return t;
}

const std::vector<std::string>& names() {
    static const std::vector<std::string> n = [] {
        std::vector<std::string> v = {"_math_atan2", "_math_hypot", "_math_fmod", "_math_copysign", "_math_log",
            "_math_gcd", "_math_factorial", "_math_isnan", "_math_isinf", "_math_isfinite", "_math_isclose",
            "_json_dumps", "_os_getcwd", "_os_chdir", "_os_listdir", "_os_mkdir", "_os_makedirs", "_os_remove",
            "_os_rmdir", "_os_rename", "_os_system", "_os_getpid", "_os_getenv", "_os_environ", "_os_exists",
            "_os_isfile", "_os_isdir", "_os_getsize", "_os_abspath", "_os_cpu_count", "_sys_argv", "_sys_exit",
            "_sys_write", "_sys_platform", "_time_monotonic", "_time_strftime", "_time_parts", "_time_mktime",
            "_random_random", "_random_randint", "_random_uniform", "_random_choice", "_random_shuffle",
            "_random_sample", "_random_seed", "_random_gauss", "_re_exec", "_re_sub", "_re_split", "_re_findall",
            "_stdin_read", "_readline", "_file_append", "_os_mtime", "_os_copyfile", "_os_rmtree", "_os_join", "_os_basename", "_os_dirname", "_os_splitext", "_os_expanduser"};
        for (const auto& kv : mathUnary()) v.push_back("_math_" + kv.first);
        return v;
    }();
    return n;
}

// ------------------------------------------------------------------ json.dumps
void dumpString(const std::string& s, std::string& out, bool ascii) {
    out += '"';
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else if (c >= 0x80 && ascii) {
                    // decode one UTF-8 code point -> \uXXXX (surrogate pair above the BMP)
                    unsigned cp = 0; int n = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
                    cp = c & (0xFF >> (n + 1));
                    for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
                    i += static_cast<size_t>(n - 1);
                    char b[16];
                    if (cp >= 0x10000) { cp -= 0x10000; std::snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); }
                    else std::snprintf(b, sizeof b, "\\u%04x", cp);
                    out += b;
                } else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void dumpValue(const Value& v, std::string& out, int indent, int level, bool sortKeys, bool ascii) {
    auto nl = [&](int lv) { if (indent >= 0) { out += '\n'; out.append(static_cast<size_t>(indent * lv), ' '); } };
    switch (v.type) {
        case ValueType::Null: out += "null"; return;
        case ValueType::Bool: out += v.boolean() ? "true" : "false"; return;
        case ValueType::Number: out += Value::formatNumber(v.number); return;
        case ValueType::String: dumpString(v.str(), out, ascii); return;
        case ValueType::Array:
        case ValueType::VmArray: {
            std::vector<Value> items = elems(v);
            if (items.empty()) { out += "[]"; return; }
            out += '[';
            for (size_t i = 0; i < items.size(); i++) {
                if (i) out += indent >= 0 ? "," : ", ";
                nl(level + 1);
                dumpValue(items[i], out, indent, level + 1, sortKeys, ascii);
            }
            nl(level);
            out += ']';
            return;
        }
        case ValueType::Map:
        case ValueType::Instance: {
            const ValueMap& m = v.type == ValueType::Map ? *v.map() : *v.instance()->fields;
            std::vector<const std::pair<std::string, Value>*> entries;
            for (const auto& e : m) entries.push_back(&e);
            if (sortKeys) std::sort(entries.begin(), entries.end(), [](auto* a, auto* b) { return a->first < b->first; });
            if (entries.empty()) { out += "{}"; return; }
            out += '{';
            for (size_t i = 0; i < entries.size(); i++) {
                if (i) out += indent >= 0 ? "," : ", ";
                nl(level + 1);
                dumpString(entries[i]->first, out, ascii);
                out += ": ";
                dumpValue(entries[i]->second, out, indent, level + 1, sortKeys, ascii);
            }
            nl(level);
            out += '}';
            return;
        }
        default: fail("json.dumps(): tipe tidak bisa diubah ke JSON");
    }
}

// ------------------------------------------------------------------ re
struct Compiled {
    std::regex re;
    std::vector<std::string> groupNames;  // index -> name ("" for unnamed)
};

Compiled compilePattern(const std::string& pat, double flags) {
    std::string out;
    std::vector<std::string> gnames{""};
    for (size_t i = 0; i < pat.size(); i++) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pat.size()) {
            char n = pat[i + 1];
            if (n == 'A') { out += '^'; i++; continue; }
            if (n == 'Z') { out += '$'; i++; continue; }
            out += c; out += n; i++;
            continue;
        }
        if (c == '(') {
            if (pat.compare(i, 4, "(?P<") == 0) {
                size_t close = pat.find('>', i);
                if (close == std::string::npos) fail("re: grup bernama tidak lengkap");
                gnames.push_back(pat.substr(i + 4, close - i - 4));
                out += '(';
                i = close;
                continue;
            }
            if (i + 1 < pat.size() && pat[i + 1] == '?') { out += c; continue; }  // (?: (?= (?! stay
            gnames.push_back("");
        }
        out += c;
    }
    auto f = std::regex::ECMAScript;
    if (static_cast<int>(flags) & 2) f |= std::regex::icase;
    if (static_cast<int>(flags) & 8) f |= std::regex::multiline;
    try {
        return {std::regex(out, f), gnames};
    } catch (const std::regex_error& e) {
        fail(std::string("re: pola tidak valid: ") + e.what());
    }
}

Value matchToValue(const std::smatch& m, const std::string& subject, const Compiled& c) {
    std::vector<Value> groups;
    for (size_t g = 0; g < m.size(); g++) {
        if (m[g].matched) groups.push_back(Value::fromString(m[g].str())); else groups.push_back(Value::null());
    }
    std::vector<Value> starts, ends;
    for (size_t g = 0; g < m.size(); g++) {
        starts.push_back(Value::fromNumber(m[g].matched ? static_cast<double>(m.position(g)) : -1));
        ends.push_back(Value::fromNumber(m[g].matched ? static_cast<double>(m.position(g) + m.length(g)) : -1));
    }
    auto names = std::make_shared<ValueMap>();
    for (size_t g = 1; g < c.groupNames.size() && g < m.size(); g++) {
        if (!c.groupNames[g].empty()) (*names)[c.groupNames[g]] = Value::fromNumber(static_cast<double>(g));
    }
    (void)subject;
    return mkArr({mkArr(std::move(groups)), mkArr(std::move(starts)), mkArr(std::move(ends)), Value::fromMap(names)});
}

}  // namespace

const std::vector<std::string>& builtinNames() { return names(); }

bool handles(const std::string& name) {
    static const std::unordered_set<std::string> s(names().begin(), names().end());
    return s.count(name) > 0;
}

void setArgv(const std::vector<std::string>& argv) { g_argv = argv; }

Value call(const std::string& name, std::vector<Value>& a, const ValueMap* kw, const pylib::CallFn& callFn) {
    auto kwv = [&](const char* k) -> const Value* {
        if (!kw) return nullptr;
        auto it = kw->find(k);
        return it == kw->end() ? nullptr : &it->second;
    };
    if (name.rfind("_math_", 0) == 0) {
        std::string f = name.substr(6);
        auto it = mathUnary().find(f);
        if (it != mathUnary().end()) return Value::fromNumber(it->second(num(a, 0, f.c_str())));
        if (f == "atan2") return Value::fromNumber(std::atan2(num(a, 0, "atan2"), num(a, 1, "atan2")));
        if (f == "hypot") return Value::fromNumber(std::hypot(num(a, 0, "hypot"), num(a, 1, "hypot")));
        if (f == "fmod") return Value::fromNumber(std::fmod(num(a, 0, "fmod"), num(a, 1, "fmod")));
        if (f == "copysign") return Value::fromNumber(std::copysign(num(a, 0, "copysign"), num(a, 1, "copysign")));
        if (f == "log") {
            double x = num(a, 0, "log");
            if (x <= 0) fail("math domain error");
            return Value::fromNumber(a.size() > 1 ? std::log(x) / std::log(num(a, 1, "log")) : std::log(x));
        }
        if (f == "gcd") {
            long long x = std::llabs(static_cast<long long>(num(a, 0, "gcd"))), y = std::llabs(static_cast<long long>(num(a, 1, "gcd")));
            while (y) { long long t = x % y; x = y; y = t; }
            return Value::fromNumber(static_cast<double>(x));
        }
        if (f == "factorial") {
            double n = num(a, 0, "factorial"), r = 1;
            if (n < 0) fail("factorial() tidak untuk bilangan negatif");
            for (double i = 2; i <= n; i++) r *= i;
            return Value::fromNumber(r);
        }
        if (f == "isnan") return Value::fromBool(std::isnan(num(a, 0, "isnan")));
        if (f == "isinf") return Value::fromBool(std::isinf(num(a, 0, "isinf")));
        if (f == "isfinite") return Value::fromBool(std::isfinite(num(a, 0, "isfinite")));
        if (f == "isclose") {
            double x = num(a, 0, "isclose"), y = num(a, 1, "isclose");
            double rel = kwv("rel_tol") ? kwv("rel_tol")->number : 1e-9, abst = kwv("abs_tol") ? kwv("abs_tol")->number : 0.0;
            return Value::fromBool(std::fabs(x - y) <= std::max(rel * std::max(std::fabs(x), std::fabs(y)), abst));
        }
    }
    if (name == "_json_dumps") {
        int indent = -1;
        bool sortKeys = false, ascii = true;
        if (a.size() > 1 && a[1].type == ValueType::Number) indent = static_cast<int>(a[1].number);
        if (const Value* v = kwv("indent")) { if (v->type == ValueType::Number) indent = static_cast<int>(v->number); }
        if (const Value* v = kwv("sort_keys")) sortKeys = v->truthy();
        if (const Value* v = kwv("ensure_ascii")) ascii = v->truthy();
        std::string out;
        dumpValue(a.at(0), out, indent, 0, sortKeys, ascii);
        return Value::fromString(out);
    }
    // ---- os
    if (name == "_os_getcwd") { char b[4096]; return Value::fromString(getcwd(b, sizeof b) ? b : ""); }
    if (name == "_os_chdir") { if (chdir(str(a, 0, "chdir").c_str()) != 0) fail("chdir gagal"); return Value::null(); }
    if (name == "_os_listdir") {
        std::string dir = a.empty() ? "." : str(a, 0, "listdir");
        DIR* d = opendir(dir.c_str());
        if (!d) fail("listdir(): tidak bisa membuka '" + dir + "'");
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
        closedir(d);
        std::sort(names.begin(), names.end());
        std::vector<Value> out;
        for (auto& n : names) out.push_back(Value::fromString(n));
        return mkArr(std::move(out));
    }
    if (name == "_os_mkdir") { if (mkdir(str(a, 0, "mkdir").c_str(), 0755) != 0) fail("mkdir gagal: " + str(a, 0, "mkdir")); return Value::null(); }
    if (name == "_os_makedirs") {
        std::string path = str(a, 0, "makedirs"), cur;
        for (size_t i = 0; i <= path.size(); i++) {
            if (i == path.size() || path[i] == '/') {
                if (!cur.empty()) mkdir(cur.c_str(), 0755);
            }
            if (i < path.size()) cur += path[i];
        }
        return Value::null();
    }
    if (name == "_os_remove") { if (unlink(str(a, 0, "remove").c_str()) != 0) fail("remove gagal: " + str(a, 0, "remove")); return Value::null(); }
    if (name == "_os_rmdir") { if (rmdir(str(a, 0, "rmdir").c_str()) != 0) fail("rmdir gagal: " + str(a, 0, "rmdir")); return Value::null(); }
    if (name == "_os_rename") { if (std::rename(str(a, 0, "rename").c_str(), str(a, 1, "rename").c_str()) != 0) fail("rename gagal"); return Value::null(); }
    if (name == "_os_system") { std::fflush(stdout); int rc = std::system(str(a, 0, "system").c_str()); return Value::fromNumber(WIFEXITED(rc) ? WEXITSTATUS(rc) : rc); }
    if (name == "_os_getpid") return Value::fromNumber(getpid());
    if (name == "_os_getenv") { const char* v = std::getenv(str(a, 0, "getenv").c_str()); return v ? Value::fromString(v) : (a.size() > 1 ? a[1] : Value::null()); }
    if (name == "_os_environ") {
        auto m = std::make_shared<ValueMap>();
        for (char** e = environ; e && *e; e++) { std::string kv = *e; size_t eq = kv.find('='); if (eq != std::string::npos) (*m)[kv.substr(0, eq)] = Value::fromString(kv.substr(eq + 1)); }
        return Value::fromMap(m);
    }
    if (name == "_os_exists" || name == "_os_isfile" || name == "_os_isdir" || name == "_os_getsize") {
        struct stat st;
        bool ok = stat(str(a, 0, "path").c_str(), &st) == 0;
        if (name == "_os_getsize") { if (!ok) fail("getsize(): file tidak ada"); return Value::fromNumber(static_cast<double>(st.st_size)); }
        if (name == "_os_exists") return Value::fromBool(ok);
        if (name == "_os_isfile") return Value::fromBool(ok && S_ISREG(st.st_mode));
        return Value::fromBool(ok && S_ISDIR(st.st_mode));
    }
    if (name == "_os_abspath") {
        std::string p = str(a, 0, "abspath");
        if (!p.empty() && p[0] == '/') return Value::fromString(p);
        char b[4096];
        std::string cwd = getcwd(b, sizeof b) ? b : "";
        return Value::fromString(cwd + "/" + p);
    }
    if (name == "_file_append") {
        std::ofstream out(str(a, 0, "write"), std::ios::binary | std::ios::app);
        if (!out) fail("OSError: tidak bisa menulis '" + str(a, 0, "write") + "'");
        out << str(a, 1, "write");
        return Value::null();
    }
    if (name == "_os_mtime") {
        struct stat st;
        if (stat(str(a, 0, "getmtime").c_str(), &st) != 0) fail("getmtime(): file tidak ada");
        return Value::fromNumber(static_cast<double>(st.st_mtime));
    }
    if (name == "_os_copyfile") {
        std::ifstream in(str(a, 0, "copyfile"), std::ios::binary);
        if (!in) fail("copyfile(): tidak bisa membuka '" + str(a, 0, "copyfile") + "'");
        std::ofstream out(str(a, 1, "copyfile"), std::ios::binary | std::ios::trunc);
        if (!out) fail("copyfile(): tidak bisa menulis '" + str(a, 1, "copyfile") + "'");
        out << in.rdbuf();
        return Value::null();
    }
    if (name == "_os_rmtree") {
        std::function<void(const std::string&)> rm = [&](const std::string& path) {
            struct stat st;
            if (lstat(path.c_str(), &st) != 0) return;
            if (S_ISDIR(st.st_mode)) {
                if (DIR* d = opendir(path.c_str())) {
                    while (dirent* e = readdir(d)) {
                        std::string n = e->d_name;
                        if (n != "." && n != "..") rm(path + "/" + n);
                    }
                    closedir(d);
                }
                rmdir(path.c_str());
            } else {
                unlink(path.c_str());
            }
        };
        rm(str(a, 0, "rmtree"));
        return Value::null();
    }
    if (name == "_os_cpu_count") return Value::fromNumber(static_cast<double>(std::thread::hardware_concurrency()));
    // ---- sys
    if (name == "_sys_argv") { std::vector<Value> v; for (auto& s : g_argv) v.push_back(Value::fromString(s)); return mkArr(std::move(v)); }
    if (name == "_sys_exit") {
        std::fflush(stdout);
        std::cout.flush();
        int code = a.empty() || a[0].type == ValueType::Null ? 0 : (a[0].type == ValueType::Number ? static_cast<int>(a[0].number) : 1);
        if (!a.empty() && a[0].type == ValueType::String) std::cerr << a[0].str() << "\n";
        std::_Exit(code);
    }
    if (name == "_sys_write") {
        bool err = a.size() > 1 && a[1].truthy();
        const std::string& s = a.at(0).str();
        (err ? std::cerr : std::cout) << s;
        return Value::fromNumber(static_cast<double>(s.size()));
    }
    if (name == "_sys_platform") return Value::fromString("linux");
    // ---- time
    if (name == "_time_monotonic") return Value::fromNumber(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (name == "_time_parts" || name == "_time_strftime" || name == "_time_mktime") {
        if (name == "_time_mktime") {
            std::tm t{};
            t.tm_year = static_cast<int>(num(a, 0, "mktime")) - 1900; t.tm_mon = static_cast<int>(num(a, 1, "mktime")) - 1;
            t.tm_mday = static_cast<int>(num(a, 2, "mktime")); t.tm_hour = static_cast<int>(num(a, 3, "mktime"));
            t.tm_min = static_cast<int>(num(a, 4, "mktime")); t.tm_sec = static_cast<int>(num(a, 5, "mktime")); t.tm_isdst = -1;
            return Value::fromNumber(static_cast<double>(std::mktime(&t)));
        }
        double ts = 0;
        size_t tsIdx = name == "_time_parts" ? 0 : 1;
        if (a.size() > tsIdx && a[tsIdx].type == ValueType::Number) ts = a[tsIdx].number;
        else ts = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        bool utc = false;
        if (const Value* u = kwv("utc")) utc = u->truthy();
        std::time_t tt = static_cast<std::time_t>(std::floor(ts));
        std::tm tm{};
        if (utc) gmtime_r(&tt, &tm); else localtime_r(&tt, &tm);
        if (name == "_time_parts") {
            double frac = ts - std::floor(ts);
            return mkArr({Value::fromNumber(tm.tm_year + 1900), Value::fromNumber(tm.tm_mon + 1), Value::fromNumber(tm.tm_mday),
                          Value::fromNumber(tm.tm_hour), Value::fromNumber(tm.tm_min), Value::fromNumber(tm.tm_sec),
                          Value::fromNumber((tm.tm_wday + 6) % 7), Value::fromNumber(tm.tm_yday + 1), Value::fromNumber(std::floor(frac * 1e6))});
        }
        char buf[512];
        std::strftime(buf, sizeof buf, str(a, 0, "strftime").c_str(), &tm);
        return Value::fromString(buf);
    }
    // ---- random
    if (name == "_random_random") return Value::fromNumber(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
    if (name == "_random_uniform") { double lo = num(a, 0, "uniform"), hi = num(a, 1, "uniform"); return Value::fromNumber(lo + (hi - lo) * std::uniform_real_distribution<double>(0.0, 1.0)(rng())); }
    if (name == "_random_randint") {
        long long lo = static_cast<long long>(num(a, 0, "randint")), hi = static_cast<long long>(num(a, 1, "randint"));
        if (hi < lo) fail("randint(): rentang kosong");
        return Value::fromNumber(static_cast<double>(std::uniform_int_distribution<long long>(lo, hi)(rng())));
    }
    if (name == "_random_choice") {
        std::vector<Value> v = a.at(0).type == ValueType::String ? std::vector<Value>{} : elems(a.at(0));
        if (a.at(0).type == ValueType::String) { for (char c : a[0].str()) v.push_back(Value::fromString(std::string(1, c))); }
        if (v.empty()) fail("choice(): urutan kosong");
        return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng())];
    }
    if (name == "_random_shuffle") {
        Value list = a.at(0);
        std::vector<Value> v = elems(list);
        std::shuffle(v.begin(), v.end(), rng());
        if (list.type == ValueType::Array) *list.array() = v;
        else { auto* st = list.vmArray(); st->numeric = false; st->nums.clear(); st->boxed = std::make_shared<std::vector<Value>>(v); }
        return Value::null();
    }
    if (name == "_random_sample") {
        std::vector<Value> v = elems(a.at(0));
        size_t k = static_cast<size_t>(num(a, 1, "sample"));
        if (k > v.size()) fail("sample(): sampel lebih besar dari populasi");
        std::shuffle(v.begin(), v.end(), rng());
        v.resize(k);
        return mkArr(std::move(v));
    }
    if (name == "_random_seed") { rng().seed(static_cast<unsigned long long>(a.empty() ? 0 : num(a, 0, "seed"))); return Value::null(); }
    if (name == "_random_gauss") return Value::fromNumber(std::normal_distribution<double>(num(a, 0, "gauss"), num(a, 1, "gauss"))(rng()));
    // ---- re: _re_exec(pattern, string, flags, mode, pos) -> match tuple or None
    if (name == "_re_exec") {
        Compiled c = compilePattern(str(a, 0, "re"), a.size() > 2 ? num(a, 2, "re") : 0);
        const std::string& subj = str(a, 1, "re");
        std::string mode = a.size() > 3 ? a[3].str() : "search";
        size_t from = a.size() > 4 ? static_cast<size_t>(num(a, 4, "re")) : 0;
        std::smatch m;
        auto begin = subj.cbegin() + static_cast<long>(std::min(from, subj.size()));
        auto flags = mode == "match" || mode == "fullmatch" ? std::regex_constants::match_continuous : std::regex_constants::match_default;
        bool ok = std::regex_search(begin, subj.cend(), m, c.re, flags);
        if (ok && mode == "fullmatch" && (m.position(0) != 0 || static_cast<size_t>(m.length(0)) != subj.size() - std::min(from, subj.size()))) ok = false;
        if (!ok) return Value::null();
        // positions are relative to `begin`; shift by `from`
        Value res = matchToValue(m, subj, c);
        if (from) {
            auto shift = [&](Value& arr) { std::vector<Value> v = elems(arr); for (auto& x : v) if (x.number >= 0) x.number += static_cast<double>(from); arr = mkArr(std::move(v)); };
            std::vector<Value> parts = elems(res);
            shift(parts[1]);
            shift(parts[2]);
            res = mkArr(std::move(parts));
        }
        return res;
    }
    if (name == "_re_findall") {  // -> list of match tuples (all non-overlapping)
        Compiled c = compilePattern(str(a, 0, "re"), a.size() > 2 ? num(a, 2, "re") : 0);
        const std::string& subj = str(a, 1, "re");
        std::vector<Value> out;
        for (auto it = std::sregex_iterator(subj.begin(), subj.end(), c.re); it != std::sregex_iterator(); ++it) {
            Value m = matchToValue(*it, subj, c);
            std::vector<Value> parts = elems(m);
            // absolute positions
            (void)parts;
            out.push_back(m);
        }
        return mkArr(std::move(out));
    }
    if (name == "_re_sub") {  // (pattern, repl, string, count, flags)
        Compiled c = compilePattern(str(a, 0, "re.sub"), a.size() > 4 ? num(a, 4, "re.sub") : 0);
        const std::string& subj = str(a, 2, "re.sub");
        long long count = a.size() > 3 && a[3].type == ValueType::Number ? static_cast<long long>(a[3].number) : 0;
        bool fn = a.at(1).type != ValueType::String;
        std::string out;
        size_t last = 0;
        long long done = 0;
        for (auto it = std::sregex_iterator(subj.begin(), subj.end(), c.re); it != std::sregex_iterator(); ++it) {
            if (count > 0 && done >= count) break;
            const std::smatch& m = *it;
            out.append(subj, last, static_cast<size_t>(m.position(0)) - last);
            if (fn) {
                std::vector<Value> ma{matchToValue(m, subj, c)};
                Value r = callFn(a[1], ma);
                out += r.str();
            } else {
                const std::string& repl = a[1].str();
                for (size_t i = 0; i < repl.size(); i++) {
                    if (repl[i] == '\\' && i + 1 < repl.size()) {
                        char n = repl[i + 1];
                        if (std::isdigit(static_cast<unsigned char>(n))) { size_t g = static_cast<size_t>(n - '0'); if (g < m.size()) out += m[g].str(); i++; continue; }
                        if (n == 'n') { out += '\n'; i++; continue; }
                        if (n == '\\') { out += '\\'; i++; continue; }
                    }
                    out += repl[i];
                }
            }
            last = static_cast<size_t>(m.position(0) + m.length(0));
            done++;
        }
        out.append(subj, last, std::string::npos);
        return Value::fromString(out);
    }
    if (name == "_re_split") {
        Compiled c = compilePattern(str(a, 0, "re.split"), a.size() > 3 ? num(a, 3, "re.split") : 0);
        const std::string& subj = str(a, 1, "re.split");
        long long maxsplit = a.size() > 2 && a[2].type == ValueType::Number ? static_cast<long long>(a[2].number) : 0;
        std::vector<Value> out;
        size_t last = 0;
        long long done = 0;
        for (auto it = std::sregex_iterator(subj.begin(), subj.end(), c.re); it != std::sregex_iterator(); ++it) {
            if (maxsplit > 0 && done >= maxsplit) break;
            const std::smatch& m = *it;
            if (m.length(0) == 0) continue;
            out.push_back(Value::fromString(subj.substr(last, static_cast<size_t>(m.position(0)) - last)));
            for (size_t g = 1; g < m.size(); g++) out.push_back(m[g].matched ? Value::fromString(m[g].str()) : Value::null());
            last = static_cast<size_t>(m.position(0) + m.length(0));
            done++;
        }
        out.push_back(Value::fromString(subj.substr(last)));
        return mkArr(std::move(out));
    }
    if (name == "_os_join") {
        std::string out;
        for (const Value& v : a) {
            if (v.type != ValueType::String) fail("path.join(): semua bagian harus teks");
            const std::string& p = v.str();
            if (!p.empty() && p[0] == '/') out = p;
            else if (out.empty() || out.back() == '/') out += p;
            else out += "/" + p;
        }
        return Value::fromString(out);
    }
    if (name == "_os_basename") { std::string p = str(a, 0, "basename"); size_t s = p.find_last_of('/'); return Value::fromString(s == std::string::npos ? p : p.substr(s + 1)); }
    if (name == "_os_dirname") { std::string p = str(a, 0, "dirname"); size_t s = p.find_last_of('/'); return Value::fromString(s == std::string::npos ? "" : (s == 0 ? "/" : p.substr(0, s))); }
    if (name == "_os_splitext") {
        std::string p = str(a, 0, "splitext");
        size_t slash = p.find_last_of('/'), dot = p.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == 0 || (slash != std::string::npos && dot == slash + 1))
            return mkArr({Value::fromString(p), Value::fromString("")});
        return mkArr({Value::fromString(p.substr(0, dot)), Value::fromString(p.substr(dot))});
    }
    if (name == "_os_expanduser") {
        std::string p = str(a, 0, "expanduser");
        if (!p.empty() && p[0] == '~') { const char* h = std::getenv("HOME"); if (h) return Value::fromString(std::string(h) + p.substr(1)); }
        return Value::fromString(p);
    }
    if (name == "_readline") {
        std::string line;
        if (!std::getline(std::cin, line)) return Value::null();
        return Value::fromString(line);
    }
    if (name == "_stdin_read") {
        std::string all, line;
        char buf[4096];
        while (std::cin.read(buf, sizeof buf) || std::cin.gcount() > 0) all.append(buf, static_cast<size_t>(std::cin.gcount()));
        return Value::fromString(all);
    }
    fail("builtin tidak dikenal: " + name);
}

}  // namespace pystd
