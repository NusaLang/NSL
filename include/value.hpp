#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct FnDeclStmt;
class Environment;
struct Value;
struct ChannelState;  // defined after Value below -- needs the complete type for its deque

// A user-defined `fungsi`: the AST node plus the environment it closed
// over. `closure` is a non-owning raw pointer -- Environment is GC-owned
// (gc.hpp), not shared_ptr, since a named function closing over its own
// defining environment is a direct reference cycle.
struct Function {
    const FnDeclStmt* decl;
    Environment* closure = nullptr;
};

// Backing store for a `waitgroup` value (Go-style counter + condvar).
struct WaitGroupState {
    std::mutex mu;
    std::condition_variable cv;
    long counter = 0;
};

// Backing store for a loaded native plugin (.so, dlopen-based). Held by
// shared_ptr from every NativeFunction it exposes, so the .so can't be
// dlclose'd while still reachable. Destructor (dlclose) lives in
// plugin.cpp so this header doesn't need <dlfcn.h>.
struct NativePlugin {
    void* handle = nullptr;
    std::string path;
    ~NativePlugin();
};

// A single function exposed by a loaded native plugin. `fnPtr` is really
// an NsFn (plugin_abi.h), kept as void* so this header stays ABI-free.
struct NativeFunction {
    std::shared_ptr<NativePlugin> plugin;
    void* fnPtr = nullptr;
    std::string name;
    int abiVer = 1; // ABI plugin: 2 = string length-aware
};

enum class ValueType { Null, Bool, Number, String, Fn, Builtin, Array, Map, Channel, WaitGroup, Native, Class, Instance, VmFn, VmArray };
struct ClassInfo;
struct InstanceState;
struct VmClosure;
struct VmArrayState;

// Tagged value, 32 bytes: type tag, inline double payload slot, one
// type-erased shared_ptr for heap data. Never mutated in place (every
// case goes through a `from*` factory) -- copying a Value copies the
// handle, like JS/Python reference semantics. Use `*Shared()` accessors
// when the object must outlive a blocking wait.
struct Value {
    ValueType type = ValueType::Null;
    // Bool reuses this same slot as 0.0/1.0 (see boolean()).
    double number = 0.0;
    // Heap handle; what it points at is decided by `type`:
    //   String/Builtin -> std::string    Fn        -> Function
    //   Array          -> vector<Value>  Map       -> unordered_map<string, Value>
    //   Channel        -> ChannelState   WaitGroup -> WaitGroupState
    //   Native         -> NativeFunction Class     -> ClassInfo
    //   Instance       -> InstanceState  VmFn      -> VmClosure
    //   VmArray        -> VmArrayState
    // Null/Bool/Number leave it empty.
    std::shared_ptr<void> ref;

    static const std::string& emptyString() {
        static const std::string empty;
        return empty;
    }

    template <typename T>
    T* as() const {
        return static_cast<T*>(ref.get());
    }

    // ---- accessors (were plain fields before the 240 -> 32 byte shrink) ----
    bool boolean() const { return number != 0.0; }
    const std::string& str() const {
        return type == ValueType::String ? *as<std::string>() : emptyString();
    }
    const std::string& builtinName() const {
        return type == ValueType::Builtin ? *as<std::string>() : emptyString();
    }
    Function* fn() const { return type == ValueType::Fn ? as<Function>() : nullptr; }
    std::vector<Value>* array() const {
        return type == ValueType::Array ? as<std::vector<Value>>() : nullptr;
    }
    std::unordered_map<std::string, Value>* map() const {
        return type == ValueType::Map ? as<std::unordered_map<std::string, Value>>() : nullptr;
    }
    ChannelState* channel() const {
        return type == ValueType::Channel ? as<ChannelState>() : nullptr;
    }
    WaitGroupState* waitgroup() const {
        return type == ValueType::WaitGroup ? as<WaitGroupState>() : nullptr;
    }
    NativeFunction* native() const {
        return type == ValueType::Native ? as<NativeFunction>() : nullptr;
    }
    ClassInfo* klass() const { return type == ValueType::Class ? as<ClassInfo>() : nullptr; }
    InstanceState* instance() const {
        return type == ValueType::Instance ? as<InstanceState>() : nullptr;
    }
    VmClosure* vmClosure() const { return type == ValueType::VmFn ? as<VmClosure>() : nullptr; }
    VmArrayState* vmArray() const {
        return type == ValueType::VmArray ? as<VmArrayState>() : nullptr;
    }

