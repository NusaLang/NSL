// AEAD ciphers, X25519, ECDSA and RSA verification for the built-in TLS client.
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

#include "tls_crypto.hpp"

namespace tls {
namespace crypto {

namespace {

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void putLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Constant-time equality for tags.
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// ---------------------------------------------------------------- AES ----
struct AesTables {
    uint8_t sbox[256];
    AesTables() {
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl8 = [](uint8_t x, int s) { return static_cast<uint8_t>((x << s) | (x >> (8 - s))); };
            sbox[p] = static_cast<uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
    }
};
const AesTables& aesTables() {
    static const AesTables t;
    return t;
}

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

struct Aes {
    uint8_t rk[15][16];
    int rounds;
    explicit Aes(const Bytes& key) {
        if (key.size() != 16 && key.size() != 32) throw std::runtime_error("AES: unsupported key size");
        const auto& sb = aesTables().sbox;
        int nk = static_cast<int>(key.size() / 4);
        rounds = nk + 6;
        int total = 4 * (rounds + 1);
        std::vector<uint32_t> w(total);
        for (int i = 0; i < nk; i++)
            w[i] = (uint32_t(key[4 * i]) << 24) | (uint32_t(key[4 * i + 1]) << 16) | (uint32_t(key[4 * i + 2]) << 8) | key[4 * i + 3];
        uint8_t rcon = 1;
        auto subWord = [&](uint32_t x) {
            return (uint32_t(sb[x >> 24]) << 24) | (uint32_t(sb[(x >> 16) & 0xff]) << 16) |
                   (uint32_t(sb[(x >> 8) & 0xff]) << 8) | sb[x & 0xff];
        };
        for (int i = nk; i < total; i++) {
            uint32_t t = w[i - 1];
            if (i % nk == 0) {
                t = subWord((t << 8) | (t >> 24)) ^ (uint32_t(rcon) << 24);
                rcon = xtime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                t = subWord(t);
            }
            w[i] = w[i - nk] ^ t;
        }
        for (int r = 0; r <= rounds; r++)
            for (int c = 0; c < 4; c++)
                for (int b = 0; b < 4; b++) rk[r][4 * c + b] = static_cast<uint8_t>(w[4 * r + c] >> (24 - 8 * b));
    }
    void encrypt(const uint8_t in[16], uint8_t out[16]) const {
        const auto& sb = aesTables().sbox;
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[0][i];
        for (int r = 1; r <= rounds; r++) {
            uint8_t t[16];
            for (int i = 0; i < 16; i++) t[i] = sb[s[i]];
            // ShiftRows (state is column-major: byte index = 4*col + row)
            uint8_t u[16];
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) u[4 * c + row] = t[4 * ((c + row) % 4) + row];
            if (r != rounds) {
                for (int c = 0; c < 4; c++) {
                    uint8_t a0 = u[4 * c], a1 = u[4 * c + 1], a2 = u[4 * c + 2], a3 = u[4 * c + 3];
                    s[4 * c] = static_cast<uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                    s[4 * c + 1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                    s[4 * c + 2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                    s[4 * c + 3] = static_cast<uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
                }
            } else {
                std::memcpy(s, u, 16);
            }
            for (int i = 0; i < 16; i++) s[i] ^= rk[r][i];
        }
        std::memcpy(out, s, 16);
    }
};

// GHASH multiplication in GF(2^128), bit-serial (no secret-dependent table lookups).
struct U128 {
    uint64_t hi = 0, lo = 0;
};
U128 gfMul(U128 x, U128 h) {
    U128 z, v = h;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = (i < 64) ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        uint64_t mask = 0 - bit;
        z.hi ^= v.hi & mask;
        z.lo ^= v.lo & mask;
        uint64_t carry = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        v.hi ^= (0xe100000000000000ULL) & (0 - carry);
    }
    return z;
}
U128 load128(const uint8_t* p) {
    U128 r;
    for (int i = 0; i < 8; i++) r.hi = (r.hi << 8) | p[i];
    for (int i = 8; i < 16; i++) r.lo = (r.lo << 8) | p[i];
    return r;
}
void store128(U128 v, uint8_t* p) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v.hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) p[8 + i] = static_cast<uint8_t>(v.lo >> (56 - 8 * i));
}

void ghashUpdate(U128& y, U128 h, const uint8_t* data, size_t len) {
    while (len > 0) {
        uint8_t block[16] = {0};
        size_t n = std::min<size_t>(16, len);
        std::memcpy(block, data, n);
        U128 b = load128(block);
        y.hi ^= b.hi;
        y.lo ^= b.lo;
        y = gfMul(y, h);
        data += n;
        len -= n;
    }
}

void gcmCtr(const Aes& aes, const uint8_t j0[16], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    for (size_t off = 0; off < len; off += 16) {
        for (int i = 15; i >= 12; i--) {
            if (++counter[i] != 0) break;
        }
        uint8_t ks[16];
        aes.encrypt(counter, ks);
        size_t n = std::min<size_t>(16, len - off);
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

void gcmTag(const Aes& aes, const uint8_t j0[16], const Bytes& aad, const uint8_t* ct, size_t ctLen, uint8_t tag[16]) {
    uint8_t zero[16] = {0}, hb[16];
    aes.encrypt(zero, hb);
    U128 h = load128(hb), y;
    ghashUpdate(y, h, aad.data(), aad.size());
    ghashUpdate(y, h, ct, ctLen);
    uint8_t lens[16];
    uint64_t aadBits = uint64_t(aad.size()) * 8, ctBits = uint64_t(ctLen) * 8;
    for (int i = 0; i < 8; i++) lens[i] = static_cast<uint8_t>(aadBits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = static_cast<uint8_t>(ctBits >> (56 - 8 * i));
    ghashUpdate(y, h, lens, 16);
    uint8_t s[16], ek[16];
    store128(y, s);
    aes.encrypt(j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ek[i];
}

// ------------------------------------------------------------- ChaCha ----
void chachaBlock(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    uint32_t x[16];
    std::memcpy(x, s, sizeof x);
    auto qr = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);
    };
    for (int i = 0; i < 10; i++) {
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) putLe32(out + 4 * i, x[i] + s[i]);
}

void chachaXor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, size_t len, uint8_t* out) {
    for (size_t off = 0; off < len; off += 64, counter++) {
        uint8_t ks[64];
        chachaBlock(key, counter, nonce, ks);
        size_t n = std::min<size_t>(64, len - off);
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

// Poly1305 (26-bit limbs, after the public-domain "donna" layout).
struct Poly1305 {
    uint32_t r[5], h[5] = {0, 0, 0, 0, 0}, pad[4];
    uint8_t buf[16];
    size_t bufLen = 0;
    explicit Poly1305(const uint8_t key[32]) {
        r[0] = le32(key) & 0x3ffffff;
        r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
        r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
        r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
        r[4] = (le32(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; i++) pad[i] = le32(key + 16 + 4 * i);
    }
    void block(const uint8_t* m, uint32_t hibit) {
        uint32_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
        uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
        h0 += le32(m) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 + uint64_t(h3) * s2 + uint64_t(h4) * s1;
        uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 + uint64_t(h3) * s3 + uint64_t(h4) * s2;
        uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 + uint64_t(h3) * s4 + uint64_t(h4) * s3;
        uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 + uint64_t(h3) * r0 + uint64_t(h4) * s4;
        uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 + uint64_t(h3) * r1 + uint64_t(h4) * r0;
        uint32_t c;
        c = static_cast<uint32_t>(d0 >> 26); h0 = static_cast<uint32_t>(d0) & 0x3ffffff;
        d1 += c; c = static_cast<uint32_t>(d1 >> 26); h1 = static_cast<uint32_t>(d1) & 0x3ffffff;
        d2 += c; c = static_cast<uint32_t>(d2 >> 26); h2 = static_cast<uint32_t>(d2) & 0x3ffffff;
        d3 += c; c = static_cast<uint32_t>(d3 >> 26); h3 = static_cast<uint32_t>(d3) & 0x3ffffff;
        d4 += c; c = static_cast<uint32_t>(d4 >> 26); h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    }
    void update(const uint8_t* m, size_t len) {
        while (len > 0) {
            size_t take = std::min<size_t>(16 - bufLen, len);
            std::memcpy(buf + bufLen, m, take);
            bufLen += take; m += take; len -= take;
            if (bufLen == 16) {
                block(buf, 1u << 24);
                bufLen = 0;
            }
        }
    }
    void finish(uint8_t tag[16]) {
        if (bufLen) {
            buf[bufLen] = 1;
            for (size_t i = bufLen + 1; i < 16; i++) buf[i] = 0;
            block(buf, 0);
        }
        uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
        c = h1 >> 26; h1 &= 0x3ffffff;
        h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
        h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
        h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        uint32_t g4 = h4 + c - (1u << 26);
        uint32_t mask = (g4 >> 31) - 1;  // all ones if h >= p
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
        uint32_t t0 = h0 | (h1 << 26);
        uint32_t t1 = (h1 >> 6) | (h2 << 20);
        uint32_t t2 = (h2 >> 12) | (h3 << 14);
        uint32_t t3 = (h3 >> 18) | (h4 << 8);
        uint64_t f;
        f = uint64_t(t0) + pad[0]; putLe32(tag, static_cast<uint32_t>(f));
        f = uint64_t(t1) + pad[1] + (f >> 32); putLe32(tag + 4, static_cast<uint32_t>(f));
        f = uint64_t(t2) + pad[2] + (f >> 32); putLe32(tag + 8, static_cast<uint32_t>(f));
        f = uint64_t(t3) + pad[3] + (f >> 32); putLe32(tag + 12, static_cast<uint32_t>(f));
    }
};

void chachaPolyTag(const uint8_t key[32], const uint8_t nonce[12], const Bytes& aad, const uint8_t* ct, size_t ctLen, uint8_t tag[16]) {
    uint8_t otk[64];
    chachaBlock(key, 0, nonce, otk);
    Poly1305 p(otk);
    static const uint8_t zeros[16] = {0};
    p.update(aad.data(), aad.size());
    if (aad.size() % 16) p.update(zeros, 16 - aad.size() % 16);
    p.update(ct, ctLen);
    if (ctLen % 16) p.update(zeros, 16 - ctLen % 16);
    uint8_t lens[16];
    uint64_t a = aad.size(), c = ctLen;
    for (int i = 0; i < 8; i++) lens[i] = static_cast<uint8_t>(a >> (8 * i));
    for (int i = 0; i < 8; i++) lens[8 + i] = static_cast<uint8_t>(c >> (8 * i));
    p.update(lens, 16);
    p.finish(tag);
}

}  // namespace

Bytes aesGcmSeal(const Bytes& key, const Bytes& nonce, const Bytes& aad, const Bytes& plain) {
    if (nonce.size() != 12) throw std::runtime_error("GCM: nonce must be 12 bytes");
    Aes aes(key);
    uint8_t j0[16];
    std::memcpy(j0, nonce.data(), 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    Bytes out(plain.size() + 16);
    gcmCtr(aes, j0, plain.data(), plain.size(), out.data());
    gcmTag(aes, j0, aad, out.data(), plain.size(), out.data() + plain.size());
    return out;
}

bool aesGcmOpen(const Bytes& key, const Bytes& nonce, const Bytes& aad, const Bytes& ct, Bytes& plain) {
    if (nonce.size() != 12 || ct.size() < 16) return false;
    Aes aes(key);
    uint8_t j0[16];
    std::memcpy(j0, nonce.data(), 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    size_t n = ct.size() - 16;
    uint8_t tag[16];
    gcmTag(aes, j0, aad, ct.data(), n, tag);
    if (!ctEqual(tag, ct.data() + n, 16)) return false;
    plain.resize(n);
    gcmCtr(aes, j0, ct.data(), n, plain.data());
    return true;
}

Bytes chachaSeal(const Bytes& key, const Bytes& nonce, const Bytes& aad, const Bytes& plain) {
    if (key.size() != 32 || nonce.size() != 12) throw std::runtime_error("ChaCha20: bad key/nonce size");
    Bytes out(plain.size() + 16);
    chachaXor(key.data(), 1, nonce.data(), plain.data(), plain.size(), out.data());
    chachaPolyTag(key.data(), nonce.data(), aad, out.data(), plain.size(), out.data() + plain.size());
    return out;
}

bool chachaOpen(const Bytes& key, const Bytes& nonce, const Bytes& aad, const Bytes& ct, Bytes& plain) {
    if (key.size() != 32 || nonce.size() != 12 || ct.size() < 16) return false;
    size_t n = ct.size() - 16;
    uint8_t tag[16];
    chachaPolyTag(key.data(), nonce.data(), aad, ct.data(), n, tag);
    if (!ctEqual(tag, ct.data() + n, 16)) return false;
    plain.resize(n);
    chachaXor(key.data(), 1, nonce.data(), ct.data(), n, plain.data());
    return true;
}

// ---------------------------------------------------------------- X25519 ----
namespace {
typedef int64_t gf[16];

void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t(1) << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
void sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t(b) - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}
void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + (int64_t(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
void fM(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}
void fS(gf o, const gf a) { fM(o, a, a); }
void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) {
        fS(c, c);
        if (a != 2 && a != 4) fM(c, c, in);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
}  // namespace

bool x25519(const uint8_t scalar[32], const uint8_t point[32], uint8_t out[32]) {
    static const gf k121665 = {0xDB41, 1};
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, static_cast<int>(r));
        sel25519(c, d, static_cast<int>(r));
        fA(e, a, c); fZ(a, a, c); fA(c, b, d); fZ(b, b, d);
        fS(d, e); fS(f, a); fM(a, c, a); fM(c, b, e);
        fA(e, a, c); fZ(a, a, c); fS(b, a); fZ(c, d, f);
        fM(a, c, k121665); fA(a, a, d); fM(c, c, a); fM(a, d, f);
        fM(d, b, x); fS(b, e);
        sel25519(a, b, static_cast<int>(r));
        sel25519(c, d, static_cast<int>(r));
    }
    for (int i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    fM(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc != 0;
}

void x25519Public(const uint8_t scalar[32], uint8_t out[32]) {
    uint8_t base[32] = {9};
    x25519(scalar, base, out);
}

// ------------------------------------------------------------------ EC ----
namespace {

struct CurveParams {
    BigInt p, b, gx, gy, n;
    size_t bytes;
};

const CurveParams& params(Curve c) {
    static const CurveParams p256 = {
        BigInt::fromHex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff"),
        BigInt::fromHex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"),
        BigInt::fromHex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"),
        BigInt::fromHex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"),
        BigInt::fromHex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"), 32};
    static const CurveParams p384 = {
        BigInt::fromHex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff"),
        BigInt::fromHex("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef"),
        BigInt::fromHex("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7"),
        BigInt::fromHex("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"),
        BigInt::fromHex("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973"), 48};
    return c == Curve::P256 ? p256 : p384;
}

struct Jac {
    BigInt x, y, z;  // z == 0 -> infinity
};

BigInt fmul(const BigInt& a, const BigInt& b, const BigInt& p) { return BigInt::modMul(a, b, p); }
BigInt fadd(const BigInt& a, const BigInt& b, const BigInt& p) { return BigInt::modAdd(a, b, p); }
BigInt fsub(const BigInt& a, const BigInt& b, const BigInt& p) { return BigInt::modSub(a, b, p); }

Jac jacDouble(const Jac& P, const BigInt& p) {
    if (P.z.isZero() || P.y.isZero()) return Jac{BigInt(1), BigInt(1), BigInt()};
    // a = -3
    BigInt delta = fmul(P.z, P.z, p);
    BigInt gamma = fmul(P.y, P.y, p);
    BigInt beta = fmul(P.x, gamma, p);
    BigInt alpha = fmul(BigInt(3), fmul(fsub(P.x, delta, p), fadd(P.x, delta, p), p), p);
    BigInt beta8 = fmul(BigInt(8), beta, p);
    BigInt x3 = fsub(fmul(alpha, alpha, p), beta8, p);
    BigInt yz = fadd(P.y, P.z, p);
    BigInt z3 = fsub(fsub(fmul(yz, yz, p), gamma, p), delta, p);
    BigInt y3 = fsub(fmul(alpha, fsub(fmul(BigInt(4), beta, p), x3, p), p), fmul(BigInt(8), fmul(gamma, gamma, p), p), p);
    return Jac{x3, y3, z3};
}

Jac jacAdd(const Jac& P, const Jac& Q, const BigInt& p) {
    if (P.z.isZero()) return Q;
    if (Q.z.isZero()) return P;
    BigInt z1z1 = fmul(P.z, P.z, p), z2z2 = fmul(Q.z, Q.z, p);
    BigInt u1 = fmul(P.x, z2z2, p), u2 = fmul(Q.x, z1z1, p);
    BigInt s1 = fmul(P.y, fmul(Q.z, z2z2, p), p), s2 = fmul(Q.y, fmul(P.z, z1z1, p), p);
    if (u1 == u2) {
        if (s1 != s2) return Jac{BigInt(1), BigInt(1), BigInt()};
        return jacDouble(P, p);
    }
    BigInt h = fsub(u2, u1, p), r = fsub(s2, s1, p);
    BigInt h2 = fmul(h, h, p), h3 = fmul(h, h2, p), u1h2 = fmul(u1, h2, p);
    BigInt x3 = fsub(fsub(fmul(r, r, p), h3, p), fmul(BigInt(2), u1h2, p), p);
    BigInt y3 = fsub(fmul(r, fsub(u1h2, x3, p), p), fmul(s1, h3, p), p);
    BigInt z3 = fmul(h, fmul(P.z, Q.z, p), p);
    return Jac{x3, y3, z3};
}

EcPoint toAffine(const Jac& P, const BigInt& p) {
    EcPoint out;
    if (P.z.isZero()) {
        out.infinity = true;
        return out;
    }
    BigInt zi = BigInt::modInvPrime(P.z, p);
    BigInt zi2 = fmul(zi, zi, p);
    out.x = fmul(P.x, zi2, p);
    out.y = fmul(P.y, fmul(zi2, zi, p), p);
    return out;
}

Jac fromAffine(const EcPoint& a) {
    if (a.infinity) return Jac{BigInt(1), BigInt(1), BigInt()};
    return Jac{a.x, a.y, BigInt(1)};
}

Jac jacMul(const BigInt& k, const Jac& P, const BigInt& p) {
    Jac R{BigInt(1), BigInt(1), BigInt()};
    for (size_t i = k.bitLength(); i-- > 0;) {
        R = jacDouble(R, p);
        if (k.bit(i)) R = jacAdd(R, P, p);
    }
    return R;
}

// Minimal DER reader for ECDSA signatures.
bool derLen(const Bytes& b, size_t& pos, size_t& len) {
    if (pos >= b.size()) return false;
    uint8_t l = b[pos++];
    if (l < 0x80) {
        len = l;
        return pos + len <= b.size();
    }
    int nb = l & 0x7f;
    if (nb == 0 || nb > 4 || pos + nb > b.size()) return false;
    len = 0;
    for (int i = 0; i < nb; i++) len = (len << 8) | b[pos++];
    return pos + len <= b.size();
}

bool derInteger(const Bytes& b, size_t& pos, BigInt& out) {
    if (pos >= b.size() || b[pos++] != 0x02) return false;
    size_t len;
    if (!derLen(b, pos, len) || len == 0) return false;
    out = BigInt::fromBytes(b.data() + pos, len);
    pos += len;
    return true;
}

}  // namespace

size_t curveBytes(Curve c) { return params(c).bytes; }

bool ecOnCurve(Curve c, const EcPoint& pt) {
    const auto& cp = params(c);
    if (pt.infinity) return false;
    if (!(pt.x < cp.p) || !(pt.y < cp.p)) return false;
    BigInt lhs = fmul(pt.y, pt.y, cp.p);
    BigInt x3 = fmul(pt.x, fmul(pt.x, pt.x, cp.p), cp.p);
    BigInt rhs = fadd(fsub(x3, fmul(BigInt(3), pt.x, cp.p), cp.p), cp.b, cp.p);
    return lhs == rhs;
}

EcPoint ecMul(Curve c, const BigInt& k, const EcPoint& pt) {
    const auto& cp = params(c);
    return toAffine(jacMul(k, fromAffine(pt), cp.p), cp.p);
}

EcPoint ecBaseMul(Curve c, const BigInt& k) {
    const auto& cp = params(c);
    EcPoint g;
    g.x = cp.gx;
    g.y = cp.gy;
    return ecMul(c, k, g);
}

bool ecDecodePoint(Curve c, const uint8_t* p, size_t n, EcPoint& out) {
    size_t bytes = curveBytes(c);
    if (n != 1 + 2 * bytes || p[0] != 4) return false;
    out.infinity = false;
    out.x = BigInt::fromBytes(p + 1, bytes);
    out.y = BigInt::fromBytes(p + 1 + bytes, bytes);
    return ecOnCurve(c, out);
}

bool ecdsaVerify(Curve c, const EcPoint& pub, const Bytes& digest, const Bytes& sigDer) {
    const auto& cp = params(c);
    if (!ecOnCurve(c, pub)) return false;
    size_t pos = 0, len;
    if (sigDer.empty() || sigDer[pos++] != 0x30 || !derLen(sigDer, pos, len) || pos + len != sigDer.size()) return false;
    BigInt r, s;
    if (!derInteger(sigDer, pos, r) || !derInteger(sigDer, pos, s) || pos != sigDer.size()) return false;
    if (r.isZero() || s.isZero() || !(r < cp.n) || !(s < cp.n)) return false;
    // e = leftmost bits of the digest, as wide as n
    BigInt e = BigInt::fromBytes(digest);
    size_t nBits = cp.n.bitLength();
    if (digest.size() * 8 > nBits) e = e.shr(digest.size() * 8 - nBits);
    BigInt w = BigInt::modInvPrime(s, cp.n);
    BigInt u1 = BigInt::modMul(e, w, cp.n), u2 = BigInt::modMul(r, w, cp.n);
    Jac g{cp.gx, cp.gy, BigInt(1)};
    Jac sum = jacAdd(jacMul(u1, g, cp.p), jacMul(u2, fromAffine(pub), cp.p), cp.p);
    EcPoint aff = toAffine(sum, cp.p);
    if (aff.infinity) return false;
    return BigInt::mod(aff.x, cp.n) == r;
}

// ------------------------------------------------------------------ RSA ----
namespace {

const uint8_t kDigestInfoSha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
const uint8_t kDigestInfoSha384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
const uint8_t kDigestInfoSha512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

bool rsaPublic(const Bytes& n, const Bytes& e, const Bytes& sig, Bytes& em, size_t& modBits) {
    BigInt N = BigInt::fromBytes(n), E = BigInt::fromBytes(e), S = BigInt::fromBytes(sig);
    modBits = N.bitLength();
    if (modBits < 1024 || N.isZero() || E.isZero() || !(S < N)) return false;
    size_t k = (modBits + 7) / 8;
    if (sig.size() != k) return false;
    em = BigInt::modPow(S, E, N).toBytes(k);
    return true;
}

Bytes mgf1(Hash h, const Bytes& seed, size_t len) {
    Bytes out;
    for (uint32_t counter = 0; out.size() < len; counter++) {
        Bytes in = seed;
        in.push_back(static_cast<uint8_t>(counter >> 24));
        in.push_back(static_cast<uint8_t>(counter >> 16));
        in.push_back(static_cast<uint8_t>(counter >> 8));
        in.push_back(static_cast<uint8_t>(counter));
        Bytes d = hashOf(h, in);
        out.insert(out.end(), d.begin(), d.end());
    }
    out.resize(len);
    return out;
}

}  // namespace

bool rsaPkcs1Verify(const Bytes& n, const Bytes& e, Hash h, const Bytes& digest, const Bytes& sig) {
    Bytes em;
    size_t modBits;
    if (!rsaPublic(n, e, sig, em, modBits)) return false;
    const uint8_t* prefix = h == Hash::Sha256 ? kDigestInfoSha256 : h == Hash::Sha384 ? kDigestInfoSha384 : kDigestInfoSha512;
    size_t prefixLen = h == Hash::Sha256 ? sizeof(kDigestInfoSha256) : h == Hash::Sha384 ? sizeof(kDigestInfoSha384) : sizeof(kDigestInfoSha512);
    size_t tLen = prefixLen + digest.size();
    if (em.size() < tLen + 11) return false;
    Bytes expect(em.size(), 0xff);
    expect[0] = 0x00;
    expect[1] = 0x01;
    expect[em.size() - tLen - 1] = 0x00;
    std::memcpy(expect.data() + em.size() - tLen, prefix, prefixLen);
    std::memcpy(expect.data() + em.size() - digest.size(), digest.data(), digest.size());
    return ctEqual(em.data(), expect.data(), em.size());
}

bool rsaPssVerify(const Bytes& n, const Bytes& e, Hash h, const Bytes& digest, const Bytes& sig) {
    Bytes em;
    size_t modBits;
    if (!rsaPublic(n, e, sig, em, modBits)) return false;
    size_t hLen = hashSize(h);
    size_t emBits = modBits - 1;
    size_t emLen = (emBits + 7) / 8;
    if (emLen < hLen + 2) return false;
    if (em.size() > emLen) em.erase(em.begin(), em.begin() + (em.size() - emLen));
    if (em[emLen - 1] != 0xbc) return false;
    Bytes maskedDb(em.begin(), em.begin() + (emLen - hLen - 1));
    Bytes hash(em.begin() + (emLen - hLen - 1), em.begin() + (emLen - 1));
    uint8_t topMask = static_cast<uint8_t>(0xff >> (8 * emLen - emBits));
    if (maskedDb[0] & ~topMask) return false;
    Bytes dbMask = mgf1(h, hash, maskedDb.size());
    Bytes db(maskedDb.size());
    for (size_t i = 0; i < db.size(); i++) db[i] = maskedDb[i] ^ dbMask[i];
    db[0] &= topMask;
    size_t i = 0;
    while (i < db.size() && db[i] == 0) i++;
    if (i >= db.size() || db[i] != 0x01) return false;
    Bytes salt(db.begin() + i + 1, db.end());
    Bytes mPrime(8, 0);
    mPrime.insert(mPrime.end(), digest.begin(), digest.end());
    mPrime.insert(mPrime.end(), salt.begin(), salt.end());
    Bytes hPrime = hashOf(h, mPrime);
    return hPrime.size() == hash.size() && ctEqual(hPrime.data(), hash.data(), hash.size());
}

// --------------------------------------------------------------- random ----
void randomBytes(uint8_t* out, size_t n) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("no entropy source available");
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, out + got, n - got);
        if (r <= 0) {
            ::close(fd);
            throw std::runtime_error("failed to read entropy");
        }
        got += static_cast<size_t>(r);
    }
    ::close(fd);
}

}  // namespace crypto
}  // namespace tls
