#include "plugin_abi.h"
#include "sha256.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace {

std::string toHex(const unsigned char* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out += d[(data[i] >> 4) & 0xF];
        out += d[data[i] & 0xF];
    }
    return out;
}

std::string toHex(const std::string& s) {
    return toHex(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = v(hex[i]);
        int lo = v(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

std::string md5Digest(const std::string& input) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string msg = input;
    uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += static_cast<char>(0x00);
    for (int i = 0; i < 8; i++) msg += static_cast<char>((bitLen >> (i * 8)) & 0xFF);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            size_t p = chunk + static_cast<size_t>(i) * 4;
            M[i] = static_cast<uint32_t>(static_cast<uint8_t>(msg[p])) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 1])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 2])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 3])) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + rotl32(F, S[i]);
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }
    unsigned char out[16];
    uint32_t regs[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = static_cast<unsigned char>(regs[i] & 0xFF);
        out[i * 4 + 1] = static_cast<unsigned char>((regs[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<unsigned char>((regs[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<unsigned char>((regs[i] >> 24) & 0xFF);
    }
    return toHex(out, 16);
}

std::string sha1Digest(const std::string& input) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string msg = input;
    uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += static_cast<char>(0x00);
    for (int i = 7; i >= 0; i--) msg += static_cast<char>((bitLen >> (i * 8)) & 0xFF);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            size_t p = chunk + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(msg[p])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<uint8_t>(msg[p + 3]));
        }
        for (int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    unsigned char out[20];
    uint32_t regs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = static_cast<unsigned char>((regs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<unsigned char>((regs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<unsigned char>((regs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<unsigned char>(regs[i] & 0xFF);
    }
    return toHex(out, 20);
}

std::string sha512Digest(const std::string& input) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
    };
    uint64_t h[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    std::string msg = input;
    unsigned __int128 bitLen = static_cast<unsigned __int128>(input.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 128 != 112) msg += static_cast<char>(0x00);
    for (int i = 15; i >= 0; i--) msg += static_cast<char>((bitLen >> (i * 8)) & 0xFF);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            size_t p = chunk + static_cast<size_t>(i) * 8;
            uint64_t v = 0;
            for (int j = 0; j < 8; j++) v = (v << 8) | static_cast<uint8_t>(msg[p + j]);
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t temp1 = hh + S1 + ch + K[i] + w[i];
            uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t temp2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    unsigned char out[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[i * 8 + j] = static_cast<unsigned char>((h[i] >> ((7 - j) * 8)) & 0xFF);
    return toHex(out, 64);
}

const unsigned char kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
unsigned char kInvSbox[256];
bool kInvSboxInit = [] {
    for (int i = 0; i < 256; i++) kInvSbox[kSbox[i]] = static_cast<unsigned char>(i);
    return true;
}();
const unsigned char kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

unsigned char gmul(unsigned char a, unsigned char b) {
    unsigned char p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        bool hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

struct AesKey {
    int nk, nr;
    unsigned char rk[240];
};

void aesExpandKey(const unsigned char* key, int keyBytes, AesKey& out) {
    out.nk = keyBytes / 4;
    out.nr = out.nk + 6;
    int totalWords = 4 * (out.nr + 1);
    unsigned char* w = out.rk;
    memcpy(w, key, static_cast<size_t>(keyBytes));
    for (int i = out.nk; i < totalWords; i++) {
        unsigned char temp[4];
        memcpy(temp, w + (i - 1) * 4, 4);
        if (i % out.nk == 0) {
            unsigned char t = temp[0];
            temp[0] = kSbox[temp[1]] ^ kRcon[i / out.nk];
            temp[1] = kSbox[temp[2]];
            temp[2] = kSbox[temp[3]];
            temp[3] = kSbox[t];
        } else if (out.nk > 6 && i % out.nk == 4) {
            for (int j = 0; j < 4; j++) temp[j] = kSbox[temp[j]];
        }
        for (int j = 0; j < 4; j++) w[i * 4 + j] = w[(i - out.nk) * 4 + j] ^ temp[j];
    }
}

void addRoundKey(unsigned char* state, const unsigned char* rk) {
    for (int i = 0; i < 16; i++) state[i] ^= rk[i];
}

void subBytes(unsigned char* state) {
    for (int i = 0; i < 16; i++) state[i] = kSbox[state[i]];
}
void invSubBytes(unsigned char* state) {
    for (int i = 0; i < 16; i++) state[i] = kInvSbox[state[i]];
}

void shiftRows(unsigned char* s) {
    unsigned char t;
    t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;
}
void invShiftRows(unsigned char* s) {
    unsigned char t;
    t = s[13];
    s[13] = s[9];
    s[9] = s[5];
    s[5] = s[1];
    s[1] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[3];
    s[3] = s[7];
    s[7] = s[11];
    s[11] = s[15];
    s[15] = t;
}

void mixColumns(unsigned char* s) {
    for (int c = 0; c < 4; c++) {
        unsigned char a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4 + 0] = static_cast<unsigned char>(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        s[c * 4 + 1] = static_cast<unsigned char>(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        s[c * 4 + 2] = static_cast<unsigned char>(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        s[c * 4 + 3] = static_cast<unsigned char>(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}
void invMixColumns(unsigned char* s) {
    for (int c = 0; c < 4; c++) {
        unsigned char a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4 + 0] = static_cast<unsigned char>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        s[c * 4 + 1] = static_cast<unsigned char>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        s[c * 4 + 2] = static_cast<unsigned char>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        s[c * 4 + 3] = static_cast<unsigned char>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void aesEncryptBlock(const AesKey& k, const unsigned char* in, unsigned char* out) {
    unsigned char state[16];
    memcpy(state, in, 16);
    addRoundKey(state, k.rk);
    for (int round = 1; round < k.nr; round++) {
        subBytes(state);
        shiftRows(state);
        mixColumns(state);
        addRoundKey(state, k.rk + round * 16);
    }
    subBytes(state);
    shiftRows(state);
    addRoundKey(state, k.rk + k.nr * 16);
    memcpy(out, state, 16);
}

void aesDecryptBlock(const AesKey& k, const unsigned char* in, unsigned char* out) {
    unsigned char state[16];
    memcpy(state, in, 16);
    addRoundKey(state, k.rk + k.nr * 16);
    for (int round = k.nr - 1; round > 0; round--) {
        invShiftRows(state);
        invSubBytes(state);
        addRoundKey(state, k.rk + round * 16);
        invMixColumns(state);
    }
    invShiftRows(state);
    invSubBytes(state);
    addRoundKey(state, k.rk);
    memcpy(out, state, 16);
}

struct BigUint {
    std::vector<uint32_t> limbs;

    void trim() {
        while (limbs.size() > 1 && limbs.back() == 0) limbs.pop_back();
    }
    bool isZero() const { return limbs.size() == 1 && limbs[0] == 0; }

    static BigUint fromDecimal(const std::string& s) {
        BigUint r;
        r.limbs = {0};
        for (char c : s) {
            if (c < '0' || c > '9') continue;
            r = r.mulSmall(10).addSmall(static_cast<uint32_t>(c - '0'));
        }
        return r;
    }

    std::string toDecimal() const {
        if (isZero()) return "0";
        std::vector<uint32_t> tmp = limbs;
        std::string digits;
        while (!(tmp.size() == 1 && tmp[0] == 0)) {
            uint64_t rem = 0;
            for (size_t i = tmp.size(); i-- > 0;) {
                uint64_t cur = (rem << 32) | tmp[i];
                tmp[i] = static_cast<uint32_t>(cur / 10);
                rem = cur % 10;
            }
            while (tmp.size() > 1 && tmp.back() == 0) tmp.pop_back();
            digits += static_cast<char>('0' + rem);
        }
        std::string out(digits.rbegin(), digits.rend());
        return out;
    }

    BigUint mulSmall(uint32_t m) const {
        BigUint r;
        r.limbs.assign(limbs.size() + 1, 0);
        uint64_t carry = 0;
        for (size_t i = 0; i < limbs.size(); i++) {
            uint64_t cur = static_cast<uint64_t>(limbs[i]) * m + carry;
            r.limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        r.limbs[limbs.size()] = static_cast<uint32_t>(carry);
        r.trim();
        return r;
    }

    BigUint addSmall(uint32_t v) const {
        BigUint r = *this;
        uint64_t carry = v;
        for (size_t i = 0; i < r.limbs.size() && carry; i++) {
            uint64_t cur = static_cast<uint64_t>(r.limbs[i]) + carry;
            r.limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        if (carry) r.limbs.push_back(static_cast<uint32_t>(carry));
        return r;
    }

    static int cmp(const BigUint& a, const BigUint& b) {
        if (a.limbs.size() != b.limbs.size()) return a.limbs.size() < b.limbs.size() ? -1 : 1;
        for (size_t i = a.limbs.size(); i-- > 0;) {
            if (a.limbs[i] != b.limbs[i]) return a.limbs[i] < b.limbs[i] ? -1 : 1;
        }
        return 0;
    }

    static BigUint add(const BigUint& a, const BigUint& b) {
        BigUint r;
        size_t n = std::max(a.limbs.size(), b.limbs.size());
        r.limbs.assign(n + 1, 0);
        uint64_t carry = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
            uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
            uint64_t cur = av + bv + carry;
            r.limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        r.limbs[n] = static_cast<uint32_t>(carry);
        r.trim();
        return r;
    }

    static BigUint sub(const BigUint& a, const BigUint& b) {
        BigUint r;
        r.limbs.assign(a.limbs.size(), 0);
        int64_t borrow = 0;
        for (size_t i = 0; i < a.limbs.size(); i++) {
            int64_t av = a.limbs[i];
            int64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
            int64_t cur = av - bv - borrow;
            if (cur < 0) {
                cur += static_cast<int64_t>(1) << 32;
                borrow = 1;
            } else {
                borrow = 0;
            }
            r.limbs[i] = static_cast<uint32_t>(cur);
        }
        r.trim();
        return r;
    }

    static BigUint mul(const BigUint& a, const BigUint& b) {
        BigUint r;
        r.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
        for (size_t i = 0; i < a.limbs.size(); i++) {
            uint64_t carry = 0;
            for (size_t j = 0; j < b.limbs.size(); j++) {
                uint64_t cur = static_cast<uint64_t>(a.limbs[i]) * b.limbs[j] + r.limbs[i + j] + carry;
                r.limbs[i + j] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
            }
            size_t k = i + b.limbs.size();
            while (carry) {
                uint64_t cur = static_cast<uint64_t>(r.limbs[k]) + carry;
                r.limbs[k] = static_cast<uint32_t>(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
                k++;
            }
        }
        r.trim();
        return r;
    }

    static void shiftLeftBits(BigUint& a, int bits) {
        int words = bits / 32;
        int rem = bits % 32;
        if (words) {
            a.limbs.insert(a.limbs.begin(), static_cast<size_t>(words), 0);
        }
        if (rem) {
            uint32_t carry = 0;
            for (size_t i = static_cast<size_t>(words); i < a.limbs.size(); i++) {
                uint32_t cur = a.limbs[i];
                a.limbs[i] = (cur << rem) | carry;
                carry = cur >> (32 - rem);
            }
            if (carry) a.limbs.push_back(carry);
        }
        a.trim();
    }

    int bitLength() const {
        if (isZero()) return 0;
        int n = static_cast<int>((limbs.size() - 1) * 32);
        uint32_t top = limbs.back();
        while (top) {
            n++;
            top >>= 1;
        }
        return n;
    }

    bool bitAt(int i) const {
        size_t w = static_cast<size_t>(i) / 32;
        if (w >= limbs.size()) return false;
        return (limbs[w] >> (i % 32)) & 1;
    }

    static void divmod(const BigUint& a, const BigUint& b, BigUint& q, BigUint& r) {
        q.limbs.assign(a.limbs.size(), 0);
        r.limbs.assign(1, 0);
        for (int i = a.bitLength() - 1; i >= 0; i--) {
            shiftLeftBits(r, 1);
            if (r.limbs.empty()) r.limbs = {0};
            r.limbs[0] |= (a.bitAt(i) ? 1u : 0u);
            r.trim();
            if (cmp(r, b) >= 0) {
                r = sub(r, b);
                size_t w = static_cast<size_t>(i) / 32;
                if (w >= q.limbs.size()) q.limbs.resize(w + 1, 0);
                q.limbs[w] |= (1u << (i % 32));
            }
        }
        q.trim();
        r.trim();
    }

    static BigUint mod(const BigUint& a, const BigUint& m) {
        BigUint q, r;
        divmod(a, m, q, r);
        return r;
    }

    bool isEven() const { return (limbs[0] & 1) == 0; }

    static BigUint fromU64(uint64_t v) {
        BigUint r;
        r.limbs = {static_cast<uint32_t>(v & 0xFFFFFFFFu), static_cast<uint32_t>(v >> 32)};
        r.trim();
        return r;
    }
};

BigUint modpow(BigUint base, BigUint exp, const BigUint& mod) {
    BigUint result = BigUint::fromU64(1);
    base = BigUint::mod(base, mod);
    while (!exp.isZero()) {
        if (!exp.isEven()) result = BigUint::mod(BigUint::mul(result, base), mod);
        base = BigUint::mod(BigUint::mul(base, base), mod);
        BigUint q, r;
        BigUint two = BigUint::fromU64(2);
        BigUint::divmod(exp, two, q, r);
        exp = q;
    }
    return result;
}

BigUint gcdBig(BigUint a, BigUint b) {
    while (!b.isZero()) {
        BigUint r = BigUint::mod(a, b);
        a = b;
        b = r;
    }
    return a;
}

struct ExtGcdResult {
    BigUint g;
    bool sSign;
    BigUint s;
};

ExtGcdResult extGcd(BigUint a, BigUint b) {
    BigUint oldR = a, r = b;
    BigUint oldS = BigUint::fromU64(1), s = BigUint::fromU64(0);
    bool oldSSign = false, sSign = false;
    while (!r.isZero()) {
        BigUint q, rem;
        BigUint::divmod(oldR, r, q, rem);
        oldR = r;
        r = rem;

        BigUint qs = BigUint::mul(q, s);
        BigUint newS;
        bool newSign;
        if (oldSSign == sSign) {
            if (BigUint::cmp(oldS, qs) >= 0) {
                newS = BigUint::sub(oldS, qs);
                newSign = oldSSign;
            } else {
                newS = BigUint::sub(qs, oldS);
                newSign = !oldSSign;
            }
        } else {
            newS = BigUint::add(oldS, qs);
            newSign = oldSSign;
        }
        if (newS.isZero()) newSign = false;
        oldS = s;
        oldSSign = sSign;
        s = newS;
        sSign = newSign;
    }
    return {oldR, oldSSign, oldS};
}

bool isPrimeBig(const BigUint& n) {
    if (n.limbs.size() == 1 && n.limbs[0] < 2) return false;
    for (uint32_t p : {2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 37u}) {
        BigUint pb = BigUint::fromU64(p);
        if (BigUint::cmp(n, pb) == 0) return true;
        if (BigUint::mod(n, pb).isZero()) return false;
    }
    BigUint d = BigUint::sub(n, BigUint::fromU64(1));
    int r = 0;
    while (d.isEven()) {
        BigUint q2, rem2;
        BigUint::divmod(d, BigUint::fromU64(2), q2, rem2);
        d = q2;
        r++;
    }
    BigUint nMinus1 = BigUint::sub(n, BigUint::fromU64(1));
    for (uint32_t a : {2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 37u}) {
        BigUint ab = BigUint::fromU64(a);
        if (BigUint::cmp(ab, n) >= 0) continue;
        BigUint x = modpow(ab, d, n);
        if (BigUint::cmp(x, BigUint::fromU64(1)) == 0 || BigUint::cmp(x, nMinus1) == 0) continue;
        bool composite = true;
        for (int i = 0; i < r - 1; i++) {
            x = BigUint::mod(BigUint::mul(x, x), n);
            if (BigUint::cmp(x, nMinus1) == 0) {
                composite = false;
                break;
            }
        }
        if (composite) return false;
    }
    return true;
}

NsValue nsString(const std::string& s) {
    NsValue v{};
    v.type = NS_STRING;
    v.str = static_cast<char*>(std::malloc(s.size() ? s.size() : 1));
    if (!s.empty()) std::memcpy(v.str, s.data(), s.size());
    v.str_len = static_cast<int>(s.size());
    return v;
}

// ABI v2: baca argumen string length-aware (boleh mengandung \0)
static std::string toStr(const NsValue& v) {
    if (v.type != NS_STRING || !v.str) return std::string();
    if (v.str_len >= 0) return std::string(v.str, static_cast<size_t>(v.str_len));
    return std::string(v.str);
}

extern "C" int nusa_abi_version() { return 2; }

NsValue nsBool(bool b) {
    NsValue v{};
    v.type = NS_BOOL;
    v.boolean = b ? 1 : 0;
    return v;
}

NsValue nsNumber(double n) {
    NsValue v{};
    v.type = NS_NUMBER;
    v.number = n;
    return v;
}

NsValue nsNull() {
    NsValue v{};
    v.type = NS_NULL;
    return v;
}

NsValue cryptoMd5Hex(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    return nsString(md5Digest(toStr(argv[0])));
}

NsValue cryptoSha1Hex(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    return nsString(sha1Digest(toStr(argv[0])));
}

NsValue cryptoSha512Hex(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    return nsString(sha512Digest(toStr(argv[0])));
}

NsValue cryptoHexEncode(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    return nsString(toHex(toStr(argv[0])));
}

NsValue cryptoHexDecode(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    std::string out;
    if (!fromHex(toStr(argv[0]), out)) return nsNull();
    return nsString(out);
}


static void divmodBigSimple(const BigUint& a, const BigUint& b, BigUint& q, BigUint& r) {
    // b dijamin <= 256 (hanya dipakai feToBytesLE)
    q = BigUint::fromU64(0);
    r = BigUint::fromU64(0);
    // proses digit desimal internal: gunakan toDecimal lalu bagi manual basis-10
    std::string dec = a.toDecimal();
    q = BigUint::fromU64(0);
    unsigned long long rem = 0;
    std::string qs;
    qs.reserve(dec.size());
    for (char ch : dec) {
        rem = rem * 10 + (ch - '0');
        unsigned long long d = rem / 256ULL;
        qs.push_back(static_cast<char>('0' + d)); // d bisa >9! -> pendekatan salah utk d>9
        rem %= 256ULL;
    }
    // pendekatan digit-per-digit gagal utk basis 256; pakai mulSmall-add rekonstruksi:
    // Kita ganti strategi: ekstrak byte lewat pengurangan berulang berbasis limb.
    (void)qs;
    // Implementasi aman: konversi lewat string desimal -> uint64 chunks
    // (cukup utk nilai < 2^255 karena kita hanya ambil sisa modulo 256 berulang)
    q = BigUint::fromU64(0);
    r = BigUint::fromU64(0);
    BigUint cur = a;
    BigUint B256 = BigUint::fromU64(256);
    for (int i = 0; i < 32; i++) {
        BigUint rr = BigUint::mod(cur, B256);
        r = rr;
        std::string dec2 = BigUint::sub(cur, rr).toDecimal();
        // bagi desimal-string dengan 256
        std::string quo;
        unsigned long long remv = 0;
        for (char ch : dec2) {
            remv = remv * 10ULL + static_cast<unsigned long long>(ch - '0');
            unsigned long long dd = remv / 256ULL;
            quo.push_back(static_cast<char>('0' + dd));
            remv -= dd * 256ULL;
        }
        size_t pos = quo.find_first_not_of('0');
        cur = (pos == std::string::npos) ? BigUint::fromU64(0) : BigUint::fromDecimal(quo.substr(pos));
    }
}

// ===== X25519 (RFC 7748) via BigUint ladder =====
static const char* X25519_P_DEC = "57896044618658097711785492504343953926634992332820282019728792003956564819949";

static std::string feToBytesLE(const BigUint& v) {
    std::string out;
    out.reserve(32);
    BigUint cur = v;
    BigUint B256 = BigUint::fromU64(256);
    for (int i = 0; i < 32; i++) {
        BigUint rr = BigUint::mod(cur, B256);
        out.push_back(static_cast<char>(std::stoul(rr.toDecimal())));
        // kurangi rr lalu bagi persis 256 via pembagian string desimal
        std::string dec2 = BigUint::sub(cur, rr).toDecimal();
        std::string quo;
        unsigned long long remv = 0;
        for (char ch : dec2) {
            remv = remv * 10ULL + (unsigned long long)(ch - '0');
            unsigned long long dd = remv / 256ULL;
            quo.push_back((char)('0' + dd));
            remv -= dd * 256ULL;
        }
        size_t pos = quo.find_first_not_of('0');
        cur = (pos == std::string::npos) ? BigUint::fromU64(0) : BigUint::fromDecimal(quo.substr(pos));
    }
    return out;
}

static BigUint feFromBytesLE(const unsigned char* b) {
    BigUint v = BigUint::fromU64(0);
    BigUint m256 = BigUint::fromU64(256);
    for (int i = 31; i >= 0; i--) {
        v = BigUint::mul(v, m256);
        v = BigUint::add(v, BigUint::fromU64(b[i]));
    }
    return v;
}

// fallback sederhana utk pembagian kecil (b <= 2^32): kurangi bergeser
static BigUint feAdd(const BigUint& a, const BigUint& b) {
    return BigUint::mod(BigUint::add(a, b), BigUint::fromDecimal(X25519_P_DEC));
}
static BigUint feSub(const BigUint& a, const BigUint& b) {
    BigUint p = BigUint::fromDecimal(X25519_P_DEC);
    if (BigUint::cmp(a, b) >= 0) return BigUint::mod(BigUint::sub(a, b), p);
    return BigUint::mod(BigUint::sub(BigUint::add(a, p), b), p);
}
static BigUint feMul(const BigUint& a, const BigUint& b) {
    return BigUint::mod(BigUint::mul(a, b), BigUint::fromDecimal(X25519_P_DEC));
}

NsValue cryptoX25519(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    std::string privHex, pubHex;
    if (!fromHex(toStr(argv[0]), privHex) || privHex.size() != 32) return nsNull();
    if (!fromHex(toStr(argv[1]), pubHex) || pubHex.size() != 32) return nsNull();

    unsigned char k[32];
    memcpy(k, privHex.data(), 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;

    unsigned char ub[32];
    memcpy(ub, pubHex.data(), 32);
    ub[31] &= 127; // mask bit tertinggi (paritas RFC)

    BigUint x1 = feFromBytesLE(ub);
    BigUint p = BigUint::fromDecimal(X25519_P_DEC);

    BigUint x2 = BigUint::fromU64(1), z2 = BigUint::fromU64(0);
    BigUint x3 = x1, z3 = BigUint::fromU64(1);
    BigUint a24 = BigUint::fromU64(121665);
    int swap = 0;

    for (int t = 254; t >= 0; t--) {
        int kt = (k[t / 8] >> (t % 8)) & 1;
        swap ^= kt;
        if (swap) { std::swap(x2, x3); std::swap(z2, z3); }
        swap = kt;

        BigUint A  = feAdd(x2, z2);
        BigUint AA = feMul(A, A);
        BigUint B  = feSub(x2, z2);
        BigUint BB = feMul(B, B);
        BigUint E  = feSub(AA, BB);
        BigUint C  = feAdd(x3, z3);
        BigUint D  = feSub(x3, z3);
        BigUint DA = feMul(D, A);
        BigUint CB = feMul(C, B);

        x3 = feMul(feAdd(DA, CB), feAdd(DA, CB));
        z3 = feMul(x1, feMul(feSub(DA, CB), feSub(DA, CB)));
        x2 = feMul(AA, BB);
        z2 = feMul(E, feAdd(AA, feMul(a24, E)));
    }
    if (swap) { std::swap(x2, x3); std::swap(z2, z3); }

    BigUint inv = modpow(z2, BigUint::sub(p, BigUint::fromU64(2)), p);
    BigUint res = feMul(x2, inv);
    std::string out = feToBytesLE(res);
    return nsString(toHex(out));
}

NsValue cryptoXor(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    std::string data, key;
    if (!fromHex(toStr(argv[0]), data) || !fromHex(toStr(argv[1]), key) || key.empty()) return nsNull();
    std::string out(data.size(), '\0');
    for (size_t i = 0; i < data.size(); i++) out[i] = static_cast<char>(data[i] ^ key[i % key.size()]);
    return nsString(toHex(out));
}

bool applyPkcs7(std::string& data, bool pad) {
    if (pad) {
        size_t padLen = 16 - (data.size() % 16);
        data.append(padLen, static_cast<char>(padLen));
        return true;
    }
    if (data.empty() || data.size() % 16 != 0) return false;
    unsigned char padLen = static_cast<unsigned char>(data.back());
    if (padLen == 0 || padLen > 16 || padLen > data.size()) return false;
    for (size_t i = data.size() - padLen; i < data.size(); i++) {
        if (static_cast<unsigned char>(data[i]) != padLen) return false;
    }
    data.resize(data.size() - padLen);
    return true;
}

NsValue cryptoAes(int argc, const NsValue* argv, bool encrypt) {
    if (argc != 4 || argv[0].type != NS_STRING || argv[1].type != NS_STRING || argv[2].type != NS_STRING ||
        argv[3].type != NS_STRING) {
        return nsNull();
    }
    std::string data, key, iv;
    if (!fromHex(toStr(argv[0]), data) || !fromHex(toStr(argv[1]), key)) return nsNull();
    std::string mode = toStr(argv[2]);
    if (!fromHex(toStr(argv[3]), iv)) return nsNull();
    if (key.size() != 16 && key.size() != 32) return nsNull();
    if ((mode == "cbc" || mode == "ctr") && iv.size() != 16) return nsNull();

    AesKey k;
    aesExpandKey(reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()), k);

    if (mode == "ecb") {
        if (encrypt) {
            if (!applyPkcs7(data, true)) return nsNull();
        } else if (data.empty() || data.size() % 16 != 0) {
            return nsNull();
        }
        std::string out(data.size(), '\0');
        for (size_t i = 0; i < data.size(); i += 16) {
            if (encrypt)
                aesEncryptBlock(k, reinterpret_cast<const unsigned char*>(data.data() + i),
                                 reinterpret_cast<unsigned char*>(&out[i]));
            else
                aesDecryptBlock(k, reinterpret_cast<const unsigned char*>(data.data() + i),
                                 reinterpret_cast<unsigned char*>(&out[i]));
        }
        if (!encrypt && !applyPkcs7(out, false)) return nsNull();
        return nsString(toHex(out));
    }
    if (mode == "cbc") {
        if (encrypt) {
            if (!applyPkcs7(data, true)) return nsNull();
            std::string out(data.size(), '\0');
            unsigned char prev[16];
            memcpy(prev, iv.data(), 16);
            for (size_t i = 0; i < data.size(); i += 16) {
                unsigned char block[16];
                for (int j = 0; j < 16; j++) block[j] = static_cast<unsigned char>(data[i + j]) ^ prev[j];
                aesEncryptBlock(k, block, reinterpret_cast<unsigned char*>(&out[i]));
                memcpy(prev, &out[i], 16);
            }
            return nsString(toHex(out));
        } else {
            if (data.empty() || data.size() % 16 != 0) return nsNull();
            std::string out(data.size(), '\0');
            unsigned char prev[16];
            memcpy(prev, iv.data(), 16);
            for (size_t i = 0; i < data.size(); i += 16) {
                unsigned char dec[16];
                aesDecryptBlock(k, reinterpret_cast<const unsigned char*>(data.data() + i), dec);
                for (int j = 0; j < 16; j++)
                    out[i + j] = static_cast<char>(dec[j] ^ static_cast<unsigned char>(prev[j]));
                memcpy(prev, data.data() + i, 16);
            }
            if (!applyPkcs7(out, false)) return nsNull();
            return nsString(toHex(out));
        }
    }
    if (mode == "ctr") {
        std::string out(data.size(), '\0');
        unsigned char counter[16];
        memcpy(counter, iv.data(), 16);
        for (size_t i = 0; i < data.size(); i += 16) {
            unsigned char keystream[16];
            aesEncryptBlock(k, counter, keystream);
            size_t n = std::min<size_t>(16, data.size() - i);
            for (size_t j = 0; j < n; j++)
                out[i + j] = static_cast<char>(static_cast<unsigned char>(data[i + j]) ^ keystream[j]);
            for (int j = 15; j >= 0; j--) {
                if (++counter[j] != 0) break;
            }
        }
        return nsString(toHex(out));
    }
    return nsNull();
}


// ---- GCM (GHASH + CTR) -----------------------------------------------
// GF(2^128) carry-less multiply, dikurangi dengan polinomial reduksi
// AES-GCM (x^128 + x^7 + x^2 + x + 1). Standar "shift-and-xor" style,
// dieksekusi native jadi jauh lebih cepat daripada versi .ns yang
// mengulang 128 iterasi per blok lewat interpreter (payload 50 KB
// butuh ~19 detik interpreted vs beberapa milidetik native).
static void gcmMul(const unsigned char x[16], const unsigned char y[16], unsigned char out[16]) {
    unsigned char z[16] = {0};
    unsigned char v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        int byteIdx = i / 8;
        int bitIdx = 7 - (i % 8);
        if ((x[byteIdx] >> bitIdx) & 1) {
            for (int b = 0; b < 16; b++) z[b] ^= v[b];
        }
        unsigned char lsb = v[15] & 1;
        for (int b = 15; b > 0; b--) v[b] = (v[b] >> 1) | ((v[b - 1] & 1) << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(out, z, 16);
}

static void gcmGhash(const unsigned char h[16], const std::string& aad, const std::string& ct, unsigned char out[16]) {
    unsigned char y[16] = {0};
    auto absorb = [&](const unsigned char* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            unsigned char block[16] = {0};
            size_t n = std::min<size_t>(16, len - off);
            memcpy(block, data + off, n);
            for (int b = 0; b < 16; b++) y[b] ^= block[b];
            unsigned char tmp[16];
            gcmMul(y, h, tmp);
            memcpy(y, tmp, 16);
            off += n;
        }
    };
    if (!aad.empty()) absorb(reinterpret_cast<const unsigned char*>(aad.data()), aad.size());
    if (!ct.empty()) absorb(reinterpret_cast<const unsigned char*>(ct.data()), ct.size());
    unsigned char lenBlock[16] = {0};
    uint64_t aadBits = static_cast<uint64_t>(aad.size()) * 8;
    uint64_t ctBits = static_cast<uint64_t>(ct.size()) * 8;
    for (int i = 0; i < 8; i++) lenBlock[7 - i] = static_cast<unsigned char>(aadBits >> (i * 8));
    for (int i = 0; i < 8; i++) lenBlock[15 - i] = static_cast<unsigned char>(ctBits >> (i * 8));
    for (int b = 0; b < 16; b++) y[b] ^= lenBlock[b];
    unsigned char tmp[16];
    gcmMul(y, h, tmp);
    memcpy(out, tmp, 16);
}

static void gcmCtrXor(const AesKey& k, const unsigned char j0[16], const std::string& in, std::string& out) {
    out.resize(in.size());
    unsigned char counter[16];
    memcpy(counter, j0, 16);
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
    size_t off = 0;
    while (off < in.size()) {
        unsigned char ks[16];
        aesEncryptBlock(k, counter, ks);
        size_t n = std::min<size_t>(16, in.size() - off);
        for (size_t j = 0; j < n; j++) out[off + j] = static_cast<char>(static_cast<unsigned char>(in[off + j]) ^ ks[j]);
        for (int i = 15; i >= 12; i--) {
            if (++counter[i] != 0) break;
        }
        off += n;
    }
}

static bool gcmCore(const std::string& key, const std::string& iv, const std::string& aad,
                     const std::string& inData, bool encrypt, std::string& outData, unsigned char tagOut[16]) {
    if (key.size() != 16 && key.size() != 32) return false;
    if (iv.size() != 12) return false;

    AesKey k;
    aesExpandKey(reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()), k);

    unsigned char h[16] = {0};
    unsigned char zero[16] = {0};
    aesEncryptBlock(k, zero, h);

    unsigned char j0[16] = {0};
    memcpy(j0, iv.data(), 12);
    j0[15] = 1;

    gcmCtrXor(k, j0, inData, outData);

    const std::string& ctForTag = encrypt ? outData : inData;
    unsigned char s[16];
    gcmGhash(h, aad, ctForTag, s);

    unsigned char ej0[16];
    aesEncryptBlock(k, j0, ej0);
    for (int i = 0; i < 16; i++) tagOut[i] = ej0[i] ^ s[i];
    return true;
}

NsValue cryptoGcmEnkripsi(int argc, const NsValue* argv) {
    if (argc != 4 || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_STRING || argv[3].type != NS_STRING) {
        return nsNull();
    }
    std::string key, iv, aad, pt;
    if (!fromHex(toStr(argv[0]), key) || !fromHex(toStr(argv[1]), iv)) return nsNull();
    if (!fromHex(toStr(argv[2]), pt)) return nsNull();
    aad = toStr(argv[3]);  // AAD dilewatkan mentah (bukan hex) -- kosong pada Noise
    std::string ct;
    unsigned char tag[16];
    if (!gcmCore(key, iv, aad, pt, true, ct, tag)) return nsNull();
    std::string tagHex = toHex(std::string(reinterpret_cast<char*>(tag), 16));
    return nsString(toHex(ct) + "|" + tagHex);
}

NsValue cryptoGcmDekripsi(int argc, const NsValue* argv) {
    if (argc != 5 || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_STRING || argv[3].type != NS_STRING || argv[4].type != NS_STRING) {
        return nsNull();
    }
    std::string key, iv, ct, tagWant;
    if (!fromHex(toStr(argv[0]), key) || !fromHex(toStr(argv[1]), iv)) return nsNull();
    if (!fromHex(toStr(argv[2]), ct)) return nsNull();
    std::string aad = toStr(argv[3]);
    if (!fromHex(toStr(argv[4]), tagWant) || tagWant.size() != 16) return nsNull();

    std::string pt;
    unsigned char tag[16];
    if (!gcmCore(key, iv, aad, ct, false, pt, tag)) return nsNull();

    unsigned char diff = 0;
    for (int i = 0; i < 16; i++) diff |= tag[i] ^ static_cast<unsigned char>(tagWant[i]);
    if (diff != 0) return nsNull();
    return nsString(toHex(pt));
}

NsValue cryptoAesEnkripsi(int argc, const NsValue* argv) { return cryptoAes(argc, argv, true); }
NsValue cryptoAesDekripsi(int argc, const NsValue* argv) { return cryptoAes(argc, argv, false); }

NsValue cryptoModpow(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_STRING || argv[1].type != NS_STRING || argv[2].type != NS_STRING) {
        return nsNull();
    }
    BigUint base = BigUint::fromDecimal(toStr(argv[0]));
    BigUint exp = BigUint::fromDecimal(toStr(argv[1]));
    BigUint mod = BigUint::fromDecimal(toStr(argv[2]));
    if (mod.isZero()) return nsNull();
    return nsString(modpow(base, exp, mod).toDecimal());
}

NsValue cryptoGcd(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    return nsString(gcdBig(BigUint::fromDecimal(toStr(argv[0])), BigUint::fromDecimal(toStr(argv[1]))).toDecimal());
}

NsValue cryptoModInverse(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    BigUint a = BigUint::fromDecimal(toStr(argv[0]));
    BigUint m = BigUint::fromDecimal(toStr(argv[1]));
    if (m.isZero()) return nsNull();
    ExtGcdResult res = extGcd(BigUint::mod(a, m), m);
    if (BigUint::cmp(res.g, BigUint::fromU64(1)) != 0) return nsNull();
    BigUint result = res.sSign ? BigUint::sub(m, BigUint::mod(res.s, m)) : BigUint::mod(res.s, m);
    if (result.isZero() && !res.sSign) result = BigUint::mod(res.s, m);
    return nsString(result.toDecimal());
}

NsValue cryptoAdalahPrima(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    return nsBool(isPrimeBig(BigUint::fromDecimal(toStr(argv[0]))));
}

NsValue cryptoPackU32(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_STRING) return nsNull();
    uint32_t n = static_cast<uint32_t>(static_cast<uint64_t>(argv[0].number));
    std::string endian = toStr(argv[1]);
    unsigned char b[4];
    if (endian == "le") {
        for (int i = 0; i < 4; i++) b[i] = static_cast<unsigned char>((n >> (i * 8)) & 0xFF);
    } else if (endian == "be") {
        for (int i = 0; i < 4; i++) b[i] = static_cast<unsigned char>((n >> ((3 - i) * 8)) & 0xFF);
    } else {
        return nsNull();
    }
    return nsString(toHex(b, 4));
}

NsValue cryptoUnpackU32(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    std::string raw;
    if (!fromHex(toStr(argv[0]), raw) || raw.size() != 4) return nsNull();
    std::string endian = toStr(argv[1]);
    uint32_t n = 0;
    if (endian == "le") {
        for (int i = 3; i >= 0; i--) n = (n << 8) | static_cast<uint8_t>(raw[i]);
    } else if (endian == "be") {
        for (int i = 0; i < 4; i++) n = (n << 8) | static_cast<uint8_t>(raw[i]);
    } else {
        return nsNull();
    }
    return nsNumber(static_cast<double>(n));
}

NsValue cryptoPackU64(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    BigUint b = BigUint::fromDecimal(toStr(argv[0]));
    uint64_t v = 0;
    if (b.limbs.size() >= 1) v = b.limbs[0];
    if (b.limbs.size() >= 2) v |= (static_cast<uint64_t>(b.limbs[1]) << 32);
    std::string endian = toStr(argv[1]);
    unsigned char bytes[8];
    if (endian == "le") {
        for (int i = 0; i < 8; i++) bytes[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
    } else if (endian == "be") {
        for (int i = 0; i < 8; i++) bytes[i] = static_cast<unsigned char>((v >> ((7 - i) * 8)) & 0xFF);
    } else {
        return nsNull();
    }
    return nsString(toHex(bytes, 8));
}

NsValue cryptoUnpackU64(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    std::string raw;
    if (!fromHex(toStr(argv[0]), raw) || raw.size() != 8) return nsNull();
    std::string endian = toStr(argv[1]);
    uint64_t n = 0;
    if (endian == "le") {
        for (int i = 7; i >= 0; i--) n = (n << 8) | static_cast<uint8_t>(raw[i]);
    } else if (endian == "be") {
        for (int i = 0; i < 8; i++) n = (n << 8) | static_cast<uint8_t>(raw[i]);
    } else {
        return nsNull();
    }
    BigUint b = BigUint::fromU64(n);
    return nsString(b.toDecimal());
}

NsValue cryptoAcakBytesHex(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_NUMBER) return nsNull();
    int n = static_cast<int>(argv[0].number);
    if (n < 0 || n > 1 << 20) return nsNull();
    std::vector<unsigned char> buf(static_cast<size_t>(n));
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (!f) return nsNull();
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    return nsString(toHex(buf.data(), buf.size()));
}

NsValue cryptoPbkdf2Sha256(int argc, const NsValue* argv) {
    if (argc != 4 || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_NUMBER || argv[3].type != NS_NUMBER)
        return nsNull();

    std::string pass = toStr(argv[0]);
    std::string salt = toStr(argv[1]);
    int iterations = static_cast<int>(argv[2].number);
    int keyLen = static_cast<int>(argv[3].number);
    if (iterations <= 0 || keyLen <= 0 || keyLen > 1024) return nsNull();

    std::string out;
    out.reserve(keyLen);

    uint32_t blockIndex = 1;
    while ((int)out.size() < keyLen) {
        std::string saltWithIndex = salt;
        saltWithIndex.push_back((char)((blockIndex >> 24) & 0xFF));
        saltWithIndex.push_back((char)((blockIndex >> 16) & 0xFF));
        saltWithIndex.push_back((char)((blockIndex >> 8) & 0xFF));
        saltWithIndex.push_back((char)(blockIndex & 0xFF));

        auto u = hmac::sha256(pass, saltWithIndex);
        std::array<uint8_t, 32> t = u;

        for (int iter = 1; iter < iterations; iter++) {
            std::string uStr(reinterpret_cast<const char*>(u.data()), 32);
            u = hmac::sha256(pass, uStr);
            for (size_t i = 0; i < 32; i++) {
                t[i] ^= u[i];
            }
        }

        size_t toCopy = std::min((size_t)(keyLen - out.size()), (size_t)32);
        out.append(reinterpret_cast<const char*>(t.data()), toCopy);
        blockIndex++;
    }

    return nsString(toHex(reinterpret_cast<const unsigned char*>(out.data()), out.size()));
}

NsValue cryptoUrlEncode(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    static const char* hex = "0123456789ABCDEF";
    std::string in = toStr(argv[0]);
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return nsString(out);
}

NsValue cryptoUrlDecode(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    std::string in = toStr(argv[0]);
    std::string out;
    out.reserve(in.size());
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return nsNull();
            int hi = v(in[i + 1]), lo = v(in[i + 2]);
            if (hi < 0 || lo < 0) return nsNull();
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return nsString(out);
}

uint32_t crc32Update(uint32_t crc, const unsigned char* data, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

NsValue cryptoCrc32(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    std::string in = toStr(argv[0]);
    uint32_t crc = crc32Update(0, reinterpret_cast<const unsigned char*>(in.data()), in.size());
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    return nsString(std::string(buf));
}

}  // namespace

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "md5_hex", cryptoMd5Hex);
    reg(registry, "sha1_hex", cryptoSha1Hex);
    reg(registry, "sha512_hex", cryptoSha512Hex);
    reg(registry, "hex_encode", cryptoHexEncode);
    reg(registry, "hex_decode", cryptoHexDecode);
    reg(registry, "xor_cipher", cryptoXor);
    reg(registry, "aes_enkripsi", cryptoAesEnkripsi);
    reg(registry, "aes_dekripsi", cryptoAesDekripsi);
    reg(registry, "gcm_enkripsi", cryptoGcmEnkripsi);
    reg(registry, "gcm_dekripsi", cryptoGcmDekripsi);
    reg(registry, "modpow", cryptoModpow);
    reg(registry, "gcd", cryptoGcd);
    reg(registry, "mod_inverse", cryptoModInverse);
    reg(registry, "adalah_prima", cryptoAdalahPrima);
    reg(registry, "pack_u32", cryptoPackU32);
    reg(registry, "unpack_u32", cryptoUnpackU32);
    reg(registry, "pack_u64", cryptoPackU64);
    reg(registry, "unpack_u64", cryptoUnpackU64);
    reg(registry, "acak_bytes_hex", cryptoAcakBytesHex);
    reg(registry, "x25519", cryptoX25519);
    reg(registry, "pbkdf2_sha256", cryptoPbkdf2Sha256);
    reg(registry, "url_encode", cryptoUrlEncode);
    reg(registry, "url_decode", cryptoUrlDecode);
    reg(registry, "crc32", cryptoCrc32);
}
