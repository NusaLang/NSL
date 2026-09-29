// TLS 1.2 / 1.3 client. See tls.hpp for scope and limitations.
#include "tls.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace tls {

using namespace crypto;

namespace {

// ------------------------------------------------------------ constants ----
enum : uint8_t { kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22, kApplicationData = 23 };
enum : uint8_t {
    kClientHello = 1, kServerHello = 2, kNewSessionTicket = 4, kEncryptedExtensions = 8, kCertificate = 11,
    kServerKeyExchange = 12, kCertificateRequest = 13, kServerHelloDone = 14, kCertificateVerify = 15,
    kClientKeyExchange = 16, kFinished = 20, kKeyUpdate = 24
};
constexpr uint16_t kGroupX25519 = 0x001d, kGroupP256 = 0x0017, kGroupP384 = 0x0018;
constexpr size_t kMaxPlaintext = 16384;

enum class Aead { AesGcm, ChaCha };

struct Suite {
    uint16_t id;
    const char* name;
    Aead aead;
    size_t keyLen;
    Hash hash;         // TLS 1.3 hash / TLS 1.2 PRF hash
    bool tls13;
    bool ecdsa;        // TLS 1.2: ECDHE_ECDSA (false = ECDHE_RSA)
};

const Suite kSuites[] = {
    {0x1301, "TLS_AES_128_GCM_SHA256", Aead::AesGcm, 16, Hash::Sha256, true, false},
    {0x1302, "TLS_AES_256_GCM_SHA384", Aead::AesGcm, 32, Hash::Sha384, true, false},
    {0x1303, "TLS_CHACHA20_POLY1305_SHA256", Aead::ChaCha, 32, Hash::Sha256, true, false},
    {0xc02b, "ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", Aead::AesGcm, 16, Hash::Sha256, false, true},
    {0xc02f, "ECDHE_RSA_WITH_AES_128_GCM_SHA256", Aead::AesGcm, 16, Hash::Sha256, false, false},
    {0xc02c, "ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", Aead::AesGcm, 32, Hash::Sha384, false, true},
    {0xc030, "ECDHE_RSA_WITH_AES_256_GCM_SHA384", Aead::AesGcm, 32, Hash::Sha384, false, false},
    {0xcca9, "ECDHE_ECDSA_WITH_CHACHA20_POLY1305", Aead::ChaCha, 32, Hash::Sha256, false, true},
    {0xcca8, "ECDHE_RSA_WITH_CHACHA20_POLY1305", Aead::ChaCha, 32, Hash::Sha256, false, false},
};

const Suite* findSuite(uint16_t id) {
    for (const Suite& s : kSuites) if (s.id == id) return &s;
    return nullptr;
}

// ------------------------------------------------------------ byte I/O ----
struct Writer {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); }
    void u24(uint32_t v) { u8(static_cast<uint8_t>(v >> 16)); u16(static_cast<uint16_t>(v)); }
    void raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
    void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void vec8(const Bytes& v) { u8(static_cast<uint8_t>(v.size())); raw(v); }
    void vec16(const Bytes& v) { u16(static_cast<uint16_t>(v.size())); raw(v); }
    // Length-prefixed (2 bytes) block written by `fn`.
    template <class F> void block16(F fn) {
        size_t at = b.size();
        u16(0);
        fn();
        size_t len = b.size() - at - 2;
        b[at] = static_cast<uint8_t>(len >> 8);
        b[at + 1] = static_cast<uint8_t>(len);
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    Reader(const uint8_t* start, size_t n) : p(start), end(start + n) {}
    explicit Reader(const Bytes& b) : p(b.data()), end(b.data() + b.size()) {}
    size_t left() const { return static_cast<size_t>(end - p); }
    void need(size_t n) const { if (left() < n) throw Error("malformed handshake message"); }
    uint8_t u8() { need(1); return *p++; }
    uint16_t u16() { need(2); uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]); p += 2; return v; }
    uint32_t u24() { need(3); uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; p += 3; return v; }
    Bytes take(size_t n) { need(n); Bytes v(p, p + n); p += n; return v; }
    Reader sub(size_t n) { need(n); Reader r(p, n); p += n; return r; }
    Reader sub8() { return sub(u8()); }
    Reader sub16() { return sub(u16()); }
    Reader sub24() { return sub(u24()); }
    bool empty() const { return p == end; }
};

Bytes concat(const Bytes& a, const Bytes& b) {
    Bytes r = a;
    r.insert(r.end(), b.begin(), b.end());
    return r;
}

