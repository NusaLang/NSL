// Big integers, SHA-2, HMAC and HKDF for the built-in TLS client.
#include <cstring>
#include <stdexcept>

#include "tls_crypto.hpp"

namespace tls {
namespace crypto {

// ======================================================================
// BigInt
// ======================================================================

BigInt::BigInt(uint64_t v) {
    if (v) d_.push_back(static_cast<uint32_t>(v));
    if (v >> 32) d_.push_back(static_cast<uint32_t>(v >> 32));
}

void BigInt::trim() {
    while (!d_.empty() && d_.back() == 0) d_.pop_back();
}

BigInt BigInt::fromBytes(const uint8_t* p, size_t n) {
    BigInt r;
    r.d_.assign((n + 3) / 4, 0);
    for (size_t i = 0; i < n; i++) {
        size_t byteFromEnd = n - 1 - i;  // significance of p[i]
        r.d_[byteFromEnd / 4] |= static_cast<uint32_t>(p[i]) << (8 * (byteFromEnd % 4));
    }
    r.trim();
    return r;
}

BigInt BigInt::fromHex(const std::string& hex) {
    Bytes b;
    std::string h = hex;
    if (h.size() % 2) h = "0" + h;
    for (size_t i = 0; i < h.size(); i += 2) {
        b.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    }
    return fromBytes(b);
}

Bytes BigInt::toBytes(size_t len) const {
    size_t minimal = (bitLength() + 7) / 8;
    size_t n = len ? len : minimal;
    if (minimal > n) throw std::runtime_error("BigInt: value does not fit in requested length");
    Bytes out(n, 0);
    for (size_t i = 0; i < minimal; i++) {
        out[n - 1 - i] = static_cast<uint8_t>(d_[i / 4] >> (8 * (i % 4)));
    }
    return out;
}

size_t BigInt::bitLength() const {
    if (d_.empty()) return 0;
    uint32_t top = d_.back();
    size_t bits = 0;
    while (top) {
        bits++;
        top >>= 1;
    }
    return (d_.size() - 1) * 32 + bits;
}

bool BigInt::bit(size_t i) const {
    size_t limb = i / 32;
    return limb < d_.size() && ((d_[limb] >> (i % 32)) & 1);
}

int BigInt::cmp(const BigInt& a, const BigInt& b) {
    if (a.d_.size() != b.d_.size()) return a.d_.size() < b.d_.size() ? -1 : 1;
    for (size_t i = a.d_.size(); i-- > 0;) {
        if (a.d_[i] != b.d_[i]) return a.d_[i] < b.d_[i] ? -1 : 1;
    }
    return 0;
}

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
    BigInt r;
    size_t n = std::max(a.d_.size(), b.d_.size());
    r.d_.assign(n + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t s = carry;
        if (i < a.d_.size()) s += a.d_[i];
        if (i < b.d_.size()) s += b.d_[i];
        r.d_[i] = static_cast<uint32_t>(s);
        carry = s >> 32;
    }
    r.d_[n] = static_cast<uint32_t>(carry);
    r.trim();
    return r;
}

BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
    BigInt r;
    r.d_.assign(a.d_.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.d_.size(); i++) {
        int64_t s = static_cast<int64_t>(a.d_[i]) - borrow - (i < b.d_.size() ? static_cast<int64_t>(b.d_[i]) : 0);
        if (s < 0) {
            s += (int64_t(1) << 32);
            borrow = 1;
        } else {
            borrow = 0;
        }
        r.d_[i] = static_cast<uint32_t>(s);
    }
    r.trim();
    return r;
}

BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
    BigInt r;
    if (a.isZero() || b.isZero()) return r;
    r.d_.assign(a.d_.size() + b.d_.size(), 0);
    for (size_t i = 0; i < a.d_.size(); i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.d_.size(); j++) {
            uint64_t cur = static_cast<uint64_t>(a.d_[i]) * b.d_[j] + r.d_[i + j] + carry;
            r.d_[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        r.d_[i + b.d_.size()] = static_cast<uint32_t>(carry);
    }
    r.trim();
    return r;
}

BigInt BigInt::pow2(size_t k) {
    BigInt r;
    r.d_.assign(k / 32 + 1, 0);
    r.d_[k / 32] = uint32_t(1) << (k % 32);
    return r;
}

BigInt BigInt::shl(size_t bits) const {
    if (d_.empty()) return *this;
    BigInt r;
    size_t limbs = bits / 32, sh = bits % 32;
    r.d_.assign(d_.size() + limbs + 1, 0);
    for (size_t i = 0; i < d_.size(); i++) {
        uint64_t v = static_cast<uint64_t>(d_[i]) << sh;
        r.d_[i + limbs] |= static_cast<uint32_t>(v);
        r.d_[i + limbs + 1] |= static_cast<uint32_t>(v >> 32);
    }
    r.trim();
    return r;
}

