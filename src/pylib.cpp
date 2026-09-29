#include "pylib.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "value_eq.hpp"

namespace pylib {
namespace {

[[noreturn]] void fail(const std::string& m) { throw PyError(m); }

bool isSeq(const Value& v) { return v.type == ValueType::Array || v.type == ValueType::VmArray; }

std::vector<Value> utf8Chars(const std::string& s) {
    std::vector<Value> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) n = 1;
        out.push_back(Value::fromString(s.substr(i, n)));
        i += n;
    }
    return out;
}

// Elements of any iterable: list, str (characters), dict (keys).
std::vector<Value> elems(const Value& v, const char* who = "argumen") {
    if (v.type == ValueType::Array) return *v.array();
    if (v.type == ValueType::VmArray) {
        auto* st = v.vmArray();
        if (!st->numeric) return *st->boxed;
        std::vector<Value> out;
        out.reserve(st->nums.size());
        for (double d : st->nums) out.push_back(Value::fromNumber(d));
        return out;
    }
    if (v.type == ValueType::String) return utf8Chars(v.str());
    if (v.type == ValueType::Map) {
        std::vector<Value> out;
        for (const auto& kv : *v.map()) out.push_back(Value::fromString(kv.first));
        return out;
    }
    fail(std::string(who) + " harus bisa diiterasi (larik, teks, atau peta)");
}

// The list's own storage; a numeric bytecode array is boxed first so any value can be stored.
std::vector<Value>& mutElems(const Value& v) {
    if (v.type == ValueType::Array) return *v.array();
    if (v.type == ValueType::VmArray) {
        auto* st = v.vmArray();
        if (st->numeric) {
            st->boxed = std::make_shared<std::vector<Value>>();
            st->boxed->reserve(st->nums.size());
            for (double d : st->nums) st->boxed->push_back(Value::fromNumber(d));
            st->numeric = false;
            st->nums.clear();
        }
        return *st->boxed;
    }
    fail("bukan larik");
}

Value mkArr(std::vector<Value> v) { return Value::fromArray(std::make_shared<std::vector<Value>>(std::move(v))); }

double numArg(const Value& v, const char* who) {
    if (v.type == ValueType::Number) return v.number;
    if (v.type == ValueType::Bool) return v.number;
    fail(std::string(who) + "(): butuh angka");
}

int cmpValues(const Value& a, const Value& b) {
    auto isNum = [](const Value& v) { return v.type == ValueType::Number || v.type == ValueType::Bool; };
    if (isNum(a) && isNum(b)) return a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
    if (a.type == ValueType::String && b.type == ValueType::String) {
        int c = a.str().compare(b.str());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (isSeq(a) && isSeq(b)) {
        std::vector<Value> x = elems(a), y = elems(b);
        for (size_t i = 0; i < x.size() && i < y.size(); i++) {
            int c = cmpValues(x[i], y[i]);
            if (c) return c;
        }
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
    }
    fail(std::string("tidak bisa membandingkan ") + a.typeName() + " dengan " + b.typeName());
}

const Value* kwGet(const ValueMap* kw, const char* name) {
    if (!kw) return nullptr;
    auto it = kw->find(name);
    return it == kw->end() ? nullptr : &it->second;
}

void noKw(const ValueMap* kw, const char* who) {
    if (!kw) return;
    for (const auto& e : *kw) {
        if (e.first != "__kw__") fail(std::string(who) + "(): argumen bernama '" + e.first + "' tidak dikenal");
    }
}

void needArgs(const std::vector<Value>& a, size_t lo, size_t hi, const char* who) {
    if (a.size() < lo || a.size() > hi) {
        fail(std::string(who) + "() butuh " + (lo == hi ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi)) +
             " argumen, dapat " + std::to_string(a.size()));
    }
}