// ----------------------------------------------------------- signatures ----
// Verifies a handshake signature with the certificate's key. `sigAlg` is the TLS SignatureScheme.
bool verifyHandshakeSignature(const x509::Certificate& cert, uint16_t sigAlg, const Bytes& signed_, const Bytes& sig, bool tls13) {
    switch (sigAlg) {
        case 0x0401: case 0x0501: case 0x0601: {  // rsa_pkcs1_sha256/384/512 (TLS 1.2 only)
            if (tls13 || cert.keyType != x509::KeyType::Rsa) return false;
            Hash h = sigAlg == 0x0401 ? Hash::Sha256 : sigAlg == 0x0501 ? Hash::Sha384 : Hash::Sha512;
            return rsaPkcs1Verify(cert.rsaN, cert.rsaE, h, hashOf(h, signed_), sig);
        }
        case 0x0804: case 0x0805: case 0x0806: {  // rsa_pss_rsae_sha256/384/512
            if (cert.keyType != x509::KeyType::Rsa) return false;
            Hash h = sigAlg == 0x0804 ? Hash::Sha256 : sigAlg == 0x0805 ? Hash::Sha384 : Hash::Sha512;
            return rsaPssVerify(cert.rsaN, cert.rsaE, h, hashOf(h, signed_), sig);
        }
        case 0x0403:  // ecdsa_secp256r1_sha256
        case 0x0503: {  // ecdsa_secp384r1_sha384
            Hash h = sigAlg == 0x0403 ? Hash::Sha256 : Hash::Sha384;
            Curve want = sigAlg == 0x0403 ? Curve::P256 : Curve::P384;
            bool p256 = cert.keyType == x509::KeyType::EcP256, p384 = cert.keyType == x509::KeyType::EcP384;
            if (!p256 && !p384) return false;
            Curve have = p256 ? Curve::P256 : Curve::P384;
            // TLS 1.3 ties the scheme to its curve; TLS 1.2 only names the hash.
            if (tls13 && have != want) return false;
            return ecdsaVerify(have, cert.ecPoint, hashOf(h, signed_), sig);
        }
        default:
            return false;
    }
}

// ----------------------------------------------------------- key shares ----
struct KeyShares {
    uint8_t x25519Priv[32];
    uint8_t x25519Pub[32];
    BigInt p256Priv;
    Bytes p256Pub;  // uncompressed point
    // secp384r1 is only ever used for a TLS 1.2 server that insists on it, so its
    // ephemeral key is made on demand.
    BigInt p384Priv;
    Bytes p384Pub;
    KeyShares() {
        randomBytes(x25519Priv, 32);
        x25519Public(x25519Priv, x25519Pub);
        // P-256 scalar in [1, n-1]: 32 random bytes are below n with overwhelming probability;
        // reduce anyway and reject zero.
        for (;;) {
            Bytes r(32);
            randomBytes(r.data(), 32);
            p256Priv = BigInt::mod(BigInt::fromBytes(r), BigInt::fromHex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"));
            if (!p256Priv.isZero()) break;
        }
        EcPoint pub = ecBaseMul(Curve::P256, p256Priv);
        p256Pub.push_back(4);
        Bytes x = pub.x.toBytes(32), y = pub.y.toBytes(32);
        p256Pub.insert(p256Pub.end(), x.begin(), x.end());
        p256Pub.insert(p256Pub.end(), y.begin(), y.end());
    }
    void ensureP384() {
        if (!p384Pub.empty()) return;
        BigInt n = BigInt::fromHex("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973");
        for (;;) {
            Bytes r(48);
            randomBytes(r.data(), 48);
            p384Priv = BigInt::mod(BigInt::fromBytes(r), n);
            if (!p384Priv.isZero()) break;
        }
        EcPoint pub = ecBaseMul(Curve::P384, p384Priv);
        p384Pub.push_back(4);
        Bytes x = pub.x.toBytes(48), y = pub.y.toBytes(48);
        p384Pub.insert(p384Pub.end(), x.begin(), x.end());
        p384Pub.insert(p384Pub.end(), y.begin(), y.end());
    }
    // ECDHE shared secret with the peer's public value for `group`.
    Bytes agree(uint16_t group, const Bytes& peer) {
        if (group == kGroupX25519) {
            if (peer.size() != 32) throw Error("bad X25519 key share");
            uint8_t out[32];
            if (!x25519(x25519Priv, peer.data(), out)) throw Error("X25519 produced an all-zero secret");
            return Bytes(out, out + 32);
        }
        if (group == kGroupP256) {
            EcPoint pt;
            if (!ecDecodePoint(Curve::P256, peer.data(), peer.size(), pt)) throw Error("bad P-256 key share");
            EcPoint r = ecMul(Curve::P256, p256Priv, pt);
            if (r.infinity) throw Error("P-256 produced the point at infinity");
            return r.x.toBytes(32);
        }
        if (group == kGroupP384) {
            ensureP384();
            EcPoint pt;
            if (!ecDecodePoint(Curve::P384, peer.data(), peer.size(), pt)) throw Error("bad P-384 key share");
            EcPoint r = ecMul(Curve::P384, p384Priv, pt);
            if (r.infinity) throw Error("P-384 produced the point at infinity");
            return r.x.toBytes(48);
        }
        throw Error("server chose a key exchange group we did not offer");
    }
};

}  // namespace

// ==========================================================================
struct Connection::Impl {
    Transport io;
    std::string host;
    Options opts;
    Bytes rawIn;        // bytes received but not yet parsed into records
    bool eof = false;

    // handshake transcript (all handshake messages, headers included)
    Bytes transcript;
    Bytes hsBuf;        // handshake bytes received but not yet consumed as messages