BigInt BigInt::shr(size_t bits) const {
    size_t limbs = bits / 32, sh = bits % 32;
    if (limbs >= d_.size()) return BigInt();
    BigInt r;
    r.d_.assign(d_.size() - limbs, 0);
    for (size_t i = limbs; i < d_.size(); i++) {
        uint64_t v = d_[i];
        if (i + 1 < d_.size()) v |= static_cast<uint64_t>(d_[i + 1]) << 32;
        r.d_[i - limbs] = static_cast<uint32_t>(v >> sh);
    }
    r.trim();
    return r;
}

BigInt BigInt::shiftedRight1() const { return shr(1); }

uint64_t BigInt::low64() const {
    uint64_t v = 0;
    if (!d_.empty()) v |= d_[0];
    if (d_.size() > 1) v |= static_cast<uint64_t>(d_[1]) << 32;
    return v;
}

// Knuth, TAOCP vol. 2, Algorithm D (Hacker's Delight "divmnu").
void BigInt::divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r) {
    if (b.isZero()) throw std::runtime_error("BigInt: division by zero");
    if (cmp(a, b) < 0) {
        q = BigInt();
        r = a;
        return;
    }
    if (b.d_.size() == 1) {
        uint64_t rem = 0;
        BigInt qq;
        qq.d_.assign(a.d_.size(), 0);
        for (size_t i = a.d_.size(); i-- > 0;) {
            uint64_t cur = (rem << 32) | a.d_[i];
            qq.d_[i] = static_cast<uint32_t>(cur / b.d_[0]);
            rem = cur % b.d_[0];
        }
        qq.trim();
        q = std::move(qq);
        r = BigInt(rem);
        return;
    }
    size_t n = b.d_.size(), m = a.d_.size() - n;
    int s = 0;
    for (uint32_t top = b.d_.back(); !(top & 0x80000000u); top <<= 1) s++;
    std::vector<uint32_t> vn(n), un(a.d_.size() + 1);
    for (size_t i = n - 1; i > 0; i--) {
        vn[i] = (b.d_[i] << s) | (s ? static_cast<uint32_t>(static_cast<uint64_t>(b.d_[i - 1]) >> (32 - s)) : 0);
    }
    vn[0] = b.d_[0] << s;
    un[a.d_.size()] = s ? static_cast<uint32_t>(static_cast<uint64_t>(a.d_.back()) >> (32 - s)) : 0;
    for (size_t i = a.d_.size() - 1; i > 0; i--) {
        un[i] = (a.d_[i] << s) | (s ? static_cast<uint32_t>(static_cast<uint64_t>(a.d_[i - 1]) >> (32 - s)) : 0);
    }
    un[0] = a.d_[0] << s;

    BigInt qq;
    qq.d_.assign(m + 1, 0);
    const uint64_t base = uint64_t(1) << 32;
    for (size_t jj = m + 1; jj-- > 0;) {
        size_t j = jj;
        uint64_t num = (static_cast<uint64_t>(un[j + n]) << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1];
        uint64_t rhat = num % vn[n - 1];
        while (qhat >= base || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= base) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i] + carry;
            carry = p >> 32;
            int64_t t = static_cast<int64_t>(un[i + j]) - borrow - static_cast<int64_t>(p & 0xFFFFFFFFu);
            un[i + j] = static_cast<uint32_t>(t);
            borrow = t < 0 ? 1 : 0;
        }
        int64_t t = static_cast<int64_t>(un[j + n]) - borrow - static_cast<int64_t>(carry);
        un[j + n] = static_cast<uint32_t>(t);
        if (t < 0) {  // qhat was one too big: add the divisor back
            qhat--;
            uint64_t c = 0;
            for (size_t i = 0; i < n; i++) {
                uint64_t sum = static_cast<uint64_t>(un[i + j]) + vn[i] + c;
                un[i + j] = static_cast<uint32_t>(sum);
                c = sum >> 32;
            }
            un[j + n] += static_cast<uint32_t>(c);
        }
        qq.d_[j] = static_cast<uint32_t>(qhat);
    }
    qq.trim();
    BigInt rr;
    rr.d_.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        rr.d_[i] = (un[i] >> s) | (s ? static_cast<uint32_t>(static_cast<uint64_t>(un[i + 1]) << (32 - s)) : 0);
    }
    rr.trim();
    q = std::move(qq);
    r = std::move(rr);
}