std::string trimChars(const std::string& s, const std::string& chars, bool left, bool right) {
    size_t b = 0, e = s.size();
    auto in = [&](char c) { return chars.empty() ? std::isspace(static_cast<unsigned char>(c)) != 0 : chars.find(c) != std::string::npos; };
    if (left) while (b < e && in(s[b])) b++;
    if (right) while (e > b && in(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------- formatting

std::string groupThousands(std::string digits) {
    std::string out;
    int n = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++n % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out;
}

std::string toBase(unsigned long long n, int base, bool upper) {
    if (n == 0) return "0";
    std::string s;
    const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (n) { s.insert(s.begin(), digs[n % static_cast<unsigned>(base)]); n /= static_cast<unsigned>(base); }
    return s;
}

}  // namespace

std::string formatValue(const Value& v, const std::string& spec) {
    if (spec.empty()) return v.stringify();
    size_t i = 0;
    char fill = ' ', align = 0, sign = '-', type = 0;
    bool alt = false, zero = false, comma = false;
    int width = 0, prec = -1;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^' || c == '='; };
    if (spec.size() >= 2 && isAlign(spec[1])) { fill = spec[0]; align = spec[1]; i = 2; }
    else if (!spec.empty() && isAlign(spec[0])) { align = spec[0]; i = 1; }
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) sign = spec[i++];
    if (i < spec.size() && spec[i] == '#') { alt = true; i++; }
    if (i < spec.size() && spec[i] == '0') { zero = true; i++; }
    while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) width = width * 10 + (spec[i++] - '0');
    if (i < spec.size() && (spec[i] == ',' || spec[i] == '_')) { comma = true; i++; }
    if (i < spec.size() && spec[i] == '.') {
        i++;
        prec = 0;
        while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) prec = prec * 10 + (spec[i++] - '0');
    }
    if (i < spec.size()) type = spec[i++];
    if (i != spec.size()) fail("format spec tidak valid: '" + spec + "'");

    std::string body;
    bool numeric = v.type == ValueType::Number || v.type == ValueType::Bool;
    if (type == 's' || (!type && !numeric)) {
        body = v.stringify();
        if (prec >= 0 && static_cast<size_t>(prec) < body.size()) body.resize(static_cast<size_t>(prec));
        if (!align) align = '<';
    } else {
        if (!numeric) fail(std::string("format '") + type + "' butuh angka");
        double x = v.number;
        bool neg = std::signbit(x) && x != 0;
        double ax = std::fabs(x);
        char buf[512];
        switch (type) {
            case 'd': case 'n': {
                body = std::to_string(static_cast<long long>(std::llround(ax)));
                break;
            }
            case 'f': case 'F':
                std::snprintf(buf, sizeof buf, "%.*f", prec < 0 ? 6 : prec, ax);
                body = buf;
                break;
            case 'e': case 'E':
                std::snprintf(buf, sizeof buf, type == 'e' ? "%.*e" : "%.*E", prec < 0 ? 6 : prec, ax);
                body = buf;
                break;
            case 'g': case 'G':
                std::snprintf(buf, sizeof buf, type == 'g' ? "%.*g" : "%.*G", prec < 0 ? 6 : prec, ax);
                body = buf;
                break;
            case '%':
                std::snprintf(buf, sizeof buf, "%.*f", prec < 0 ? 6 : prec, ax * 100.0);
                body = std::string(buf) + "%";
                break;
            case 'x': body = (alt ? "0x" : "") + toBase(static_cast<unsigned long long>(ax), 16, false); break;
            case 'X': body = (alt ? "0X" : "") + toBase(static_cast<unsigned long long>(ax), 16, true); break;
            case 'o': body = (alt ? "0o" : "") + toBase(static_cast<unsigned long long>(ax), 8, false); break;
            case 'b': body = (alt ? "0b" : "") + toBase(static_cast<unsigned long long>(ax), 2, false); break;
            case 'c': body = std::string(1, static_cast<char>(static_cast<int>(x))); neg = false; break;
            case 0:
                if (prec >= 0) { std::snprintf(buf, sizeof buf, "%.*g", prec, ax); body = buf; }
                else body = Value::formatNumber(ax);
                break;
            default: fail(std::string("tipe format tidak dikenal: '") + type + "'");
        }
        if (comma) {
            size_t dot = body.find('.');
            std::string ip = dot == std::string::npos ? body : body.substr(0, dot);
            body = groupThousands(ip) + (dot == std::string::npos ? "" : body.substr(dot));
        }
        std::string sgn = neg ? "-" : (sign == '+' ? "+" : (sign == ' ' ? " " : ""));
        if (zero && !align) { align = '='; fill = '0'; }
        if (!align) align = '>';
        if (align == '=') {
            int pad = width - static_cast<int>(sgn.size() + body.size());
            if (pad > 0) body.insert(0, static_cast<size_t>(pad), fill);
            return sgn + body;
        }
        body = sgn + body;
    }
    int len = static_cast<int>(body.size());
    if (width > len) {
        int pad = width - len;
        if (align == '<') body.append(static_cast<size_t>(pad), fill);
        else if (align == '^') { body.insert(0, static_cast<size_t>(pad / 2), fill); body.append(static_cast<size_t>(pad - pad / 2), fill); }
        else body.insert(0, static_cast<size_t>(pad), fill);
    }
    return body;
}

namespace {

std::string percentFormat(const std::string& fmt, const Value& argv) {
    std::vector<Value> args;
    bool tuple = isSeq(argv);
    if (tuple) args = elems(argv); else args.push_back(argv);
    size_t next = 0;
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        if (++i >= fmt.size()) fail("format % tidak lengkap");
        if (fmt[i] == '%') { out += '%'; continue; }
        std::string flags;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) flags += fmt[i++];
        int width = 0, prec = -1;
        while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
        if (i < fmt.size() && fmt[i] == '.') {
            i++;
            prec = 0;
            while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) prec = prec * 10 + (fmt[i++] - '0');
        }
        if (i >= fmt.size()) fail("format % tidak lengkap");
        char t = fmt[i];
        if (next >= args.size()) fail("argumen kurang untuk format %");
        const Value& a = args[next++];
        std::string spec;
        bool left = flags.find('-') != std::string::npos;
        if (flags.find('+') != std::string::npos) spec += '+';
        if (flags.find('0') != std::string::npos && !left) spec += '0';
        if (width) spec = (left ? "<" : "") + spec + std::to_string(width);
        if (prec >= 0) spec += "." + std::to_string(prec);
        switch (t) {
            case 's': case 'r': out += formatValue(Value::fromString(a.stringify()), (left ? "<" : ">") + (width ? std::to_string(width) : std::string()) + (prec >= 0 ? "." + std::to_string(prec) : "") + "s"); break;
            case 'd': case 'i': case 'u': out += formatValue(Value::fromNumber(std::trunc(numArg(a, "%d"))), spec + "d"); break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': out += formatValue(a, spec + t); break;
            case 'x': case 'X': case 'o': out += formatValue(Value::fromNumber(std::trunc(numArg(a, "%x"))), spec + t); break;
            case 'c': out += a.type == ValueType::String ? a.str() : std::string(1, static_cast<char>(static_cast<int>(numArg(a, "%c")))); break;
            default: fail(std::string("format % tidak dikenal: %") + t);
        }
    }
    if (next < args.size() && tuple) fail("argumen kelebihan untuk format %");
    return out;
}