    // Ownership-taking variants -- only for the handful of places that need
    // the payload to outlive the Value they read it from (a channel held
    // across a blocking kanal_terima, a ClassInfo stored into an
    // InstanceState / another ClassInfo's parent, a Function passed to
    // callFunction).
    std::shared_ptr<ChannelState> channelShared() const {
        return type == ValueType::Channel ? std::static_pointer_cast<ChannelState>(ref)
                                          : nullptr;
    }
    std::shared_ptr<WaitGroupState> waitgroupShared() const {
        return type == ValueType::WaitGroup ? std::static_pointer_cast<WaitGroupState>(ref)
                                            : nullptr;
    }
    std::shared_ptr<ClassInfo> klassShared() const {
        return type == ValueType::Class ? std::static_pointer_cast<ClassInfo>(ref) : nullptr;
    }
    std::shared_ptr<Function> fnShared() const {
        return type == ValueType::Fn ? std::static_pointer_cast<Function>(ref) : nullptr;
    }
    std::shared_ptr<std::vector<Value>> arrayShared() const {
        return type == ValueType::Array ? std::static_pointer_cast<std::vector<Value>>(ref)
                                        : nullptr;
    }
    std::shared_ptr<std::unordered_map<std::string, Value>> mapShared() const {
        return type == ValueType::Map
                   ? std::static_pointer_cast<std::unordered_map<std::string, Value>>(ref)
                   : nullptr;
    }

    static Value null() { return Value{}; }

    static Value fromBool(bool b) {
        Value v;
        v.type = ValueType::Bool;
        v.number = b ? 1.0 : 0.0;
        return v;
    }
    static Value fromNumber(double n) {
        Value v;
        v.type = ValueType::Number;
        v.number = n;
        return v;
    }
    static Value fromString(std::string s) {
        Value v;
        v.type = ValueType::String;
        v.ref = std::make_shared<std::string>(std::move(s));
        return v;
    }
    static Value fromFunction(std::shared_ptr<Function> f) {
        Value v;
        v.type = ValueType::Fn;
        v.ref = std::move(f);
        return v;
    }
    static Value builtin(std::string name) {
        Value v;
        v.type = ValueType::Builtin;
        v.ref = std::make_shared<std::string>(std::move(name));
        return v;
    }
    static Value fromArray(std::shared_ptr<std::vector<Value>> a) {
        Value v;
        v.type = ValueType::Array;
        v.ref = std::move(a);
        return v;
    }
    static Value newArray() { return fromArray(std::make_shared<std::vector<Value>>()); }
    static Value fromMap(std::shared_ptr<std::unordered_map<std::string, Value>> m) {
        Value v;
        v.type = ValueType::Map;
        v.ref = std::move(m);
        return v;
    }
    static Value newMap() {
        return fromMap(std::make_shared<std::unordered_map<std::string, Value>>());
    }
    static Value fromChannel(std::shared_ptr<ChannelState> c) {
        Value v;
        v.type = ValueType::Channel;
        v.ref = std::move(c);
        return v;
    }
    static Value fromWaitGroup(std::shared_ptr<WaitGroupState> w) {
        Value v;
        v.type = ValueType::WaitGroup;
        v.ref = std::move(w);
        return v;
    }
    static Value fromNative(std::shared_ptr<NativeFunction> n) {
        Value v;
        v.type = ValueType::Native;
        v.ref = std::move(n);
        return v;
    }
    static Value fromClass(std::shared_ptr<ClassInfo> c) {
        Value v;
        v.type = ValueType::Class;
        v.ref = std::move(c);
        return v;
    }
    static Value fromInstance(std::shared_ptr<InstanceState> i) {
        Value v;
        v.type = ValueType::Instance;
        v.ref = std::move(i);
        return v;
    }
    static Value fromVmClosure(std::shared_ptr<VmClosure> c) {
        Value v;
        v.type = ValueType::VmFn;
        v.ref = std::move(c);
        return v;
    }
    static Value fromVmArray(std::shared_ptr<VmArrayState> a) {
        Value v;
        v.type = ValueType::VmArray;
        v.ref = std::move(a);
        return v;
    }

    bool truthy() const {
        if (type == ValueType::Null) return false;
        if (type == ValueType::Bool) return boolean();
        return true;
    }

    bool callable() const {
        return type == ValueType::Fn || type == ValueType::Builtin || type == ValueType::Native ||
               type == ValueType::Class || type == ValueType::VmFn;
    }

    const char* typeName() const;

    static std::string formatNumber(double number) {
        if (number == static_cast<long long>(number)) {
            return std::to_string(static_cast<long long>(number));
        }
        // Shortest decimal that round-trips to the exact same double --
        // default 6-digit precision silently truncated large timestamps.
        for (int precision = 1; precision <= 17; precision++) {
            std::ostringstream oss;
            oss << std::setprecision(precision) << number;
            std::string s = oss.str();
            if (std::stod(s) == number) return s;
        }
        std::ostringstream oss;
        oss << std::setprecision(17) << number;
        return oss.str();
    }

