#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

// Text is stored as UTF-8 bytes, but len / indexing / slicing count characters like Python. ASCII text
// (the common case) is detected once per string and cached, so s[i] in a loop stays O(1).
namespace u8 {

inline bool isAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

struct Info {
    std::shared_ptr<void> keep;  // holds the string alive so its address can't be reused while cached
    const void* key = nullptr;
    bool ascii = true;
    std::vector<uint32_t> offs;  // byte offset of each character (non-ASCII text only), plus the end
};

inline const Info& info(const Value& v) {
    static thread_local Info cache;
    static thread_local Info small;
    const std::string& s = v.str();
    if (s.size() < 48) {  // tiny strings: cheaper to recompute than to cache
        small.ascii = isAscii(s);
        small.offs.clear();
        if (!small.ascii) {
            for (size_t i = 0; i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) small.offs.push_back(static_cast<uint32_t>(i));
            small.offs.push_back(static_cast<uint32_t>(s.size()));
        }
        return small;
    }
    if (cache.key == v.ref.get()) return cache;
    cache.keep = v.ref;
    cache.key = v.ref.get();
    cache.ascii = isAscii(s);
    cache.offs.clear();
    if (!cache.ascii) {
        for (size_t i = 0; i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) cache.offs.push_back(static_cast<uint32_t>(i));
        cache.offs.push_back(static_cast<uint32_t>(s.size()));
    }
    return cache;
}

inline size_t length(const Value& v) {
    const Info& in = info(v);
    return in.ascii ? v.str().size() : in.offs.size() - 1;
}

// Characters [lo, hi) as text; indexes already clamped to [0, length].
inline std::string slice(const Value& v, size_t lo, size_t hi) {
    const Info& in = info(v);
    if (lo >= hi) return std::string();
    if (in.ascii) return v.str().substr(lo, hi - lo);
    return v.str().substr(in.offs[lo], in.offs[hi] - in.offs[lo]);
}

// Character index of a byte offset (for find/index results).
inline size_t charIndexOfByte(const std::string& s, size_t byteOff) {
    size_t n = 0;
    for (size_t i = 0; i < byteOff && i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) n++;
    return n;
}

inline size_t charCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

inline uint32_t decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = n == 1 ? c : (c & (0xFF >> (n + 1)));
    for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
    i += static_cast<size_t>(n);
    return cp;
}

inline void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// Simple case mapping: ASCII, Latin-1, Latin Extended-A pairs, Greek and Cyrillic.
inline uint32_t toUpperCp(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        bool oddLower = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (oddLower) return (c % 2 == 0) ? c - 1 : c;
        return (c % 2 == 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

inline uint32_t toLowerCp(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x138 && c != 0x149 && c != 0x178) {
        bool oddLower = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (oddLower) return (c % 2 == 1) ? c + 1 : c;
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c == 0x178) return 0xFF;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

inline std::string mapCase(const std::string& s, bool upper) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = decode(s, i);
        encode(upper ? toUpperCp(cp) : toLowerCp(cp), out);
    }
    return out;
}

}  // namespace u8