    // record protection, per direction
    struct Protect {
        bool on = false;
        Aead aead = Aead::AesGcm;
        Bytes key, iv;      // iv: 12 bytes (1.3, ChaCha 1.2) or 4-byte salt (AES-GCM 1.2)
        uint64_t seq = 0;
    };
    Protect rd, wr;
    bool tls13 = false;
    const Suite* suite = nullptr;
    Bytes appData;      // decrypted application bytes not yet returned
    size_t appOff = 0;
    bool closed = false;
    Bytes rdSecret, wrSecret;  // TLS 1.3 traffic secrets (for KeyUpdate)

    // ---------------------------------------------------------- transport
    void sendAll(const Bytes& data) {
        size_t off = 0;
        while (off < data.size()) {
            long n = io.send(data.data() + off, data.size() - off);
            if (n <= 0) throw Error("connection write failed");
            off += static_cast<size_t>(n);
        }
    }

    bool fill(size_t need) {  // false on clean EOF before any of the needed bytes arrived
        while (rawIn.size() < need) {
            uint8_t buf[16384];
            long n = io.recv(buf, sizeof buf);
            if (n < 0) throw Error("connection read failed");
            if (n == 0) {
                eof = true;
                return false;
            }
            rawIn.insert(rawIn.end(), buf, buf + n);
        }
        return true;
    }

    // ------------------------------------------------------------ records
    static Bytes nonceFor(const Protect& p) {
        Bytes n = p.iv;
        for (int i = 0; i < 8; i++) n[n.size() - 1 - i] ^= static_cast<uint8_t>(p.seq >> (8 * i));
        return n;
    }

    Bytes seal(Protect& p, uint8_t type, const Bytes& plain) {
        if (tls13) {
            Bytes inner = plain;
            inner.push_back(type);
            Bytes aad = {kApplicationData, 3, 3, static_cast<uint8_t>((inner.size() + 16) >> 8), static_cast<uint8_t>(inner.size() + 16)};
            Bytes nonce = nonceFor(p);
            Bytes ct = p.aead == Aead::AesGcm ? aesGcmSeal(p.key, nonce, aad, inner) : chachaSeal(p.key, nonce, aad, inner);
            p.seq++;
            return ct;
        }
        Bytes aad(13);
        for (int i = 0; i < 8; i++) aad[i] = static_cast<uint8_t>(p.seq >> (56 - 8 * i));
        aad[8] = type;
        aad[9] = 3;
        aad[10] = 3;
        aad[11] = static_cast<uint8_t>(plain.size() >> 8);
        aad[12] = static_cast<uint8_t>(plain.size());
        Bytes out;
        if (p.aead == Aead::AesGcm) {
            Bytes nonce = p.iv;  // 4-byte salt || 8-byte explicit (= sequence number)
            for (int i = 0; i < 8; i++) nonce.push_back(static_cast<uint8_t>(p.seq >> (56 - 8 * i)));
            out.assign(nonce.begin() + 4, nonce.end());
            Bytes ct = aesGcmSeal(p.key, nonce, aad, plain);
            out.insert(out.end(), ct.begin(), ct.end());
        } else {
            out = chachaSeal(p.key, nonceFor(p), aad, plain);
        }
        p.seq++;
        return out;
    }

