#pragma once

// X.509 certificate parsing and chain validation for the built-in TLS client.

#include <cstdint>
#include <string>
#include <vector>

#include "tls_crypto.hpp"

namespace tls {
namespace x509 {

using crypto::Bytes;

enum class KeyType { None, Rsa, EcP256, EcP384 };

struct Certificate {
    Bytes der;
    Bytes tbs;           // the signed part, exactly as encoded
    Bytes issuer, subject;  // DER of the Name, compared bytewise
    Bytes sigAlgOid;     // outer signatureAlgorithm OID contents
    Bytes signature;     // BIT STRING contents without the unused-bits byte
    int64_t notBefore = 0, notAfter = 0;  // seconds since the Unix epoch
    KeyType keyType = KeyType::None;
    Bytes rsaN, rsaE;
    crypto::EcPoint ecPoint;
    std::vector<std::string> dnsNames;
    std::vector<Bytes> ipAddresses;
    std::string commonName;
    bool hasSan = false;
    bool hasBasicConstraints = false, isCa = false;
    bool hasKeyUsage = false;
    uint16_t keyUsage = 0;  // bit i of the first two bytes, MSB-first as in the BIT STRING
    bool hasEku = false, ekuServerAuth = false;
};

// Parses one DER certificate. Throws std::runtime_error on malformed input.
Certificate parse(const Bytes& der);

// Verifies `child`'s signature with `issuer`'s public key.
bool verifySignature(const Certificate& child, const Certificate& issuer);

// Trusted root certificates.
class TrustStore {
public:
    // System bundle if one exists ($SSL_CERT_FILE first), else the embedded one.
    static const TrustStore& system();
    static TrustStore fromPem(const std::string& pemText);
    const std::vector<Certificate>& roots() const { return roots_; }
    void add(Certificate c) { roots_.push_back(std::move(c)); }

private:
    std::vector<Certificate> roots_;
};

// True if `host` is covered by the certificate (SAN dNSName with left-most
// wildcard, or an IP literal against iPAddress entries).
bool hostnameMatches(const Certificate& leaf, const std::string& host);

// Validates the server-supplied chain (leaf first) against `trust` for `host` at
// `now` (Unix seconds). Returns "" on success or a human-readable reason.
// Not checked: revocation (CRL/OCSP), name constraints, path-length constraints.
std::string validateChain(const std::vector<Bytes>& chainDer, const std::string& host, int64_t now,
                          const TrustStore& trust, Certificate* leafOut = nullptr);

// PEM helpers.
std::vector<Bytes> pemToDerList(const std::string& pemText);
Bytes base64Decode(const std::string& s);

}  // namespace x509
}  // namespace tls