BigInt BigInt::mod(const BigInt& a, const BigInt& m) {
    BigInt q, r;
    divmod(a, m, q, r);
    return r;
}

BigInt BigInt::modAdd(const BigInt& a, const BigInt& b, const BigInt& m) {
    BigInt s = add(a, b);
    return cmp(s, m) >= 0 ? sub(s, m) : s;
}

BigInt BigInt::modSub(const BigInt& a, const BigInt& b, const BigInt& m) {
    return cmp(a, b) >= 0 ? sub(a, b) : sub(add(a, m), b);
}

BigInt BigInt::modMul(const BigInt& a, const BigInt& b, const BigInt& m) { return mod(mul(a, b), m); }

BigInt BigInt::modPow(const BigInt& base, const BigInt& exp, const BigInt& m) {
    BigInt result(1);
    BigInt b = mod(base, m);
    size_t bits = exp.bitLength();
    for (size_t i = bits; i-- > 0;) {
        result = modMul(result, result, m);
        if (exp.bit(i)) result = modMul(result, b, m);
    }
    return mod(result, m);
}

BigInt BigInt::modInvPrime(const BigInt& a, const BigInt& p) { return modPow(a, sub(p, BigInt(2)), p); }

// ======================================================================
// SHA-2 (constants derived from the fractional parts of prime roots, so
// there is no table to mistype)
// ======================================================================

namespace {

struct Sha2Constants {
    uint32_t k256[64];
    uint64_t k512[80];
    uint64_t h512[8];
    uint64_t h384[8];
    uint32_t h256[8];
    Sha2Constants() {
        std::vector<uint32_t> primes;
        for (uint32_t c = 2; primes.size() < 80; c++) {
            bool isPrime = true;
            for (uint32_t p : primes) {
                if (p * p > c) break;
                if (c % p == 0) { isPrime = false; break; }
            }
            if (isPrime) primes.push_back(c);
        }
        for (size_t i = 0; i < 80; i++) {
            k512[i] = fracRoot(primes[i], 3);
            if (i < 64) k256[i] = static_cast<uint32_t>(k512[i] >> 32);
        }
        for (size_t i = 0; i < 8; i++) {
            h512[i] = fracRoot(primes[i], 2);
            h384[i] = fracRoot(primes[i + 8], 2);
            h256[i] = static_cast<uint32_t>(h512[i] >> 32);
        }
    }
    // First 64 bits of the fractional part of root_k(p): floor(root_k(p * 2^(64k))) mod 2^64.
    static uint64_t fracRoot(uint32_t p, int k) {
        BigInt target = BigInt::mul(BigInt(p), BigInt::pow2(64 * k));
        BigInt x;
        for (size_t bit = 72; bit-- > 0;) {
            BigInt cand = BigInt::add(x, BigInt::pow2(bit));
            BigInt pw = cand;
            for (int i = 1; i < k; i++) pw = BigInt::mul(pw, cand);
            if (BigInt::cmp(pw, target) <= 0) x = cand;
        }
        return x.low64();
    }
};

const Sha2Constants& sha2() {
    static const Sha2Constants c;
    return c;
}

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

}  // namespace

size_t hashSize(Hash h) { return h == Hash::Sha256 ? 32 : h == Hash::Sha384 ? 48 : 64; }
size_t hashBlockSize(Hash h) { return h == Hash::Sha256 ? 64 : 128; }

Hasher::Hasher(Hash h) : kind_(h) {
    const auto& c = sha2();
    if (h == Hash::Sha256) {
        for (int i = 0; i < 8; i++) h32_[i] = c.h256[i];
    } else if (h == Hash::Sha384) {
        for (int i = 0; i < 8; i++) h64_[i] = c.h384[i];
    } else {
        for (int i = 0; i < 8; i++) h64_[i] = c.h512[i];
    }
}

