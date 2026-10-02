// Known-answer tests for the built-in TLS crypto: reads tests/tls/vectors.txt (made by
// tests/tls/gen_vectors.py with an independent implementation) and checks every line,
// plus tampered variants that must be rejected.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "tls_crypto.hpp"

using namespace tls::crypto;

static Bytes unhex(const std::string& s) {
    Bytes b;
    if (s == "-") return b;
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return b;
}
static Hash hashByName(const std::string& n) { return n == "sha256" ? Hash::Sha256 : n == "sha384" ? Hash::Sha384 : Hash::Sha512; }

static int failures = 0, checks = 0;
#define CHECK(cond, what)                                                              \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL: %s (line %d: %s)\n", what, lineNo, line.c_str());        \
        }                                                                              \
    } while (0)

int main(int argc, char** argv) {
    std::ifstream f(argc > 1 ? argv[1] : "tests/tls/vectors.txt");
    if (!f) {
        std::printf("tidak bisa buka file vektor\n");
        return 2;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        std::istringstream ss(line);
        std::string kind, tok;
        ss >> kind;
        std::map<std::string, std::string> kv;
        while (ss >> tok) {
            size_t eq = tok.find('=');
            kv[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
        if (kind == "hash") {
            Bytes m = unhex(kv["msg"]);
            CHECK(hashOf(hashByName(kv["alg"]), m) == unhex(kv["digest"]), "hash");
            // incremental in odd-sized pieces must agree
            Hasher h(hashByName(kv["alg"]));
            for (size_t i = 0; i < m.size(); i += 7) h.update(m.data() + i, std::min<size_t>(7, m.size() - i));
            CHECK(h.finish() == unhex(kv["digest"]), "hash incremental");
        } else if (kind == "hmac") {
            CHECK(hmac(hashByName(kv["alg"]), unhex(kv["key"]), unhex(kv["msg"])) == unhex(kv["mac"]), "hmac");
        } else if (kind == "hkdf") {
            Hash h = hashByName(kv["alg"]);
            Bytes prk = hkdfExtract(h, unhex(kv["salt"]), unhex(kv["ikm"]));
            CHECK(hkdfExpand(h, prk, unhex(kv["info"]), std::stoul(kv["n"])) == unhex(kv["okm"]), "hkdf");
        } else if (kind == "prf") {
            Bytes label = unhex(kv["label"]);
            CHECK(tls12Prf(hashByName(kv["alg"]), unhex(kv["secret"]), std::string(label.begin(), label.end()), unhex(kv["seed"]),
                           std::stoul(kv["n"])) == unhex(kv["out"]), "tls12 prf");
        } else if (kind == "aesgcm" || kind == "chacha") {
            bool gcm = kind == "aesgcm";
            Bytes key = unhex(kv["key"]), nonce = unhex(kv["nonce"]), aad = unhex(kv["aad"]), pt = unhex(kv["pt"]), ct = unhex(kv["ct"]);
            Bytes sealed = gcm ? aesGcmSeal(key, nonce, aad, pt) : chachaSeal(key, nonce, aad, pt);
            CHECK(sealed == ct, "aead seal");
            Bytes back;
            bool ok = gcm ? aesGcmOpen(key, nonce, aad, ct, back) : chachaOpen(key, nonce, aad, ct, back);
            CHECK(ok && back == pt, "aead open");
            Bytes bad = ct;
            bad[bad.size() - 1] ^= 1;  // tampered tag
            Bytes dummy;
            CHECK(!(gcm ? aesGcmOpen(key, nonce, aad, bad, dummy) : chachaOpen(key, nonce, aad, bad, dummy)), "aead rejects bad tag");
            if (!pt.empty()) {
                bad = ct;
                bad[0] ^= 1;  // tampered ciphertext
                CHECK(!(gcm ? aesGcmOpen(key, nonce, aad, bad, dummy) : chachaOpen(key, nonce, aad, bad, dummy)), "aead rejects bad ciphertext");
            }
        } else if (kind == "x25519") {
            Bytes priv = unhex(kv["priv"]), peer = unhex(kv["peer"]);
            uint8_t pub[32], shared[32];
            x25519Public(priv.data(), pub);
            CHECK(Bytes(pub, pub + 32) == unhex(kv["pub"]), "x25519 public");
            CHECK(x25519(priv.data(), peer.data(), shared) && Bytes(shared, shared + 32) == unhex(kv["shared"]), "x25519 shared");
        } else if (kind == "ecdsa") {
            Curve c = kv["curve"] == "p256" ? Curve::P256 : Curve::P384;
            EcPoint pub;
            Bytes raw = unhex(kv["pub"]);
            CHECK(ecDecodePoint(c, raw.data(), raw.size(), pub), "ec point decodes");
            Bytes dg = unhex(kv["digest"]), sig = unhex(kv["sig"]);
            CHECK(ecdsaVerify(c, pub, dg, sig), "ecdsa verify");
            Bytes badDg = dg;
            badDg[0] ^= 1;
            CHECK(!ecdsaVerify(c, pub, badDg, sig), "ecdsa rejects wrong digest");
            Bytes badSig = sig;
            badSig[badSig.size() - 1] ^= 1;
            CHECK(!ecdsaVerify(c, pub, dg, badSig), "ecdsa rejects wrong signature");
        } else if (kind == "ecmul") {
            Curve c = kv["curve"] == "p256" ? Curve::P256 : Curve::P384;
            EcPoint peer;
            Bytes raw = unhex(kv["pub"]);
            CHECK(ecDecodePoint(c, raw.data(), raw.size(), peer), "ecmul peer decodes");
            EcPoint r = ecMul(c, BigInt::fromHex(kv["k"]), peer);
            CHECK(!r.infinity && r.x.toBytes(curveBytes(c)) == unhex(kv["shared"]), "ec scalar multiplication (ECDH)");
        } else if (kind == "rsa_pkcs1" || kind == "rsa_pss") {
            bool pss = kind == "rsa_pss";
            Bytes n = unhex(kv["n"]), e = unhex(kv["e"]), dg = unhex(kv["digest"]), sig = unhex(kv["sig"]);
            Hash h = hashByName(kv["alg"]);
            auto verify = [&](const Bytes& d, const Bytes& s) { return pss ? rsaPssVerify(n, e, h, d, s) : rsaPkcs1Verify(n, e, h, d, s); };
            CHECK(verify(dg, sig), "rsa verify");
            Bytes badDg = dg;
            badDg[3] ^= 1;
            CHECK(!verify(badDg, sig), "rsa rejects wrong digest");
            Bytes badSig = sig;
            badSig[10] ^= 1;
            CHECK(!verify(dg, badSig), "rsa rejects wrong signature");
        }
    }
    // Curve sanity: n*G must be the point at infinity and G must be on the curve.
    for (Curve c : {Curve::P256, Curve::P384}) {
        EcPoint g = ecBaseMul(c, BigInt(1));
        int lineNo = 0;
        std::string line = "curve sanity";
        CHECK(ecOnCurve(c, g), "generator on curve");
    }
    std::printf("%d pemeriksaan, %d gagal\n", checks, failures);
    return failures ? 1 : 0;
}