std::string strFormat(const std::string& fmt, const std::vector<Value>& args, size_t first, const ValueMap* kw) {
    std::string out;
    size_t auto_idx = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') { out += '{'; i++; continue; }
            size_t close = fmt.find('}', i);
            if (close == std::string::npos) fail("format: '{' tanpa '}'");
            std::string field = fmt.substr(i + 1, close - i - 1);
            i = close;
            std::string spec;
            size_t colon = field.find(':');
            if (colon != std::string::npos) { spec = field.substr(colon + 1); field.resize(colon); }
            size_t bang = field.find('!');
            if (bang != std::string::npos) field.resize(bang);
            const Value* val = nullptr;
            if (field.empty()) {
                if (first + auto_idx >= args.size()) fail("format: argumen kurang");
                val = &args[first + auto_idx++];
            } else if (std::isdigit(static_cast<unsigned char>(field[0]))) {
                size_t idx = static_cast<size_t>(std::atoi(field.c_str()));
                if (first + idx >= args.size()) fail("format: argumen kurang");
                val = &args[first + idx];
            } else {
                val = kwGet(kw, field.c_str());
                if (!val) fail("format: tidak ada argumen bernama '" + field + "'");
            }
            out += formatValue(*val, spec);
        } else if (c == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') i++;
            out += '}';
        } else {
            out += c;
        }
    }
    return out;
}

// ------------------------------------------------------------- name tables

const char* const kStrMethods[] = {"strip", "lstrip", "rstrip", "replace", "startswith", "endswith", "find", "rfind",
    "index", "count", "split", "rsplit", "format", "title", "capitalize", "isdigit", "isalpha", "isalnum", "isspace",
    "isupper", "islower", "splitlines", "zfill", "center", "ljust", "rjust", "partition", "swapcase", "casefold",
    "removeprefix", "removesuffix", "upper", "lower", "join", "encode"};
const char* const kListMethods[] = {"extend", "insert", "remove", "pop", "index", "count", "sort", "reverse", "copy",
    "clear", "append", "add", "discard", "union", "intersection", "difference", "symmetric_difference", "issubset",
    "issuperset", "isdisjoint", "update"};
const char* const kDictMethods[] = {"values", "items", "get", "setdefault", "pop", "update", "clear", "copy", "keys"};

const std::vector<std::string>& allNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n = {"abs", "min", "max", "sum", "round", "pow", "divmod", "chr", "ord", "hex", "bin",
            "oct", "bool", "list", "tuple", "set", "dict", "sorted", "reversed", "enumerate", "zip", "map", "filter",
            "any", "all", "isinstance", "callable", "int", "_floordiv", "_pow", "_percent", "_fmt", "_delitem",
            "_assert", "frozenset", "_bitor", "_bitand", "_bitxor", "_bitnot", "_shl", "_shr", "_setdiff", "_concat", "_kwmerge"};
        for (const char* m : kStrMethods) n.push_back(std::string("_m_s_") + m);
        for (const char* m : kListMethods) n.push_back(std::string("_m_a_") + m);
        for (const char* m : kDictMethods) n.push_back(std::string("_m_d_") + m);
        return n;
    }();
    return names;
}

}  // namespace

const std::vector<std::string>& builtinNames() { return allNames(); }

bool handles(const std::string& name) {
    static const std::unordered_set<std::string> set(allNames().begin(), allNames().end());
    return set.count(name) > 0;
}

const char* methodBuiltin(const Value& target, const std::string& name) {
    static std::unordered_map<std::string, std::string> cache;
    auto lookup = [&](const char* prefix, const char* const* list, size_t n) -> const char* {
        for (size_t i = 0; i < n; i++) {
            if (name == list[i]) {
                std::string full = std::string(prefix) + name;
                return cache.emplace(full, full).first->second.c_str();
            }
        }
        return nullptr;
    };
    switch (target.type) {
        case ValueType::String: return lookup("_m_s_", kStrMethods, sizeof kStrMethods / sizeof *kStrMethods);
        case ValueType::Array:
        case ValueType::VmArray: return lookup("_m_a_", kListMethods, sizeof kListMethods / sizeof *kListMethods);
        case ValueType::Map:
            if (target.map()->count(name)) return nullptr;  // an entry of the same name wins (module-like maps)
            return lookup("_m_d_", kDictMethods, sizeof kDictMethods / sizeof *kDictMethods);
        default: return nullptr;
    }
}

namespace {


std::string kindOf(const Value& t) {
    if (t.type != ValueType::Builtin) return "";
    const std::string& n = t.builtinName();
    if (n == "int" ) return "int";
    if (n == "ke_angka") return "float";
    if (n == "ke_teks" || n == "teks") return "str";
    if (n == "list" || n == "tuple" || n == "set" || n == "frozenset") return "list";
    if (n == "dict") return "dict";
    if (n == "bool") return "bool";
    return "";
}

bool matchesType(const Value& v, const Value& t) {
    if (isSeq(t)) {
        for (const Value& x : elems(t)) if (matchesType(v, x)) return true;
        return false;
    }
    std::string k = kindOf(t);
    if (k == "int") return v.type == ValueType::Number && v.number == std::floor(v.number);
    if (k == "float") return v.type == ValueType::Number;
    if (k == "str") return v.type == ValueType::String;
    if (k == "list") return isSeq(v);
    if (k == "dict") return v.type == ValueType::Map;
    if (k == "bool") return v.type == ValueType::Bool;
    if (t.type == ValueType::Class && v.type == ValueType::Instance) {
        for (ClassInfo* c = v.instance()->classInfo.get(); c; c = c->parent.get()) if (c == t.klass()) return true;
    }
    return false;
}

double toInt(const Value& v, int base) {
    if (v.type == ValueType::Number || v.type == ValueType::Bool) return std::trunc(v.number);
    if (v.type == ValueType::String) {
        std::string s = trimChars(v.str(), "", true, true);
        if (s.empty()) fail("int(): teks kosong");
        char* end = nullptr;
        errno = 0;
        long long n = std::strtoll(s.c_str(), &end, base);
        if (*end != '\0' || errno) fail("int(): tidak bisa mengubah '" + v.str() + "' ke bilangan bulat");
        return static_cast<double>(n);
    }
    fail("int(): tipe tidak didukung");
}

void sortValues(std::vector<Value>& items, const Value* key, bool reverse, const CallFn& callFn) {
    std::vector<std::pair<Value, Value>> keyed;  // (key, item)
    keyed.reserve(items.size());
    for (Value& it : items) {
        if (key && key->type != ValueType::Null) {
            std::vector<Value> a{it};
            keyed.emplace_back(callFn(*key, a), it);
        } else {
            keyed.emplace_back(it, it);
        }
    }
    std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& x, const auto& y) {
        return reverse ? cmpValues(y.first, x.first) < 0 : cmpValues(x.first, y.first) < 0;
    });
    for (size_t i = 0; i < items.size(); i++) items[i] = keyed[i].second;
}

