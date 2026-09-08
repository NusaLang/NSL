#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "value.hpp"

// Minimal, self-contained JSON codec (no external deps) mapping
// directly to/from Value: null/boolean/angka/teks/larik/peta cover
// exactly what JSON can represent. `fungsi`/builtin values can't be
// represented in JSON -- encoding one throws.
namespace json {

inline void encodeString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

inline void encodeValue(const Value& v, std::string& out) {
    switch (v.type) {
        case ValueType::Null:
            out += "null";
            return;
        case ValueType::Bool:
            out += v.boolean() ? "true" : "false";
            return;
        case ValueType::Number:
            out += Value::formatNumber(v.number);
            return;
        case ValueType::String:
            encodeString(v.str(), out);
            return;
        case ValueType::Array: {
            out += '[';
            for (size_t i = 0; i < v.array()->size(); i++) {
                if (i > 0) out += ',';
                encodeValue((*v.array())[i], out);
            }
            out += ']';
            return;
        }
        case ValueType::Map: {
            out += '{';
            bool first = true;
            for (const auto& [k, val] : *v.map()) {
                if (!first) out += ',';
                first = false;
                encodeString(k, out);
                out += ':';
                encodeValue(val, out);
            }
            out += '}';
            return;
        }
        case ValueType::Fn:
        case ValueType::Builtin:
            throw std::runtime_error("json_encode(): nggak bisa encode fungsi ke JSON");
        case ValueType::Channel:
            throw std::runtime_error("json_encode(): nggak bisa encode kanal ke JSON");
        case ValueType::WaitGroup:
            throw std::runtime_error("json_encode(): nggak bisa encode waitgroup ke JSON");
        case ValueType::Native:
            throw std::runtime_error("json_encode(): nggak bisa encode fungsi plugin ke JSON");
        case ValueType::Class:
            throw std::runtime_error("json_encode(): nggak bisa encode kelas ke JSON");
        case ValueType::VmFn:
            throw std::runtime_error("json_encode(): nggak bisa encode fungsi ke JSON");
        case ValueType::VmArray: {
            out += '[';
            if (v.vmArray()->numeric) {
                for (size_t i = 0; i < v.vmArray()->nums.size(); i++) {
                    if (i) out += ',';
                    encodeValue(Value::fromNumber(v.vmArray()->nums[i]), out);
                }
            } else {
                for (size_t i = 0; i < v.vmArray()->boxed->size(); i++) {
                    if (i) out += ',';
                    encodeValue((*v.vmArray()->boxed)[i], out);
                }
            }
            out += ']';
            return;
        }
        case ValueType::Instance: {
            out += '{';
            bool first = true;
            for (const auto& [k, val] : *v.instance()->fields) {
                if (!first) out += ',';
                first = false;
                encodeString(k, out);
                out += ':';
                encodeValue(val, out);
            }
            out += '}';
            return;
        }
    }
}

inline std::string encode(const Value& v) {
    std::string out;
    encodeValue(v, out);
    return out;
}

// Straightforward recursive-descent JSON parser. Not fully spec-strict
// (no surrogate-pair \u handling for astral characters, accepts a
// trailing-whitespace-only tail) -- covers everything a normal REST
// API request/response body actually uses.
class Decoder {
public:
    explicit Decoder(const std::string& s) : s_(s) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (pos_ != s_.size()) throw std::runtime_error("JSON: ada sisa teks setelah nilai selesai");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char advance() { return s_[pos_++]; }
    void skipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }
    void expect(char c) {
        if (pos_ >= s_.size() || s_[pos_] != c) {
            throw std::runtime_error(std::string("JSON: diharap '") + c + "'");
        }
        pos_++;
    }
    void expectLiteral(const char* lit) {
        size_t len = std::strlen(lit);
        if (s_.compare(pos_, len, lit) != 0) {
            throw std::runtime_error(std::string("JSON: literal nggak valid, diharap '") + lit + "'");
        }
        pos_ += len;
    }

    Value parseValue() {
        skipWs();
        if (pos_ >= s_.size()) throw std::runtime_error("JSON: input berakhir nggak lengkap");
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value::fromString(parseStringRaw());
        if (c == 't') {
            expectLiteral("true");
            return Value::fromBool(true);
        }
        if (c == 'f') {
            expectLiteral("false");
            return Value::fromBool(false);
        }
        if (c == 'n') {
            expectLiteral("null");
            return Value::null();
        }
        return parseNumber();
    }

    Value parseNumber() {
        size_t start = pos_;
        if (peek() == '-') pos_++;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) pos_++;
        if (peek() == '.') {
            pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) pos_++;
        }
        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) pos_++;
        }
        if (pos_ == start) throw std::runtime_error("JSON: angka nggak valid");
        return Value::fromNumber(std::stod(s_.substr(start, pos_ - start)));
    }

    std::string parseStringRaw() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) throw std::runtime_error("JSON: string nggak ditutup");
            char c = advance();
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) throw std::runtime_error("JSON: escape nggak lengkap");
            char esc = advance();
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) throw std::runtime_error("JSON: \\u nggak lengkap");
                    unsigned int code = static_cast<unsigned int>(std::stoul(s_.substr(pos_, 4), nullptr, 16));
                    pos_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    throw std::runtime_error("JSON: escape nggak dikenal");
            }
        }
        return out;
    }

    Value parseArray() {
        expect('[');
        auto arr = std::make_shared<std::vector<Value>>();
        skipWs();
        if (peek() == ']') {
            pos_++;
            return Value::fromArray(arr);
        }
        while (true) {
            arr->push_back(parseValue());
            skipWs();
            if (peek() == ',') {
                pos_++;
                continue;
            }
            expect(']');
            break;
        }
        return Value::fromArray(arr);
    }

    Value parseObject() {
        expect('{');
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
        skipWs();
        if (peek() == '}') {
            pos_++;
            return Value::fromMap(m);
        }
        while (true) {
            skipWs();
            std::string key = parseStringRaw();
            skipWs();
            expect(':');
            (*m)[key] = parseValue();
            skipWs();
            if (peek() == ',') {
                pos_++;
                continue;
            }
            expect('}');
            break;
        }
        return Value::fromMap(m);
    }
};

inline Value decode(const std::string& s) {
    Decoder d(s);
    return d.parse();
}

}  // namespace json
