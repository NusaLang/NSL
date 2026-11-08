#pragma once

// A TLS 1.2 / 1.3 client implemented from scratch (no OpenSSL/BearSSL).
//
//   * key exchange: X25519 and secp256r1 (ECDHE)
//   * AEAD: AES-128/256-GCM and ChaCha20-Poly1305
//   * server authentication: certificate chain to the system (or embedded) roots,
//     host name check, and the handshake signature (RSA-PSS/PKCS#1, ECDSA P-256/P-384)
//
// Not implemented: client certificates, session resumption, 0-RTT, renegotiation,
// HelloRetryRequest, revocation checking. See tls_x509.hpp for what validation covers.

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "tls_x509.hpp"

namespace tls {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& msg) : std::runtime_error(msg) {}
};

// Thrown by Connection::read() when the transport reported "no data yet" (recv returned -2):
// nothing is lost, call read() again once the socket is readable.
class WouldBlock : public Error {
public:
    WouldBlock() : Error("would block") {}
};

// Byte transport underneath the TLS session. Each returns the number of bytes moved;
// recv returns 0 at end of stream, -2 for "would block" (only meaningful after the handshake),
// any other negative value on failure.
struct Transport {
    std::function<long(uint8_t* buf, size_t len)> recv;
    std::function<long(const uint8_t* buf, size_t len)> send;
};

struct Options {
    bool verify = true;                          // false disables ALL certificate checks (tests only)
    int64_t now = 0;                             // Unix time for validity checks; 0 = current time
    const x509::TrustStore* trust = nullptr;     // nullptr = x509::TrustStore::system()
    std::vector<std::string> alpn;               // e.g. {"http/1.1"}
};

class Connection {
public:
    // Performs the handshake. Throws tls::Error if it fails or the server can't be authenticated.
    Connection(Transport transport, const std::string& host, const Options& options = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Sends every byte of `data`.
    void write(const uint8_t* data, size_t len);
    void write(const std::string& s) { write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    // Up to `len` bytes of application data; 0 means the peer closed the connection.
    size_t read(uint8_t* buf, size_t len);
    // Sends close_notify.
    void shutdown();

    const std::string& protocol() const { return protocol_; }      // "TLSv1.3" / "TLSv1.2"
    const std::string& cipherSuite() const { return cipherName_; }
    const std::string& alpn() const { return alpn_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string protocol_, cipherName_, alpn_;
};

}  // namespace tls