Value strMethod(const std::string& m, std::vector<Value>& a, const ValueMap* kw) {
    const std::string& s = a[0].str();
    auto argStr = [&](size_t i, const char* who) -> const std::string& {
        if (i >= a.size() || a[i].type != ValueType::String) fail(std::string(who) + "(): argumen harus teks");
        return a[i].str();
    };
    if (m == "upper") { std::string r = s; for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return Value::fromString(r); }
    if (m == "lower" || m == "casefold") { std::string r = s; for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return Value::fromString(r); }
    if (m == "swapcase") { std::string r = s; for (char& c : r) c = std::isupper(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(c)) : static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return Value::fromString(r); }
    if (m == "strip" || m == "lstrip" || m == "rstrip") {
        std::string chars = a.size() > 1 && a[1].type == ValueType::String ? a[1].str() : "";
        return Value::fromString(trimChars(s, chars, m != "rstrip", m != "lstrip"));
    }
    if (m == "replace") {
        needArgs(a, 3, 4, "replace");
        const std::string& from = argStr(1, "replace");
        const std::string& to = argStr(2, "replace");
        long long limit = a.size() > 3 ? static_cast<long long>(numArg(a[3], "replace")) : -1;
        if (from.empty()) return Value::fromString(s);
        std::string r;
        size_t pos = 0;
        long long done = 0;
        while (limit < 0 || done < limit) {
            size_t f = s.find(from, pos);
            if (f == std::string::npos) break;
            r.append(s, pos, f - pos);
            r += to;
            pos = f + from.size();
            done++;
        }
        r.append(s, pos, std::string::npos);
        return Value::fromString(r);
    }
    if (m == "startswith" || m == "endswith") {
        auto test = [&](const std::string& p) {
            return m == "startswith" ? s.compare(0, p.size(), p) == 0 && s.size() >= p.size()
                                     : s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
        };
        if (a.size() > 1 && isSeq(a[1])) { for (const Value& p : elems(a[1])) if (test(p.str())) return Value::fromBool(true); return Value::fromBool(false); }
        return Value::fromBool(test(argStr(1, m.c_str())));
    }
    if (m == "find" || m == "rfind" || m == "index") {
        const std::string& sub = argStr(1, m.c_str());
        size_t p = m == "rfind" ? s.rfind(sub) : s.find(sub);
        if (p == std::string::npos) {
            if (m == "index") fail("index(): tidak ditemukan");
            return Value::fromNumber(-1);
        }
        return Value::fromNumber(static_cast<double>(p));
    }
    if (m == "count") {
        const std::string& sub = argStr(1, "count");
        if (sub.empty()) return Value::fromNumber(static_cast<double>(s.size() + 1));
        double n = 0;
        for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) n++;
        return Value::fromNumber(n);
    }
    if (m == "split" || m == "rsplit") {
        long long maxsplit = -1;
        std::string sep;
        bool ws = true;
        if (a.size() > 1 && a[1].type == ValueType::String) { sep = a[1].str(); ws = false; }
        if (a.size() > 2) maxsplit = static_cast<long long>(numArg(a[2], "split"));
        if (const Value* v = kwGet(kw, "maxsplit")) maxsplit = static_cast<long long>(numArg(*v, "split"));
        if (const Value* v = kwGet(kw, "sep")) { if (v->type == ValueType::String) { sep = v->str(); ws = false; } }
        std::vector<Value> out;
        if (ws) {
            size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i >= s.size()) break;
                if (maxsplit >= 0 && static_cast<long long>(out.size()) >= maxsplit) { out.push_back(Value::fromString(trimChars(s.substr(i), "", false, true))); i = s.size(); break; }
                size_t j = i;
                while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
                out.push_back(Value::fromString(s.substr(i, j - i)));
                i = j;
            }
            return mkArr(std::move(out));
        }
        if (sep.empty()) fail("split(): pemisah kosong");
        size_t pos = 0;
        while (true) {
            size_t f = s.find(sep, pos);
            if (f == std::string::npos || (maxsplit >= 0 && static_cast<long long>(out.size()) >= maxsplit)) { out.push_back(Value::fromString(s.substr(pos))); break; }
            out.push_back(Value::fromString(s.substr(pos, f - pos)));
            pos = f + sep.size();
        }
        return mkArr(std::move(out));
    }
    if (m == "splitlines") {
        std::vector<Value> out;
        std::string cur;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\n' || s[i] == '\r') {
                out.push_back(Value::fromString(cur));
                cur.clear();
                if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') i++;
            } else cur += s[i];
        }
        if (!cur.empty()) out.push_back(Value::fromString(cur));
        return mkArr(std::move(out));
    }
    if (m == "join") {  // receiver is the separator; the list is the second argument
        std::string r;
        std::vector<Value> parts = elems(a[1], "join");
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i].type != ValueType::String) fail("join(): semua elemen harus teks");
            if (i) r += s;
            r += parts[i].str();
        }
        return Value::fromString(r);
    }
    if (m == "format") return Value::fromString(strFormat(s, a, 1, kw));
    if (m == "title" || m == "capitalize") {
        std::string r = s;
        bool start = true;
        for (char& c : r) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(start ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
                start = m == "capitalize" ? false : false;
            } else start = m == "title";
        }
        if (m == "capitalize" && !r.empty()) { for (size_t i = 1; i < r.size(); i++) r[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[i]))); }
        return Value::fromString(r);
    }
    auto all = [&](int (*pred)(int)) {
        if (s.empty()) return Value::fromBool(false);
        for (char c : s) if (!pred(static_cast<unsigned char>(c))) return Value::fromBool(false);
        return Value::fromBool(true);
    };
    if (m == "isdigit") return all(std::isdigit);
    if (m == "isalpha") return all(std::isalpha);
    if (m == "isalnum") return all(std::isalnum);
    if (m == "isspace") return all(std::isspace);
    if (m == "isupper") { bool any = false; for (char c : s) { if (std::islower(static_cast<unsigned char>(c))) return Value::fromBool(false); if (std::isupper(static_cast<unsigned char>(c))) any = true; } return Value::fromBool(any); }
    if (m == "islower") { bool any = false; for (char c : s) { if (std::isupper(static_cast<unsigned char>(c))) return Value::fromBool(false); if (std::islower(static_cast<unsigned char>(c))) any = true; } return Value::fromBool(any); }
    if (m == "zfill") {
        size_t w = static_cast<size_t>(numArg(a[1], "zfill"));
        if (s.size() >= w) return Value::fromString(s);
        size_t signLen = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
        return Value::fromString(s.substr(0, signLen) + std::string(w - s.size(), '0') + s.substr(signLen));
    }
    if (m == "center" || m == "ljust" || m == "rjust") {
        size_t w = static_cast<size_t>(numArg(a[1], m.c_str()));
        char fill = a.size() > 2 && a[2].type == ValueType::String && !a[2].str().empty() ? a[2].str()[0] : ' ';
        if (s.size() >= w) return Value::fromString(s);
        size_t pad = w - s.size();
        if (m == "ljust") return Value::fromString(s + std::string(pad, fill));
        if (m == "rjust") return Value::fromString(std::string(pad, fill) + s);
        size_t left = pad / 2 + (pad % 2 && w % 2 ? 1 : 0);
        return Value::fromString(std::string(left, fill) + s + std::string(pad - left, fill));
    }
    if (m == "partition") {
        const std::string& sep = argStr(1, "partition");
        size_t f = s.find(sep);
        if (f == std::string::npos) return mkArr({Value::fromString(s), Value::fromString(""), Value::fromString("")});
        return mkArr({Value::fromString(s.substr(0, f)), Value::fromString(sep), Value::fromString(s.substr(f + sep.size()))});
    }
    if (m == "removeprefix") { const std::string& p = argStr(1, m.c_str()); return Value::fromString(s.compare(0, p.size(), p) == 0 && s.size() >= p.size() ? s.substr(p.size()) : s); }
    if (m == "removesuffix") { const std::string& p = argStr(1, m.c_str()); return Value::fromString(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0 ? s.substr(0, s.size() - p.size()) : s); }
    if (m == "encode") return Value::fromString(s);
    fail("metode teks tidak dikenal: " + m);
}