void Hasher::compress(const uint8_t* block) {
    const auto& c = sha2();
    if (kind_ == Hash::Sha256) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
                   (uint32_t(block[4 * i + 2]) << 8) | block[4 * i + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h32_[0], b = h32_[1], cc = h32_[2], d = h32_[3], e = h32_[4], f = h32_[5], g = h32_[6], h = h32_[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + c.k256[i] + w[i];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
        }
        h32_[0] += a; h32_[1] += b; h32_[2] += cc; h32_[3] += d;
        h32_[4] += e; h32_[5] += f; h32_[6] += g; h32_[7] += h;
    } else {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            uint64_t v = 0;
            for (int j = 0; j < 8; j++) v = (v << 8) | block[8 * i + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h64_[0], b = h64_[1], cc = h64_[2], d = h64_[3], e = h64_[4], f = h64_[5], g = h64_[6], h = h64_[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = h + S1 + ch + c.k512[i] + w[i];
            uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
            uint64_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
        }
        h64_[0] += a; h64_[1] += b; h64_[2] += cc; h64_[3] += d;
        h64_[4] += e; h64_[5] += f; h64_[6] += g; h64_[7] += h;
    }
}

void Hasher::update(const uint8_t* data, size_t len) {
    size_t block = hashBlockSize(kind_);
    total_ += len;
    while (len > 0) {
        size_t take = std::min(len, block - bufLen_);
        std::memcpy(buf_.data() + bufLen_, data, take);
        bufLen_ += take;
        data += take;
        len -= take;
        if (bufLen_ == block) {
            compress(buf_.data());
            bufLen_ = 0;
        }
    }
}

Bytes Hasher::finish() const {
    Hasher t = *this;  // finish a copy so the caller can keep updating
    size_t block = hashBlockSize(kind_);
    size_t lenBytes = block == 64 ? 8 : 16;
    uint64_t bits = total_ * 8;
    uint8_t pad[256] = {0x80};
    size_t padLen = (t.bufLen_ < block - lenBytes) ? (block - lenBytes - t.bufLen_) : (2 * block - lenBytes - t.bufLen_);
    uint8_t lenField[16] = {0};
    for (int i = 0; i < 8; i++) lenField[lenBytes - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    uint64_t savedTotal = t.total_;
    t.update(pad, padLen);
    t.update(lenField, lenBytes);
    (void)savedTotal;
    Bytes out;
    if (kind_ == Hash::Sha256) {
        for (int i = 0; i < 8; i++)
            for (int j = 3; j >= 0; j--) out.push_back(static_cast<uint8_t>(t.h32_[i] >> (8 * j)));
    } else {
        for (int i = 0; i < 8; i++)
            for (int j = 7; j >= 0; j--) out.push_back(static_cast<uint8_t>(t.h64_[i] >> (8 * j)));
        out.resize(hashSize(kind_));
    }
    return out;
}

Bytes hashOf(Hash h, const uint8_t* data, size_t len) {
    Hasher hs(h);
    hs.update(data, len);
    return hs.finish();
}

// ======================================================================
// HMAC / HKDF / PRF
// ======================================================================

Bytes hmac(Hash h, const Bytes& key, const uint8_t* data, size_t len) {
    size_t block = hashBlockSize(h);
    Bytes k = key;
    if (k.size() > block) k = hashOf(h, k);
    k.resize(block, 0);
    Bytes ipad(block), opad(block);
    for (size_t i = 0; i < block; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Hasher inner(h);
    inner.update(ipad);
    inner.update(data, len);
    Bytes ih = inner.finish();
    Hasher outer(h);
    outer.update(opad);
    outer.update(ih);
    return outer.finish();
}

Bytes hkdfExtract(Hash h, const Bytes& salt, const Bytes& ikm) {
    Bytes s = salt.empty() ? Bytes(hashSize(h), 0) : salt;
    return hmac(h, s, ikm);
}

Bytes hkdfExpand(Hash h, const Bytes& prk, const Bytes& info, size_t length) {
    Bytes out, t;
    for (uint8_t counter = 1; out.size() < length; counter++) {
        Bytes msg = t;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(counter);
        t = hmac(h, prk, msg);
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

Bytes hkdfExpandLabel(Hash h, const Bytes& secret, const std::string& label, const Bytes& context, size_t length) {
    std::string full = "tls13 " + label;
    Bytes info;
    info.push_back(static_cast<uint8_t>(length >> 8));
    info.push_back(static_cast<uint8_t>(length));
    info.push_back(static_cast<uint8_t>(full.size()));
    info.insert(info.end(), full.begin(), full.end());
    info.push_back(static_cast<uint8_t>(context.size()));
    info.insert(info.end(), context.begin(), context.end());
    return hkdfExpand(h, secret, info, length);
}

Bytes tls12Prf(Hash h, const Bytes& secret, const std::string& label, const Bytes& seed, size_t length) {
    Bytes labelSeed(label.begin(), label.end());
    labelSeed.insert(labelSeed.end(), seed.begin(), seed.end());
    Bytes out, a = labelSeed;
    while (out.size() < length) {
        a = hmac(h, secret, a);
        Bytes msg = a;
        msg.insert(msg.end(), labelSeed.begin(), labelSeed.end());
        Bytes chunk = hmac(h, secret, msg);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    out.resize(length);
    return out;
}

}  // namespace crypto
}  // namespace tls