    std::string stringify() const;
};

// Backing store for a `kanal` (channel) value. capacity == 0 means
// unbounded; a positive capacity blocks like a buffered Go channel.
struct ChannelState {
    std::mutex mu;
    std::condition_variable notEmpty;
    std::condition_variable notFull;
    std::deque<Value> queue;
    size_t capacity = 0;
    bool closed = false;
};

struct ClassInfo {
    std::string name;
    std::shared_ptr<ClassInfo> parent;
    std::unordered_map<std::string, std::shared_ptr<Function>> methods;
    std::vector<std::string> structFields;
    bool isStruct = false;
    bool isEnum = false;
};

struct InstanceState {
    std::shared_ptr<ClassInfo> classInfo;
    std::shared_ptr<std::unordered_map<std::string, Value>> fields;
};

struct VmArrayState {
    bool numeric = true;
    std::vector<double> nums;
    std::shared_ptr<std::vector<Value>> boxed;
};

inline const char* Value::typeName() const {
    switch (type) {
        case ValueType::Null: return "kosong";
        case ValueType::Bool: return "boolean";
        case ValueType::Number: return "angka";
        case ValueType::String: return "teks";
        case ValueType::Fn: return "fungsi";
        case ValueType::Builtin: return "fungsi";
        case ValueType::Array: return "larik";
        case ValueType::Map: return "peta";
        case ValueType::Channel: return "kanal";
        case ValueType::WaitGroup: return "waitgroup";
        case ValueType::Native: return "fungsi";
        case ValueType::Class: return "kelas";
        case ValueType::Instance: return instance() && instance()->classInfo ? instance()->classInfo->name.c_str() : "objek";
        case ValueType::VmFn: return "fungsi";
        case ValueType::VmArray: return "larik";
    }
    return "?";
}