bool containsValue(const std::vector<Value>& v, const Value& x) {
    for (const Value& e : v) if (valuesDeepEqual(e, x)) return true;
    return false;
}

std::vector<Value> uniqueValues(const std::vector<Value>& v) {
    std::vector<Value> out;
    for (const Value& e : v) if (!containsValue(out, e)) out.push_back(e);
    return out;
}

Value setOp(const std::string& op, const std::vector<Value>& x, const std::vector<Value>& y) {
    std::vector<Value> out;
    if (op == "union") { out = uniqueValues(x); for (const Value& e : y) if (!containsValue(out, e)) out.push_back(e); }
    else if (op == "intersection") { for (const Value& e : uniqueValues(x)) if (containsValue(y, e)) out.push_back(e); }
    else if (op == "difference") { for (const Value& e : uniqueValues(x)) if (!containsValue(y, e)) out.push_back(e); }
    else {  // symmetric_difference
        for (const Value& e : uniqueValues(x)) if (!containsValue(y, e)) out.push_back(e);
        for (const Value& e : uniqueValues(y)) if (!containsValue(x, e)) out.push_back(e);
    }
    return mkArr(std::move(out));
}

Value listMethod(const std::string& m, std::vector<Value>& a, const ValueMap* kw, const CallFn& callFn) {
    const Value& self = a[0];
    if (m == "add") { auto& v = mutElems(self); if (!containsValue(v, a.at(1))) v.push_back(a[1]); return Value::null(); }
    if (m == "discard") { auto& v = mutElems(self); for (size_t i = 0; i < v.size(); i++) if (valuesDeepEqual(v[i], a.at(1))) { v.erase(v.begin() + static_cast<long>(i)); break; } return Value::null(); }
    if (m == "union" || m == "intersection" || m == "difference" || m == "symmetric_difference") {
        std::vector<Value> other = a.size() > 1 ? elems(a[1]) : std::vector<Value>{};
        return setOp(m, elems(self), other);
    }
    if (m == "issubset" || m == "issuperset" || m == "isdisjoint") {
        std::vector<Value> x = elems(self), y = elems(a.at(1));
        if (m == "issubset") { for (const Value& e : x) if (!containsValue(y, e)) return Value::fromBool(false); return Value::fromBool(true); }
        if (m == "issuperset") { for (const Value& e : y) if (!containsValue(x, e)) return Value::fromBool(false); return Value::fromBool(true); }
        for (const Value& e : x) if (containsValue(y, e)) return Value::fromBool(false);
        return Value::fromBool(true);
    }
    if (m == "update") {
        auto& v = mutElems(self);
        for (const Value& e : elems(a.at(1))) if (!containsValue(v, e)) v.push_back(e);
        return Value::null();
    }
    if (m == "append") { mutElems(self).push_back(a.at(1)); return Value::null(); }
    if (m == "extend") { std::vector<Value> add = elems(a.at(1), "extend"); auto& v = mutElems(self); v.insert(v.end(), add.begin(), add.end()); return Value::null(); }
    if (m == "insert") {
        auto& v = mutElems(self);
        long long i = static_cast<long long>(numArg(a.at(1), "insert"));
        if (i < 0) i += static_cast<long long>(v.size());
        i = std::max<long long>(0, std::min<long long>(i, static_cast<long long>(v.size())));
        v.insert(v.begin() + i, a.at(2));
        return Value::null();
    }
    if (m == "remove") {
        auto& v = mutElems(self);
        for (size_t i = 0; i < v.size(); i++) if (valuesDeepEqual(v[i], a.at(1))) { v.erase(v.begin() + static_cast<long>(i)); return Value::null(); }
        fail("remove(): nilai tidak ada di larik");
    }
    if (m == "pop") {
        auto& v = mutElems(self);
        if (v.empty()) fail("pop(): larik kosong");
        long long i = a.size() > 1 ? static_cast<long long>(numArg(a[1], "pop")) : -1;
        if (i < 0) i += static_cast<long long>(v.size());
        if (i < 0 || i >= static_cast<long long>(v.size())) fail("pop(): indeks di luar jangkauan");
        Value out = v[static_cast<size_t>(i)];
        v.erase(v.begin() + i);
        return out;
    }
    if (m == "index" || m == "count") {
        std::vector<Value> v = elems(self);
        double n = 0;
        for (size_t i = 0; i < v.size(); i++) {
            if (valuesDeepEqual(v[i], a.at(1))) {
                if (m == "index") return Value::fromNumber(static_cast<double>(i));
                n++;
            }
        }
        if (m == "index") fail("index(): nilai tidak ada di larik");
        return Value::fromNumber(n);
    }
    if (m == "sort") {
        auto& v = mutElems(self);
        const Value* key = kwGet(kw, "key");
        const Value* rev = kwGet(kw, "reverse");
        sortValues(v, key, rev && rev->truthy(), callFn);
        return Value::null();
    }
    if (m == "reverse") { auto& v = mutElems(self); std::reverse(v.begin(), v.end()); return Value::null(); }
    if (m == "copy") return mkArr(elems(self));
    if (m == "clear") { mutElems(self).clear(); return Value::null(); }
    fail("metode larik tidak dikenal: " + m);
}