    void sendRecord(uint8_t type, const Bytes& payload) {
        size_t off = 0;
        do {
            size_t n = std::min(kMaxPlaintext, payload.size() - off);
            Bytes chunk(payload.begin() + static_cast<std::ptrdiff_t>(off), payload.begin() + static_cast<std::ptrdiff_t>(off + n));
            Bytes body = wr.on ? seal(wr, type, chunk) : chunk;
            uint8_t outerType = (wr.on && tls13) ? static_cast<uint8_t>(kApplicationData) : type;
            // Record-layer version is fixed at 0x0303 (legacy field; the real version is negotiated inside).
            Bytes rec = {outerType, 3, 3, static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size())};
            rec.insert(rec.end(), body.begin(), body.end());
            sendAll(rec);
            off += n;
        } while (off < payload.size());
    }

    // Returns false on clean EOF at a record boundary.
    bool readRecord(uint8_t& type, Bytes& payload) {
        if (!fill(5)) {
            if (rawIn.empty()) return false;
            throw Error("connection closed in the middle of a record");
        }
        uint8_t outer = rawIn[0];
        size_t len = (size_t(rawIn[3]) << 8) | rawIn[4];
        if (len > kMaxPlaintext + 2048) throw Error("oversized TLS record");
        if (!fill(5 + len)) throw Error("connection closed in the middle of a record");
        Bytes body(rawIn.begin() + 5, rawIn.begin() + 5 + static_cast<std::ptrdiff_t>(len));
        Bytes header(rawIn.begin(), rawIn.begin() + 5);
        rawIn.erase(rawIn.begin(), rawIn.begin() + 5 + static_cast<std::ptrdiff_t>(len));
        type = outer;
        if (!rd.on || outer == kChangeCipherSpec) {
            payload = std::move(body);
            return true;
        }
        if (tls13) {
            if (outer != kApplicationData) throw Error("unencrypted record after handshake keys were set");
            Bytes aad(header.begin(), header.end());
            Bytes plain;
            bool ok = rd.aead == Aead::AesGcm ? aesGcmOpen(rd.key, nonceFor(rd), aad, body, plain) : chachaOpen(rd.key, nonceFor(rd), aad, body, plain);
            if (!ok) throw Error("bad record MAC");
            rd.seq++;
            while (!plain.empty() && plain.back() == 0) plain.pop_back();
            if (plain.empty()) throw Error("empty inner plaintext");
            type = plain.back();
            plain.pop_back();
            if (plain.size() > kMaxPlaintext) throw Error("oversized plaintext");
            payload = std::move(plain);
            return true;
        }
        // TLS 1.2 AEAD
        Bytes plain;
        if (rd.aead == Aead::AesGcm) {
            if (body.size() < 8 + 16) throw Error("short record");
            Bytes nonce = rd.iv;
            nonce.insert(nonce.end(), body.begin(), body.begin() + 8);
            Bytes ct(body.begin() + 8, body.end());
            size_t plainLen = ct.size() - 16;
            Bytes aad(13);
            for (int i = 0; i < 8; i++) aad[i] = static_cast<uint8_t>(rd.seq >> (56 - 8 * i));
            aad[8] = outer; aad[9] = 3; aad[10] = 3;
            aad[11] = static_cast<uint8_t>(plainLen >> 8);
            aad[12] = static_cast<uint8_t>(plainLen);
            if (!aesGcmOpen(rd.key, nonce, aad, ct, plain)) throw Error("bad record MAC");
        } else {
            if (body.size() < 16) throw Error("short record");
            size_t plainLen = body.size() - 16;
            Bytes aad(13);
            for (int i = 0; i < 8; i++) aad[i] = static_cast<uint8_t>(rd.seq >> (56 - 8 * i));
            aad[8] = outer; aad[9] = 3; aad[10] = 3;
            aad[11] = static_cast<uint8_t>(plainLen >> 8);
            aad[12] = static_cast<uint8_t>(plainLen);
            if (!chachaOpen(rd.key, nonceFor(rd), aad, body, plain)) throw Error("bad record MAC");
        }
        rd.seq++;
        payload = std::move(plain);
        return true;
    }

    // Next complete handshake message (header + body). Throws on alert / unexpected records.
    Bytes nextHandshake() {
        for (;;) {
            if (hsBuf.size() >= 4) {
                size_t len = (size_t(hsBuf[1]) << 16) | (size_t(hsBuf[2]) << 8) | hsBuf[3];
                if (hsBuf.size() >= 4 + len) {
                    Bytes msg(hsBuf.begin(), hsBuf.begin() + 4 + static_cast<std::ptrdiff_t>(len));
                    hsBuf.erase(hsBuf.begin(), hsBuf.begin() + 4 + static_cast<std::ptrdiff_t>(len));
                    return msg;
                }
            }
            uint8_t type;
            Bytes payload;
            if (!readRecord(type, payload)) throw Error("connection closed during the handshake");
            if (type == kAlert) throw Error(alertText(payload));
            if (type == kChangeCipherSpec) {
                if (!tls13) {  // TLS 1.2: the peer switches to encrypted records after this
                    if (!hsBuf.empty()) throw Error("ChangeCipherSpec in the middle of a handshake message");
                    ccsSeen = true;
                    return Bytes();  // empty marker: caller handles the switch
                }
                continue;  // TLS 1.3 compatibility CCS: ignore
            }
            if (type != kHandshake) throw Error("unexpected record type during the handshake");
            hsBuf.insert(hsBuf.end(), payload.begin(), payload.end());
        }
    }
    bool ccsSeen = false;

    static std::string alertText(const Bytes& payload) {
        if (payload.size() < 2) return "TLS alert";
        static const struct { int code; const char* text; } names[] = {
            {0, "close_notify"}, {10, "unexpected_message"}, {20, "bad_record_mac"}, {40, "handshake_failure"},
            {42, "bad_certificate"}, {43, "unsupported_certificate"}, {44, "certificate_revoked"}, {45, "certificate_expired"},
            {46, "certificate_unknown"}, {47, "illegal_parameter"}, {48, "unknown_ca"}, {50, "decode_error"},
            {51, "decrypt_error"}, {70, "protocol_version"}, {71, "insufficient_security"}, {80, "internal_error"},
            {86, "inappropriate_fallback"}, {112, "unrecognized_name"}, {120, "no_application_protocol"}};
        for (auto& n : names) if (n.code == payload[1]) return std::string("server sent TLS alert: ") + n.text;
        return "server sent TLS alert " + std::to_string(payload[1]);
    }

    // ------------------------------------------------------- ClientHello
    Bytes clientRandom, serverRandom;

    Bytes buildClientHello(const KeyShares& ks) {
        Writer w;
        w.u16(0x0303);
        clientRandom.resize(32);
        randomBytes(clientRandom.data(), 32);
        w.raw(clientRandom);
        Bytes sid(32);
        randomBytes(sid.data(), 32);
        w.vec8(sid);
        w.block16([&] { for (const Suite& s : kSuites) w.u16(s.id); });
        w.u8(1);
        w.u8(0);  // null compression
        w.block16([&] {
            auto ext = [&](uint16_t type, auto fn) {
                w.u16(type);
                w.block16(fn);
            };
            bool isIp = host.find(':') != std::string::npos ||
                        std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
            if (!isIp) {
                ext(0, [&] {  // server_name
                    w.block16([&] {
                        w.u8(0);
                        w.vec16(Bytes(host.begin(), host.end()));
                    });
                });
            }
            ext(10, [&] { w.block16([&] { w.u16(kGroupX25519); w.u16(kGroupP256); w.u16(kGroupP384); }); });  // supported_groups
            ext(11, [&] { w.u8(1); w.u8(0); });                                             // ec_point_formats
            ext(13, [&] {                                                                   // signature_algorithms
                w.block16([&] {
                    for (uint16_t a : {0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601}) w.u16(a);
                });
            });
            ext(23, [&] {});                                                                // extended_master_secret
            ext(0xff01, [&] { w.u8(0); });                                                  // renegotiation_info (initial)
            if (!opts.alpn.empty()) {
                ext(16, [&] {
                    w.block16([&] {
                        for (const std::string& p : opts.alpn) w.vec8(Bytes(p.begin(), p.end()));
                    });
                });
            }
            ext(43, [&] { w.u8(4); w.u16(0x0304); w.u16(0x0303); });                        // supported_versions
            ext(51, [&] {                                                                   // key_share
                w.block16([&] {
                    w.u16(kGroupX25519);
                    w.vec16(Bytes(ks.x25519Pub, ks.x25519Pub + 32));
                    w.u16(kGroupP256);
                    w.vec16(ks.p256Pub);
                });
            });
        });
        Bytes msg;
        msg.push_back(kClientHello);
        Bytes body = w.b;
        msg.push_back(static_cast<uint8_t>(body.size() >> 16));
        msg.push_back(static_cast<uint8_t>(body.size() >> 8));
        msg.push_back(static_cast<uint8_t>(body.size()));
        msg.insert(msg.end(), body.begin(), body.end());
        return msg;
    }

    // ---------------------------------------------------- key derivation
    void setKeys(Protect& p, const Suite* s, const Bytes& secret) {
        p.on = true;
        p.aead = s->aead;
        p.key = hkdfExpandLabel(s->hash, secret, "key", {}, s->keyLen);
        p.iv = hkdfExpandLabel(s->hash, secret, "iv", {}, 12);
        p.seq = 0;
    }

    Bytes transcriptHash(Hash h) const { return hashOf(h, transcript); }

    // --------------------------------------------------- certificate check
    x509::Certificate leaf;
    bool haveLeaf = false;

    void checkCertificates(const std::vector<Bytes>& chain) {
        if (chain.empty()) throw Error("server sent no certificate");
        if (!opts.verify) {
            leaf = x509::parse(chain[0]);
            haveLeaf = true;
            return;
        }
        const x509::TrustStore& trust = opts.trust ? *opts.trust : x509::TrustStore::system();
        int64_t now = opts.now ? opts.now : static_cast<int64_t>(std::time(nullptr));
        std::string why = x509::validateChain(chain, host, now, trust, &leaf);
        if (!why.empty()) throw Error("certificate verification failed: " + why);
        haveLeaf = true;
    }

    // ------------------------------------------------------- ServerHello
    struct ServerHelloInfo {
        uint16_t version = 0x0303;
        uint16_t suiteId = 0;
        uint16_t group = 0;
        Bytes keyShare;
        bool ems = false;
        std::string alpn;
    };

    ServerHelloInfo parseServerHello(const Bytes& msg) {
        Reader r(msg.data() + 4, msg.size() - 4);
        ServerHelloInfo info;
        r.u16();  // legacy_version
        serverRandom = r.take(32);
        static const uint8_t hrr[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
                                        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};
        if (std::memcmp(serverRandom.data(), hrr, 32) == 0) throw Error("server asked for a HelloRetryRequest (unsupported)");
        r.take(r.u8());  // session id echo
        info.suiteId = r.u16();
        if (r.u8() != 0) throw Error("server chose a compression method");
        if (!r.empty()) {
            Reader exts = r.sub16();
            while (!exts.empty()) {
                uint16_t type = exts.u16();
                Reader body = exts.sub16();
                if (type == 43) info.version = body.u16();
                else if (type == 51) {
                    info.group = body.u16();
                    if (!body.empty()) info.keyShare = body.take(body.u16());
                } else if (type == 23) info.ems = true;
                else if (type == 16) {
                    Reader list = body.sub16();
                    Reader name = list.sub8();
                    info.alpn.assign(reinterpret_cast<const char*>(name.p), name.left());
                }
            }
        }
        return info;
    }

    // ============================================================ TLS 1.3
    void finishTls13(const ServerHelloInfo& sh, KeyShares& ks) {
        tls13 = true;
        Hash h = suite->hash;
        size_t hl = hashSize(h);
        Bytes shared = ks.agree(sh.group, sh.keyShare);
        Bytes zeros(hl, 0);
        Bytes early = hkdfExtract(h, {}, zeros);
        Bytes derived = hkdfExpandLabel(h, early, "derived", hashOf(h, Bytes()), hl);
        Bytes hs = hkdfExtract(h, derived, shared);
        Bytes cHs = hkdfExpandLabel(h, hs, "c hs traffic", transcriptHash(h), hl);
        Bytes sHs = hkdfExpandLabel(h, hs, "s hs traffic", transcriptHash(h), hl);
        setKeys(rd, suite, sHs);
        setKeys(wr, suite, cHs);  // the client's own Finished is the first thing it sends encrypted

        std::vector<Bytes> chain;
        bool sawCert = false, sawVerify = false, certRequested = false;
        for (;;) {
            Bytes msg = nextHandshake();
            uint8_t type = msg[0];
            Reader body(msg.data() + 4, msg.size() - 4);
            if (type == kEncryptedExtensions) {
                Reader exts = body.sub16();
                while (!exts.empty()) {
                    uint16_t et = exts.u16();
                    Reader eb = exts.sub16();
                    if (et == 16) {
                        Reader list = eb.sub16();
                        Reader name = list.sub8();
                        alpn_.assign(reinterpret_cast<const char*>(name.p), name.left());
                    }
                }
                transcript.insert(transcript.end(), msg.begin(), msg.end());
            } else if (type == kCertificateRequest) {
                certRequested = true;
                transcript.insert(transcript.end(), msg.begin(), msg.end());
            } else if (type == kCertificate) {
                body.take(body.u8());  // certificate_request_context
                Reader list = body.sub24();
                while (!list.empty()) {
                    Reader cert = list.sub24();
                    chain.push_back(cert.take(cert.left()));
                    list.sub16();  // per-certificate extensions (OCSP staple etc.), ignored
                }
                checkCertificates(chain);
                sawCert = true;
                transcript.insert(transcript.end(), msg.begin(), msg.end());
            } else if (type == kCertificateVerify) {
                if (!sawCert) throw Error("CertificateVerify before Certificate");
                uint16_t alg = body.u16();
                Bytes sig = body.take(body.u16());
                Bytes content(64, 0x20);
                const char* ctx = "TLS 1.3, server CertificateVerify";
                content.insert(content.end(), ctx, ctx + std::strlen(ctx));
                content.push_back(0);
                Bytes th = transcriptHash(h);
                content.insert(content.end(), th.begin(), th.end());
                if (opts.verify && !verifyHandshakeSignature(leaf, alg, content, sig, true)) {
                    throw Error("server's CertificateVerify signature is invalid");
                }
                sawVerify = true;
                transcript.insert(transcript.end(), msg.begin(), msg.end());
            } else if (type == kFinished) {
                if (!sawVerify && opts.verify) throw Error("server Finished without authenticating itself");
                Bytes finishedKey = hkdfExpandLabel(h, sHs, "finished", {}, hl);
                Bytes expected = hmac(h, finishedKey, transcriptHash(h));
                Bytes got(msg.begin() + 4, msg.end());
                if (got.size() != expected.size() || !std::equal(got.begin(), got.end(), expected.begin())) {
                    throw Error("server Finished MAC is wrong");
                }
                transcript.insert(transcript.end(), msg.begin(), msg.end());
                break;
            } else {
                throw Error("unexpected handshake message " + std::to_string(type) + " in TLS 1.3 flight");
            }
        }
        // Application secrets come from the transcript up to and including the server Finished.
        Bytes derived2 = hkdfExpandLabel(h, hs, "derived", hashOf(h, Bytes()), hl);
        Bytes master = hkdfExtract(h, derived2, zeros);
        Bytes cAp = hkdfExpandLabel(h, master, "c ap traffic", transcriptHash(h), hl);
        Bytes sAp = hkdfExpandLabel(h, master, "s ap traffic", transcriptHash(h), hl);

        if (certRequested) {  // we have no client certificate: answer with an empty list
            Writer w;
            w.u8(0);
            w.u24(0);
            Bytes msg = {kCertificate, 0, 0, 4};
            msg.insert(msg.end(), w.b.begin(), w.b.end());
            sendRecord(kHandshake, msg);
            transcript.insert(transcript.end(), msg.begin(), msg.end());
        }
        Bytes cFinKey = hkdfExpandLabel(h, cHs, "finished", {}, hl);
        Bytes verify = hmac(h, cFinKey, transcriptHash(h));
        Bytes fin = {kFinished, 0, 0, static_cast<uint8_t>(verify.size())};
        fin.insert(fin.end(), verify.begin(), verify.end());
        sendRecord(kHandshake, fin);
        transcript.insert(transcript.end(), fin.begin(), fin.end());

        setKeys(rd, suite, sAp);
        setKeys(wr, suite, cAp);
        rdSecret = sAp;
        wrSecret = cAp;
    }

    void handlePostHandshake13(const Bytes& payload) {
        hsBuf.insert(hsBuf.end(), payload.begin(), payload.end());
        while (hsBuf.size() >= 4) {
            size_t len = (size_t(hsBuf[1]) << 16) | (size_t(hsBuf[2]) << 8) | hsBuf[3];
            if (hsBuf.size() < 4 + len) return;
            uint8_t type = hsBuf[0];
            Bytes body(hsBuf.begin() + 4, hsBuf.begin() + 4 + static_cast<std::ptrdiff_t>(len));
            hsBuf.erase(hsBuf.begin(), hsBuf.begin() + 4 + static_cast<std::ptrdiff_t>(len));
            if (type == kKeyUpdate) {
                rdSecret = hkdfExpandLabel(suite->hash, rdSecret, "traffic upd", {}, hashSize(suite->hash));
                setKeys(rd, suite, rdSecret);
                if (!body.empty() && body[0] == 1) {  // peer wants us to update too
                    Bytes ku = {kKeyUpdate, 0, 0, 1, 0};
                    sendRecord(kHandshake, ku);
                    wrSecret = hkdfExpandLabel(suite->hash, wrSecret, "traffic upd", {}, hashSize(suite->hash));
                    setKeys(wr, suite, wrSecret);
                }
            }
            // NewSessionTicket and anything else: ignored (no resumption)
        }
    }

    // ============================================================ TLS 1.2
    void finishTls12(const ServerHelloInfo& sh, KeyShares& ks, const Bytes& clientHello) {
        (void)clientHello;
        tls13 = false;
        Hash h = suite->hash;
        bool ems = sh.ems;
        std::vector<Bytes> chain;
        Bytes peerPub;
        uint16_t group = 0;
        bool haveCert = false, haveKx = false, certRequested = false;
        for (;;) {
            Bytes msg = nextHandshake();
            if (msg.empty()) throw Error("unexpected ChangeCipherSpec");
            uint8_t type = msg[0];
            Reader body(msg.data() + 4, msg.size() - 4);
            transcript.insert(transcript.end(), msg.begin(), msg.end());
            if (type == kCertificate) {
                Reader list = body.sub24();
                while (!list.empty()) {
                    Reader cert = list.sub24();
                    chain.push_back(cert.take(cert.left()));
                }
                checkCertificates(chain);
                haveCert = true;
            } else if (type == kServerKeyExchange) {
                if (!haveCert) throw Error("ServerKeyExchange before Certificate");
                size_t paramsStart = 4;
                if (body.u8() != 3) throw Error("server key exchange is not a named curve");
                group = body.u16();
                peerPub = body.take(body.u8());
                size_t paramsLen = 1 + 2 + 1 + peerPub.size();
                uint16_t alg = body.u16();
                Bytes sig = body.take(body.u16());
                Bytes signedData = concat(clientRandom, serverRandom);
                signedData.insert(signedData.end(), msg.begin() + static_cast<std::ptrdiff_t>(paramsStart),
                                  msg.begin() + static_cast<std::ptrdiff_t>(paramsStart + paramsLen));
                if (opts.verify && !verifyHandshakeSignature(leaf, alg, signedData, sig, false)) {
                    throw Error("server's key exchange signature is invalid");
                }
                haveKx = true;
            } else if (type == kCertificateRequest) {
                certRequested = true;
            } else if (type == kServerHelloDone) {
                break;
            } else {
                throw Error("unexpected handshake message " + std::to_string(type) + " in TLS 1.2 flight");
            }
        }
        if (!haveCert || !haveKx) throw Error("incomplete TLS 1.2 server flight");
        if (certRequested) {
            Bytes msg = {kCertificate, 0, 0, 3, 0, 0, 0};
            sendRecord(kHandshake, msg);
            transcript.insert(transcript.end(), msg.begin(), msg.end());
        }
        // ClientKeyExchange
        Bytes premaster = ks.agree(group, peerPub);  // (creates the P-384 key when that group was chosen)
        Bytes myPub = group == kGroupX25519 ? Bytes(ks.x25519Pub, ks.x25519Pub + 32) : group == kGroupP256 ? ks.p256Pub : ks.p384Pub;
        Bytes cke = {kClientKeyExchange, 0, 0, static_cast<uint8_t>(1 + myPub.size()), static_cast<uint8_t>(myPub.size())};
        cke.insert(cke.end(), myPub.begin(), myPub.end());
        sendRecord(kHandshake, cke);
        transcript.insert(transcript.end(), cke.begin(), cke.end());

        Bytes master;
        if (ems) {
            master = tls12Prf(h, premaster, "extended master secret", hashOf(h, transcript), 48);
        } else {
            master = tls12Prf(h, premaster, "master secret", concat(clientRandom, serverRandom), 48);
        }
        bool gcm = suite->aead == Aead::AesGcm;
        size_t ivLen = gcm ? 4 : 12;
        Bytes block = tls12Prf(h, master, "key expansion", concat(serverRandom, clientRandom), 2 * suite->keyLen + 2 * ivLen);
        Bytes cKey(block.begin(), block.begin() + suite->keyLen);
        Bytes sKey(block.begin() + suite->keyLen, block.begin() + 2 * suite->keyLen);
        Bytes cIv(block.begin() + 2 * suite->keyLen, block.begin() + 2 * suite->keyLen + ivLen);
        Bytes sIv(block.begin() + 2 * suite->keyLen + ivLen, block.end());

        // client CCS + Finished
        sendRecord(kChangeCipherSpec, Bytes{1});
        wr.on = true; wr.aead = suite->aead; wr.key = cKey; wr.iv = cIv; wr.seq = 0;
        Bytes cVerify = tls12Prf(h, master, "client finished", hashOf(h, transcript), 12);
        Bytes cFin = {kFinished, 0, 0, 12};
        cFin.insert(cFin.end(), cVerify.begin(), cVerify.end());
        sendRecord(kHandshake, cFin);
        transcript.insert(transcript.end(), cFin.begin(), cFin.end());

        // server CCS + Finished
        Bytes msg = nextHandshake();
        if (!msg.empty() || !ccsSeen) throw Error("expected ChangeCipherSpec from the server");
        rd.on = true; rd.aead = suite->aead; rd.key = sKey; rd.iv = sIv; rd.seq = 0;
        msg = nextHandshake();
        if (msg.empty() || msg[0] != kFinished) throw Error("expected Finished from the server");
        Bytes expected = tls12Prf(h, master, "server finished", hashOf(h, transcript), 12);
        Bytes got(msg.begin() + 4, msg.end());
        if (got.size() != 12 || !std::equal(got.begin(), got.end(), expected.begin())) throw Error("server Finished MAC is wrong");
        transcript.insert(transcript.end(), msg.begin(), msg.end());
    }

    std::string alpn_;

    // ============================================================ handshake
    void handshake(std::string& protocolOut, std::string& cipherOut) {
        KeyShares ks;
        Bytes hello = buildClientHello(ks);
        transcript = hello;
        sendRecord(kHandshake, hello);
        Bytes shMsg = nextHandshake();
        if (shMsg.empty() || shMsg[0] != kServerHello) throw Error("expected ServerHello");
        transcript.insert(transcript.end(), shMsg.begin(), shMsg.end());
        ServerHelloInfo sh = parseServerHello(shMsg);
        suite = findSuite(sh.suiteId);
        if (!suite) throw Error("server chose a cipher suite we did not offer");
        cipherOut = suite->name;
        alpn_ = sh.alpn;
        if (sh.version == 0x0304) {
            if (!suite->tls13) throw Error("TLS 1.3 negotiated with a TLS 1.2 cipher suite");
            protocolOut = "TLSv1.3";
            finishTls13(sh, ks);
        } else if (sh.version == 0x0303) {
            if (suite->tls13) throw Error("TLS 1.2 negotiated with a TLS 1.3 cipher suite");
            // A 1.3-capable server that answers 1.2 to a 1.3-capable client must say so in its random.
            static const uint8_t downgrade[8] = {0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
            if (std::memcmp(serverRandom.data() + 24, downgrade, 8) == 0) throw Error("TLS version downgrade detected");
            protocolOut = "TLSv1.2";
            finishTls12(sh, ks, hello);
        } else {
            throw Error("server negotiated an unsupported TLS version");
        }
        hsBuf.clear();
    }

    // ============================================================= app data
    size_t readApp(uint8_t* buf, size_t len) {
        while (appOff >= appData.size()) {
            if (closed) return 0;
            appData.clear();
            appOff = 0;
            uint8_t type;
            Bytes payload;
            if (!readRecord(type, payload)) {  // TCP EOF without close_notify: treat as end of stream
                closed = true;
                return 0;
            }
            if (type == kApplicationData) {
                appData = std::move(payload);
            } else if (type == kAlert) {
                closed = true;
                if (payload.size() >= 2 && payload[1] == 0) return 0;  // close_notify
                if (payload.size() >= 2 && payload[0] == 1 && payload[1] == 90) return 0;  // user_canceled
                throw Error(alertText(payload));
            } else if (type == kHandshake) {
                if (!tls13) throw Error("unexpected handshake message (renegotiation is not supported)");
                handlePostHandshake13(payload);
            } else if (type == kChangeCipherSpec) {
                continue;
            } else {
                throw Error("unexpected record type");
            }
        }
        size_t n = std::min(len, appData.size() - appOff);
        std::memcpy(buf, appData.data() + appOff, n);
        appOff += n;
        return n;
    }
};

Connection::Connection(Transport transport, const std::string& host, const Options& options) : impl_(new Impl) {
    impl_->io = std::move(transport);
    impl_->host = host;
    impl_->opts = options;
    impl_->handshake(protocol_, cipherName_);
    alpn_ = impl_->alpn_;
}

Connection::~Connection() = default;

void Connection::write(const uint8_t* data, size_t len) {
    if (impl_->closed) throw Error("connection is closed");
    Bytes all(data, data + len);
    impl_->sendRecord(kApplicationData, all);
}

size_t Connection::read(uint8_t* buf, size_t len) { return impl_->readApp(buf, len); }

void Connection::shutdown() {
    if (impl_->closed) return;
    impl_->closed = true;
    try {
        impl_->sendRecord(kAlert, Bytes{1, 0});
    } catch (const std::exception&) {
    }
}

}  // namespace tls
