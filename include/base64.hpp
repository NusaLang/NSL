#pragma once

#include <stdexcept>
#include <string>

// Minimal, self-contained Base64 (RFC 4648) codec -- no external deps,
// so it builds and behaves identically everywhere Nusantara does
// (Linux, Termux/Android, NDK cross-compiles).
namespace base64 {

inline std::string encode(const std::string& in) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned char b0 = static_cast<unsigned char>(in[i]);
        unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
        unsigned char b2 = static_cast<unsigned char>(in[i + 2]);
        out += table[b0 >> 2];
        out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += table[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out += table[b2 & 0x3F];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned char b0 = static_cast<unsigned char>(in[i]);
        out += table[b0 >> 2];
        out += table[(b0 & 0x03) << 4];
        out += "==";
    } else if (rem == 2) {
        unsigned char b0 = static_cast<unsigned char>(in[i]);
        unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
        out += table[b0 >> 2];
        out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += table[(b1 & 0x0F) << 2];
        out += "=";
    }
    return out;
}

inline std::string decode(const std::string& in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve((in.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = value(c);
        if (v < 0) throw std::runtime_error("karakter base64 nggak valid");
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

}  // namespace base64