Value dictMethod(const std::string& m, std::vector<Value>& a, const ValueMap* kw) {
    ValueMap& d = *a[0].map();
    if (m == "keys") { std::vector<Value> out; for (const auto& e : d) out.push_back(Value::fromString(e.first)); return mkArr(std::move(out)); }
    if (m == "values") { std::vector<Value> out; for (const auto& e : d) out.push_back(e.second); return mkArr(std::move(out)); }
    if (m == "items") { std::vector<Value> out; for (const auto& e : d) out.push_back(mkArr({Value::fromString(e.first), e.second})); return mkArr(std::move(out)); }
    auto keyOf = [&](const Value& k) { return k.type == ValueType::String ? k.str() : k.stringify(); };
    if (m == "get") { auto it = d.find(keyOf(a.at(1))); return it == d.end() ? (a.size() > 2 ? a[2] : Value::null()) : it->second; }
    if (m == "setdefault") {
        std::string k = keyOf(a.at(1));
        auto it = d.find(k);
        if (it != d.end()) return it->second;
        Value def = a.size() > 2 ? a[2] : Value::null();
        d[k] = def;
        return def;
    }
    if (m == "pop") {
        std::string k = keyOf(a.at(1));
        auto it = d.find(k);
        if (it == d.end()) { if (a.size() > 2) return a[2]; fail("pop(): kunci '" + k + "' tidak ada"); }
        Value out = it->second;
        d.erase(k);
        return out;
    }
    if (m == "update") {
        if (a.size() > 1) {
            if (a[1].type == ValueType::Map) { for (const auto& e : *a[1].map()) d[e.first] = e.second; }
            else for (const Value& pr : elems(a[1])) { std::vector<Value> p = elems(pr); if (p.size() == 2) d[keyOf(p[0])] = p[1]; }
        }
        if (kw) for (const auto& e : *kw) if (e.first != "__kw__") d[e.first] = e.second;
        return Value::null();
    }
    if (m == "clear") { d.clear(); return Value::null(); }
    if (m == "copy") { auto c = std::make_shared<ValueMap>(d); return Value::fromMap(c); }
    fail("metode peta tidak dikenal: " + m);
}

}  // namespace

