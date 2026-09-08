#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include "base64.hpp"
#include "json.hpp"
#include "sha256.hpp"
#include "value.hpp"

// Minimal JWT (JSON Web Token), HS256 only -- no RS256/ES256 (those
// need asymmetric-key crypto, well beyond "base"), no header options
// beyond {"alg":"HS256","typ":"JWT"}. Built entirely on the base64/
// sha256/json primitives already in this codebase.
namespace jwt {

namespace detail {

inline std::string base64UrlEncode(const std::string& s) {
    std::string b64 = base64::encode(s);
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return b64;
}

inline std::string base64UrlDecode(const std::string& s) {
    std::string b64 = s;
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64 += '=';
    return base64::decode(b64);
}

// Constant-time: an early-exit `==` on a signature leaks timing info
// an attacker could use to guess it byte by byte.
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

}  // namespace detail

// `payload` must be a Map. Returns the signed token string.
inline std::string create(const Value& payload, const std::string& secret) {
    if (payload.type != ValueType::Map) throw std::runtime_error("jwt_buat(): payload harus peta");
    std::string headerB64 = detail::base64UrlEncode(R"({"alg":"HS256","typ":"JWT"})");
    std::string payloadB64 = detail::base64UrlEncode(json::encode(payload));
    std::string signingInput = headerB64 + "." + payloadB64;

    auto sig = hmac::sha256(secret, signingInput);
    std::string sigStr(sig.begin(), sig.end());
    return signingInput + "." + detail::base64UrlEncode(sigStr);
}

// Verifies signature + "exp" claim if present. Returns Value::null()
// for ANY failure (malformed/bad signature/expired) without
// distinguishing which -- callers can't leak which one it was.
inline Value verify(const std::string& token, const std::string& secret) {
    size_t firstDot = token.find('.');
    if (firstDot == std::string::npos) return Value::null();
    size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return Value::null();

    std::string headerB64 = token.substr(0, firstDot);
    std::string payloadB64 = token.substr(firstDot + 1, secondDot - firstDot - 1);
    std::string sigB64 = token.substr(secondDot + 1);

    std::string signingInput = headerB64 + "." + payloadB64;
    auto expectedSig = hmac::sha256(secret, signingInput);
    std::string expectedSigStr(expectedSig.begin(), expectedSig.end());
    std::string expectedSigB64 = detail::base64UrlEncode(expectedSigStr);

    if (!detail::constantTimeEquals(expectedSigB64, sigB64)) return Value::null();

    Value payload;
    try {
        payload = json::decode(detail::base64UrlDecode(payloadB64));
    } catch (const std::exception&) {
        return Value::null();
    }
    if (payload.type != ValueType::Map) return Value::null();

    auto it = payload.map()->find("exp");
    if (it != payload.map()->end() && it->second.type == ValueType::Number) {
        double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (now > it->second.number) return Value::null();  // expired
    }
    return payload;
}

}  // namespace jwt
