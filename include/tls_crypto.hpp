#pragma once

// Cryptographic primitives for the built-in TLS client (see tls.hpp).
// Everything here is self-contained C++ -- no third-party code. It is written
// for a *client* verifying public data: big-integer and elliptic-curve code is
// NOT constant-time (fine for verifying signatures), while the parts that touch
// secrets (X25519, AES-GCM, ChaCha20-Poly1305) avoid secret-dependent branches.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tls {
namespace crypto {

using Bytes = std::vector<uint8_t>;

// ---- hashes ----
enum class Hash { Sha256, Sha384, Sha512 };
size_t hashSize(Hash h);
size_t hashBlockSize(Hash h);
Bytes hashOf(Hash h, const uint8_t* data, size_t len);
inline Bytes hashOf(Hash h, const Bytes& b) { return hashOf(h, b.data(), b.size()); }

// Incremental hash (transcript hashing needs running snapshots).
class Hasher {
public:
    explicit Hasher(Hash h);
    void update(const uint8_t* data, size_t len);
    void update(const Bytes& b) { update(b.data(), b.size()); }
    Bytes finish() const;  // does not disturb the running state
    Hash kind() const { return kind_; }

private:
    Hash kind_;
    uint64_t total_ = 0;
    std::array<uint8_t, 128> buf_{};
    size_t bufLen_ = 0;
    std::array<uint64_t, 8> h64_{};
    std::array<uint32_t, 8> h32_{};
    void compress(const uint8_t* block);
};

Bytes hmac(Hash h, const Bytes& key, const uint8_t* data, size_t len);
inline Bytes hmac(Hash h, const Bytes& key, const Bytes& data) { return hmac(h, key, data.data(), data.size()); }
Bytes hkdfExtract(Hash h, const Bytes& salt, const Bytes& ikm);
Bytes hkdfExpand(Hash h, const Bytes& prk, const Bytes& info, size_t length);
// TLS 1.3 HKDF-Expand-Label / Derive-Secret.
Bytes hkdfExpandLabel(Hash h, const Bytes& secret, const std::string& label, const Bytes& context, size_t length);
// TLS 1.2 PRF (P_hash).
Bytes tls12Prf(Hash h, const Bytes& secret, const std::string& label, const Bytes& seed, size_t length);

// ---- AEAD ----
// AES-128/256-GCM and ChaCha20-Poly1305. seal returns ciphertext||tag; open
// returns false on authentication failure.
Bytes aesGcmSeal(const Bytes& key, const Bytes& nonce12, const Bytes& aad, const Bytes& plain);
bool aesGcmOpen(const Bytes& key, const Bytes& nonce12, const Bytes& aad, const Bytes& cipherAndTag, Bytes& plain);
Bytes chachaSeal(const Bytes& key32, const Bytes& nonce12, const Bytes& aad, const Bytes& plain);
bool chachaOpen(const Bytes& key32, const Bytes& nonce12, const Bytes& aad, const Bytes& cipherAndTag, Bytes& plain);

// ---- key exchange ----
// X25519 (RFC 7748). Returns false if the result is all zeros.
bool x25519(const uint8_t scalar[32], const uint8_t point[32], uint8_t out[32]);
void x25519Public(const uint8_t scalar[32], uint8_t out[32]);

// ---- big integers (little-endian 32-bit limbs) ----
class BigInt {
public:
    BigInt() = default;
    explicit BigInt(uint64_t v);
    static BigInt fromBytes(const uint8_t* p, size_t n);  // big-endian
    static BigInt fromBytes(const Bytes& b) { return fromBytes(b.data(), b.size()); }
    static BigInt fromHex(const std::string& hex);
    Bytes toBytes(size_t len = 0) const;  // big-endian, left-padded to len (0 = minimal)
    bool isZero() const { return d_.empty(); }
    bool isOdd() const { return !d_.empty() && (d_[0] & 1); }
    size_t bitLength() const;
    bool bit(size_t i) const;
    static int cmp(const BigInt& a, const BigInt& b);
    friend bool operator==(const BigInt& a, const BigInt& b) { return cmp(a, b) == 0; }
    friend bool operator!=(const BigInt& a, const BigInt& b) { return cmp(a, b) != 0; }
    friend bool operator<(const BigInt& a, const BigInt& b) { return cmp(a, b) < 0; }

    static BigInt add(const BigInt& a, const BigInt& b);
    static BigInt sub(const BigInt& a, const BigInt& b);  // requires a >= b
    static BigInt mul(const BigInt& a, const BigInt& b);
    static void divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r);
    static BigInt mod(const BigInt& a, const BigInt& m);
    static BigInt modAdd(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt modSub(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt modMul(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt modPow(const BigInt& base, const BigInt& exp, const BigInt& m);
    static BigInt modInvPrime(const BigInt& a, const BigInt& p);  // Fermat; p must be prime
    BigInt shiftedRight1() const;
    static BigInt pow2(size_t k);
    BigInt shl(size_t bits) const;
    BigInt shr(size_t bits) const;
    uint64_t low64() const;

private:
    std::vector<uint32_t> d_;
    void trim();
};

// ---- signatures (verification only) ----
enum class Curve { P256, P384 };
struct EcPoint {
    BigInt x, y;
    bool infinity = false;
};
bool ecOnCurve(Curve c, const EcPoint& p);
EcPoint ecMul(Curve c, const BigInt& k, const EcPoint& p);   // affine in, affine out
EcPoint ecBaseMul(Curve c, const BigInt& k);
size_t curveBytes(Curve c);
// Public key as SEC1 uncompressed point (04 || X || Y).
bool ecDecodePoint(Curve c, const uint8_t* p, size_t n, EcPoint& out);
// `sigDer` is the ASN.1 SEQUENCE{r, s}; `digest` is the message hash.
bool ecdsaVerify(Curve c, const EcPoint& pub, const Bytes& digest, const Bytes& sigDer);

// RSA public-key operations. n, e big-endian.
bool rsaPkcs1Verify(const Bytes& n, const Bytes& e, Hash h, const Bytes& digest, const Bytes& sig);
bool rsaPssVerify(const Bytes& n, const Bytes& e, Hash h, const Bytes& digest, const Bytes& sig);

// Fills `out` from the OS entropy source. Throws on failure.
void randomBytes(uint8_t* out, size_t n);

}  // namespace crypto
}  // namespace tls