Value call(const std::string& name, std::vector<Value>& a, const ValueMap* kw, const CallFn& callFn) {
    if (name.rfind("_m_s_", 0) == 0) { if (a.empty() || a[0].type != ValueType::String) fail("metode teks butuh teks"); return strMethod(name.substr(5), a, kw); }
    if (name.rfind("_m_a_", 0) == 0) { if (a.empty() || !isSeq(a[0])) fail("metode larik butuh larik"); return listMethod(name.substr(5), a, kw, callFn); }
    if (name.rfind("_m_d_", 0) == 0) { if (a.empty() || a[0].type != ValueType::Map) fail("metode peta butuh peta"); return dictMethod(name.substr(5), a, kw); }

    if (name == "abs") { noKw(kw, "abs"); needArgs(a, 1, 1, "abs"); return Value::fromNumber(std::fabs(numArg(a[0], "abs"))); }
    if (name == "min" || name == "max") {
        std::vector<Value> items = a.size() == 1 ? elems(a[0], name.c_str()) : a;
        if (items.empty()) {
            if (const Value* d = kwGet(kw, "default")) return *d;
            fail(name + "(): urutan kosong");
        }
        const Value* key = kwGet(kw, "key");
        Value best = items[0];
        Value bestKey = best;
        if (key && key->type != ValueType::Null) { std::vector<Value> ka{best}; bestKey = callFn(*key, ka); }
        for (size_t i = 1; i < items.size(); i++) {
            Value k = items[i];
            if (key && key->type != ValueType::Null) { std::vector<Value> ka{items[i]}; k = callFn(*key, ka); }
            int c = cmpValues(k, bestKey);
            if ((name == "min" && c < 0) || (name == "max" && c > 0)) { best = items[i]; bestKey = k; }
        }
        return best;
    }
    if (name == "sum") {
        needArgs(a, 1, 2, "sum");
        double total = a.size() > 1 ? numArg(a[1], "sum") : 0;
        if (const Value* st = kwGet(kw, "start")) total = numArg(*st, "sum");
        for (const Value& v : elems(a[0], "sum")) total += numArg(v, "sum");
        return Value::fromNumber(total);
    }
    if (name == "round") {
        needArgs(a, 1, 2, "round");
        double x = numArg(a[0], "round");
        const Value* nd = a.size() > 1 ? &a[1] : kwGet(kw, "ndigits");
        if (!nd || nd->type == ValueType::Null) return Value::fromNumber(std::nearbyint(x));
        double scale = std::pow(10.0, numArg(*nd, "round"));
        return Value::fromNumber(std::nearbyint(x * scale) / scale);
    }
    if (name == "pow" || name == "_pow") {
        needArgs(a, 2, 3, "pow");
        double r = std::pow(numArg(a[0], "pow"), numArg(a[1], "pow"));
        if (a.size() == 3) { double m = numArg(a[2], "pow"); r = std::fmod(r, m); if (r < 0) r += m; }
        return Value::fromNumber(r);
    }
    if (name == "_floordiv") {
        needArgs(a, 2, 2, "//");
        double d = numArg(a[1], "//");
        if (d == 0) fail("pembagian dengan nol");
        return Value::fromNumber(std::floor(numArg(a[0], "//") / d));
    }
    if (name == "divmod") {
        needArgs(a, 2, 2, "divmod");
        double x = numArg(a[0], "divmod"), d = numArg(a[1], "divmod");
        if (d == 0) fail("pembagian dengan nol");
        double q = std::floor(x / d);
        return mkArr({Value::fromNumber(q), Value::fromNumber(x - q * d)});
    }
    if (name == "chr") { needArgs(a, 1, 1, "chr"); long cp = static_cast<long>(numArg(a[0], "chr")); std::string s; if (cp < 0x80) s += static_cast<char>(cp); else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); } else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); } else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); } return Value::fromString(s); }
    if (name == "ord") {
        needArgs(a, 1, 1, "ord");
        const std::string& s = a[0].str();
        if (s.empty()) fail("ord(): teks kosong");
        unsigned char c = static_cast<unsigned char>(s[0]);
        if (c < 0x80) return Value::fromNumber(c);
        int n = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        long cp = c & (0xFF >> (n + 1));
        for (int i = 1; i < n && static_cast<size_t>(i) < s.size(); i++) cp = (cp << 6) | (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0x3F);
        return Value::fromNumber(static_cast<double>(cp));
    }
    if (name == "hex" || name == "bin" || name == "oct") {
        needArgs(a, 1, 1, name.c_str());
        long long n = static_cast<long long>(numArg(a[0], name.c_str()));
        std::string body = toBase(static_cast<unsigned long long>(n < 0 ? -n : n), name == "hex" ? 16 : name == "bin" ? 2 : 8, false);
        return Value::fromString(std::string(n < 0 ? "-" : "") + (name == "hex" ? "0x" : name == "bin" ? "0b" : "0o") + body);
    }
    if (name == "bool") { needArgs(a, 0, 1, "bool"); return Value::fromBool(!a.empty() && a[0].truthy() && !(isSeq(a[0]) && elems(a[0]).empty())); }
    if (name == "int") {
        needArgs(a, 0, 2, "int");
        if (a.empty()) return Value::fromNumber(0);
        return Value::fromNumber(toInt(a[0], a.size() > 1 ? static_cast<int>(numArg(a[1], "int")) : 10));
    }
    if (name == "list" || name == "tuple") { needArgs(a, 0, 1, name.c_str()); return mkArr(a.empty() ? std::vector<Value>{} : elems(a[0], name.c_str())); }
    if (name == "set" || name == "frozenset") {
        needArgs(a, 0, 1, name.c_str());
        std::vector<Value> out;
        if (!a.empty()) for (const Value& v : elems(a[0], name.c_str())) {
            bool dup = false;
            for (const Value& o : out) if (valuesDeepEqual(o, v)) { dup = true; break; }
            if (!dup) out.push_back(v);
        }
        return mkArr(std::move(out));
    }
    if (name == "dict") {
        auto m = std::make_shared<ValueMap>();
        if (!a.empty()) {
            if (a[0].type == ValueType::Map) *m = *a[0].map();
            else for (const Value& pr : elems(a[0], "dict")) { std::vector<Value> p = elems(pr); if (p.size() != 2) fail("dict(): pasangan harus [kunci, nilai]"); (*m)[p[0].type == ValueType::String ? p[0].str() : p[0].stringify()] = p[1]; }
        }
        if (kw) for (const auto& e : *kw) if (e.first != "__kw__") (*m)[e.first] = e.second;
        return Value::fromMap(m);
    }
    if (name == "sorted") {
        needArgs(a, 1, 1, "sorted");
        std::vector<Value> items = elems(a[0], "sorted");
        const Value* rev = kwGet(kw, "reverse");
        sortValues(items, kwGet(kw, "key"), rev && rev->truthy(), callFn);
        return mkArr(std::move(items));
    }
    if (name == "reversed") { needArgs(a, 1, 1, "reversed"); std::vector<Value> v = elems(a[0], "reversed"); std::reverse(v.begin(), v.end()); return mkArr(std::move(v)); }
    if (name == "enumerate") {
        needArgs(a, 1, 2, "enumerate");
        double start = a.size() > 1 ? numArg(a[1], "enumerate") : (kwGet(kw, "start") ? numArg(*kwGet(kw, "start"), "enumerate") : 0);
        std::vector<Value> out;
        double i = start;
        for (const Value& v : elems(a[0], "enumerate")) out.push_back(mkArr({Value::fromNumber(i++), v}));
        return mkArr(std::move(out));
    }
    if (name == "zip") {
        std::vector<std::vector<Value>> cols;
        size_t n = SIZE_MAX;
        for (const Value& v : a) { cols.push_back(elems(v, "zip")); n = std::min(n, cols.back().size()); }
        if (cols.empty()) return mkArr({});
        std::vector<Value> out;
        for (size_t i = 0; i < n; i++) { std::vector<Value> row; for (auto& c : cols) row.push_back(c[i]); out.push_back(mkArr(std::move(row))); }
        return mkArr(std::move(out));
    }
    if (name == "map") {
        if (a.size() < 2) fail("map() butuh fungsi dan minimal satu urutan");
        std::vector<std::vector<Value>> cols;
        size_t n = SIZE_MAX;
        for (size_t i = 1; i < a.size(); i++) { cols.push_back(elems(a[i], "map")); n = std::min(n, cols.back().size()); }
        std::vector<Value> out;
        for (size_t i = 0; i < n; i++) { std::vector<Value> args; for (auto& c : cols) args.push_back(c[i]); out.push_back(callFn(a[0], args)); }
        return mkArr(std::move(out));
    }
    if (name == "filter") {
        needArgs(a, 2, 2, "filter");
        std::vector<Value> out;
        for (const Value& v : elems(a[1], "filter")) {
            bool keep;
            if (a[0].type == ValueType::Null) keep = v.truthy();
            else { std::vector<Value> args{v}; keep = callFn(a[0], args).truthy(); }
            if (keep) out.push_back(v);
        }
        return mkArr(std::move(out));
    }
    if (name == "any" || name == "all") {
        needArgs(a, 1, 1, name.c_str());
        for (const Value& v : elems(a[0], name.c_str())) {
            bool t = v.truthy();
            if (name == "any" && t) return Value::fromBool(true);
            if (name == "all" && !t) return Value::fromBool(false);
        }
        return Value::fromBool(name == "all");
    }
    if (name == "isinstance") { needArgs(a, 2, 2, "isinstance"); return Value::fromBool(matchesType(a[0], a[1])); }
    if (name == "callable") {
        needArgs(a, 1, 1, "callable");
        ValueType t = a[0].type;
        return Value::fromBool(t == ValueType::Fn || t == ValueType::VmFn || t == ValueType::Builtin || t == ValueType::Native || t == ValueType::Class);
    }
    if (name == "_bitor" || name == "_bitand" || name == "_bitxor") {
        needArgs(a, 2, 2, "operator bit");
        if (isSeq(a[0]) && isSeq(a[1])) return setOp(name == "_bitor" ? "union" : name == "_bitand" ? "intersection" : "symmetric_difference", elems(a[0]), elems(a[1]));
        if (a[0].type == ValueType::Map && a[1].type == ValueType::Map && name == "_bitor") {
            auto m = std::make_shared<ValueMap>(*a[0].map());
            for (const auto& e : *a[1].map()) (*m)[e.first] = e.second;
            return Value::fromMap(m);
        }
        long long x = static_cast<long long>(numArg(a[0], "bit")), y = static_cast<long long>(numArg(a[1], "bit"));
        return Value::fromNumber(static_cast<double>(name == "_bitor" ? (x | y) : name == "_bitand" ? (x & y) : (x ^ y)));
    }
    if (name == "_shl" || name == "_shr") {
        needArgs(a, 2, 2, "shift");
        long long x = static_cast<long long>(numArg(a[0], "shift")), n = static_cast<long long>(numArg(a[1], "shift"));
        if (n < 0 || n > 62) fail("jumlah geser tidak valid");
        return Value::fromNumber(static_cast<double>(name == "_shl" ? (x << n) : (x >> n)));
    }
    if (name == "_bitnot") { needArgs(a, 1, 1, "~"); return Value::fromNumber(static_cast<double>(~static_cast<long long>(numArg(a[0], "~")))); }
    if (name == "_setdiff") { needArgs(a, 2, 2, "-"); return setOp("difference", elems(a[0]), elems(a[1])); }
    if (name == "_concat") {
        std::vector<Value> out;
        for (const Value& part : a) { std::vector<Value> e = elems(part); out.insert(out.end(), e.begin(), e.end()); }
        return mkArr(std::move(out));
    }
    if (name == "_kwmerge") {
        auto m = std::make_shared<ValueMap>();
        for (const Value& part : a) {
            if (part.type != ValueType::Map) fail("**: butuh peta");
            for (const auto& e : *part.map()) (*m)[e.first] = e.second;
        }
        return Value::fromMap(m);
    }
    if (name == "_percent") { needArgs(a, 2, 2, "%"); return Value::fromString(percentFormat(a[0].str(), a[1])); }
    if (name == "_fmt") { needArgs(a, 2, 2, "format"); return Value::fromString(formatValue(a[0], a[1].str())); }
    if (name == "_delitem") {
        needArgs(a, 2, 2, "del");
        if (a[0].type == ValueType::Map) {
            std::string k = a[1].type == ValueType::String ? a[1].str() : a[1].stringify();
            if (!a[0].map()->count(k)) fail("del: kunci '" + k + "' tidak ada");
            a[0].map()->erase(k);
            return Value::null();
        }
        if (isSeq(a[0])) {
            auto& v = mutElems(a[0]);
            long long i = static_cast<long long>(numArg(a[1], "del"));
            if (i < 0) i += static_cast<long long>(v.size());
            if (i < 0 || i >= static_cast<long long>(v.size())) fail("del: indeks di luar jangkauan");
            v.erase(v.begin() + i);
            return Value::null();
        }
        fail("del: hanya untuk larik atau peta");
    }
    if (name == "_assert") {
        needArgs(a, 1, 2, "assert");
        if (!a[0].truthy()) fail(a.size() > 1 ? "AssertionError: " + a[1].stringify() : std::string("AssertionError"));
        return Value::null();
    }
    fail("builtin tidak dikenal: " + name);
}

}  // namespace pylib