inline std::string Value::stringify() const {
    switch (type) {
        case ValueType::Null: return "kosong";
        case ValueType::Bool: return boolean() ? "benar" : "salah";
        case ValueType::String: return str();
        case ValueType::Fn: return "<fn>";
        case ValueType::Builtin: return "<builtin " + builtinName() + ">";
        case ValueType::Number: return formatNumber(number);
        case ValueType::Array: {
            std::string out = "[";
            for (size_t i = 0; i < array()->size(); i++) {
                if (i > 0) out += ", ";
                const Value& el = (*array())[i];
                out += (el.type == ValueType::String) ? ("\"" + el.str() + "\"") : el.stringify();
            }
            out += "]";
            return out;
        }
        case ValueType::Map: {
            auto itTipe = map()->find("tipe");
            if (itTipe != map()->end() && itTipe->second.type == ValueType::String) {
                const std::string& t = itTipe->second.str();
                if (t == "elemen") {
                    std::string tag = "div";
                    auto itTag = map()->find("tag");
                    if (itTag != map()->end()) {
                        tag = itTag->second.type == ValueType::String ? itTag->second.str() : itTag->second.stringify();
                    }
                    // paritas Fizz: nama atribut kanonik + skip nilai kosong
                    auto attrNameOf = [](const std::string& k) -> std::string {
                        if (k == "kelas" || k == "className") return "class";
                        if (k == "tipe") return "type";
                        if (k == "nama") return "name";
                        if (k == "nilai") return "value";
                        if (k == "htmlFor" || k == "untuk") return "for";
                        if (k == "tabIndex") return "tabindex";
                        if (k == "httpEquiv") return "http-equiv";
                        if (k == "autoFocus") return "autofocus";
                        if (k == "readOnly") return "readonly";
                        if (k == "maxLength") return "maxlength";
                        if (k == "strokeWidth") return "stroke-width";
                        if (k == "strokeLinecap") return "stroke-linecap";
                        if (k == "strokeLinejoin") return "stroke-linejoin";
                        if (k == "fillRule") return "fill-rule";
                        if (k == "clipRule") return "clip-rule";
                        if (k == "strokeDasharray") return "stroke-dasharray";
                        return k;
                    };
                    auto escapeAttr = [](const std::string& s) -> std::string {
                        std::string out;
                        out.reserve(s.size());
                        for (char ch : s) {
                            switch (ch) {
                                case '&': out += "&amp;"; break;
                                case '<': out += "&lt;"; break;
                                case '>': out += "&gt;"; break;
                                case '"': out += "&quot;"; break;
                                default: out += ch;
                            }
                        }
                        return out;
                    };
                    std::string attrs = "";
                    auto itProps = map()->find("props");
                    if (itProps != map()->end() && itProps->second.type == ValueType::Map) {
                        for (const auto& [k, v] : *itProps->second.map()) {
                            if (k != "key" && k != "ref" && k != "children" && v.type != ValueType::Fn && v.type != ValueType::VmFn && v.type != ValueType::Null) {
                                std::string attrName = attrNameOf(k);
                                bool voidBool = (v.type == ValueType::Bool);
                                if (voidBool) {
                                    if (v.boolean()) attrs += " " + attrName;
                                    continue;
                                }
                                attrs += " " + attrName + "=\"" + escapeAttr(v.stringify()) + "\"";
                            }
                        }
                    }
                    // anak: skip kosong, LARIK di-flatten (bukan di-stringify --
                    // dulu larik anak ke-stringify jadi teks "[...]" nyasar)
                    std::function<void(const Value&, std::string&)> emitChild =
                        [&](const Value& c, std::string& out) {
                            if (c.type == ValueType::Null) return;
                            if (c.type == ValueType::Array) {
                                for (const auto& x : *c.array()) emitChild(x, out);
                                return;
                            }
                            out += c.stringify();
                        };
                    std::string childrenHtml = "";
                    auto itChildren = map()->find("children");
                    if (itChildren != map()->end() && itChildren->second.type == ValueType::Array) {
                        for (const auto& child : *itChildren->second.array()) {
                            emitChild(child, childrenHtml);
                        }
                    } else if (itChildren != map()->end() && itChildren->second.type != ValueType::Null) {
                        emitChild(itChildren->second, childrenHtml);
                    }
                    if (tag == "br" || tag == "hr" || tag == "img" || tag == "input" || tag == "link" || tag == "meta"
                        || tag == "area" || tag == "base" || tag == "col" || tag == "embed" || tag == "source"
                        || tag == "track" || tag == "wbr" || tag == "param") {
                        return "<" + tag + attrs + " />";
                    }
                    return "<" + tag + attrs + ">" + childrenHtml + "</" + tag + ">";
                } else if (t == "teks" || t == "mentah") {
                    auto itIsi = map()->find("isi");
                    if (itIsi == map()->end() || itIsi->second.type == ValueType::Null) return "";
                    if (t == "mentah") return itIsi->second.stringify();
                    // teks di-escape ala escapeTextForBrowser (mentah tetap mentah)
                    {
                        const std::string s = itIsi->second.stringify();
                        std::string out;
                        out.reserve(s.size());
                        for (char ch : s) {
                            switch (ch) {
                                case '&': out += "&amp;"; break;
                                case '<': out += "&lt;"; break;
                                case '>': out += "&gt;"; break;
                                case '"': out += "&quot;"; break;
                                case '\'': out += "&#x27;"; break;
                                default: out += ch;
                            }
                        }
                        return out;
                    }
                } else if (t == "pecahan") {
                    std::function<void(const Value&, std::string&)> emitChild2 =
                        [&](const Value& c, std::string& out) {
                            if (c.type == ValueType::Null) return;
                            if (c.type == ValueType::Array) {
                                for (const auto& x : *c.array()) emitChild2(x, out);
                                return;
                            }
                            out += c.stringify();
                        };
                    std::string childrenHtml = "";
                    auto itChildren = map()->find("children");
                    if (itChildren != map()->end() && itChildren->second.type == ValueType::Array) {
                        for (const auto& child : *itChildren->second.array()) {
                            emitChild2(child, childrenHtml);
                        }
                    }
                    return childrenHtml;
                }
            }

            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : *map()) {
                if (!first) out += ", ";
                first = false;
                out += k + ": ";
                out += (v.type == ValueType::String) ? ("\"" + v.str() + "\"") : v.stringify();
            }
            out += "}";
            return out;
        }
        case ValueType::Channel:
            return "<kanal>";
        case ValueType::WaitGroup:
            return "<waitgroup>";
        case ValueType::Native:
            return "<plugin " + (native() ? native()->name : "") + ">";
        case ValueType::Class:
            return "<kelas " + (klass() ? klass()->name : "") + ">";
        case ValueType::Instance:
            return "<objek " + (instance() && instance()->classInfo ? instance()->classInfo->name : "") + ">";
        case ValueType::VmFn:
            return "<fn>";
        case ValueType::VmArray: {
            std::string out = "[";
            if (vmArray()->numeric) {
                for (size_t i = 0; i < vmArray()->nums.size(); i++) {
                    if (i > 0) out += ", ";
                    out += formatNumber(vmArray()->nums[i]);
                }
            } else {
                for (size_t i = 0; i < vmArray()->boxed->size(); i++) {
                    if (i > 0) out += ", ";
                    const Value& el = (*vmArray()->boxed)[i];
                    out += (el.type == ValueType::String) ? ("\"" + el.str() + "\"") : el.stringify();
                }
            }
            out += "]";
            return out;
        }
    }
    return "";
}
