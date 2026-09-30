// X.509 parsing, chain validation and the trust store for the built-in TLS client.
#include "tls_x509.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tls {
namespace x509 {

using namespace crypto;

namespace {

struct Tlv {
    uint8_t tag = 0;
    const uint8_t* val = nullptr;
    size_t len = 0;
    const uint8_t* start = nullptr;
    size_t total = 0;
};

[[noreturn]] void bad(const char* what) { throw std::runtime_error(std::string("certificate: ") + what); }

// Reads the TLV at `p`, advances past it.
Tlv readTlv(const uint8_t*& p, const uint8_t* end) {
    Tlv t;
    t.start = p;
    if (end - p < 2) bad("truncated");
    t.tag = *p++;
    if ((t.tag & 0x1f) == 0x1f) bad("high tag numbers unsupported");
    size_t len = *p++;
    if (len & 0x80) {
        int nb = len & 0x7f;
        if (nb == 0 || nb > 4 || end - p < nb) bad("bad length");
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | *p++;
    }
    if (static_cast<size_t>(end - p) < len) bad("length past end");
    t.val = p;
    t.len = len;
    p += len;
    t.total = static_cast<size_t>(p - t.start);
    return t;
}

Tlv expect(const uint8_t*& p, const uint8_t* end, uint8_t tag, const char* what) {
    Tlv t = readTlv(p, end);
    if (t.tag != tag) bad(what);
    return t;
}

bool oidEquals(const Tlv& t, std::initializer_list<uint8_t> oid) {
    return t.len == oid.size() && std::equal(oid.begin(), oid.end(), t.val);
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t parseTime(const Tlv& t) {
    std::string s(reinterpret_cast<const char*>(t.val), t.len);
    if (s.empty() || s.back() != 'Z') bad("unsupported time format");
    auto num = [&](size_t off, size_t n) {
        if (off + n > s.size()) bad("short time");
        int v = 0;
        for (size_t i = 0; i < n; i++) {
            if (!std::isdigit(static_cast<unsigned char>(s[off + i]))) bad("bad time digit");
            v = v * 10 + (s[off + i] - '0');
        }
        return v;
    };
    int year, pos;
    if (t.tag == 0x17) {  // UTCTime YYMMDDHHMMSSZ
        int yy = num(0, 2);
        year = yy >= 50 ? 1900 + yy : 2000 + yy;
        pos = 2;
    } else if (t.tag == 0x18) {  // GeneralizedTime YYYYMMDDHHMMSSZ
        year = num(0, 4);
        pos = 4;
    } else {
        bad("unknown time type");
    }
    int mon = num(pos, 2), day = num(pos + 2, 2), hh = num(pos + 4, 2), mm = num(pos + 6, 2), ss = num(pos + 8, 2);
    return daysFromCivil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day)) * 86400 + hh * 3600 + mm * 60 + ss;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Certificate parse(const Bytes& der) {
    Certificate c;
    c.der = der;
    const uint8_t* p = der.data();
    const uint8_t* end = p + der.size();
    Tlv cert = expect(p, end, 0x30, "not a SEQUENCE");
    const uint8_t* q = cert.val;
    const uint8_t* qend = cert.val + cert.len;

    Tlv tbs = expect(q, qend, 0x30, "no tbsCertificate");
    c.tbs.assign(tbs.start, tbs.start + tbs.total);
    Tlv sigAlg = expect(q, qend, 0x30, "no signatureAlgorithm");
    {
        const uint8_t* a = sigAlg.val;
        Tlv oid = expect(a, sigAlg.val + sigAlg.len, 0x06, "bad signature OID");
        c.sigAlgOid.assign(oid.val, oid.val + oid.len);
    }
    Tlv sigVal = expect(q, qend, 0x03, "no signature");
    if (sigVal.len < 1 || sigVal.val[0] != 0) bad("bad signature bit string");
    c.signature.assign(sigVal.val + 1, sigVal.val + sigVal.len);

    const uint8_t* t = tbs.val;
    const uint8_t* tend = tbs.val + tbs.len;
    Tlv next = readTlv(t, tend);
    if (next.tag == 0xa0) next = readTlv(t, tend);  // version
    if (next.tag != 0x02) bad("no serial number");
    expect(t, tend, 0x30, "no inner signature algorithm");
    Tlv issuer = expect(t, tend, 0x30, "no issuer");
    c.issuer.assign(issuer.start, issuer.start + issuer.total);
    Tlv validity = expect(t, tend, 0x30, "no validity");
    {
        const uint8_t* v = validity.val;
        c.notBefore = parseTime(readTlv(v, validity.val + validity.len));
        c.notAfter = parseTime(readTlv(v, validity.val + validity.len));
    }
    Tlv subject = expect(t, tend, 0x30, "no subject");
    c.subject.assign(subject.start, subject.start + subject.total);
    {  // first commonName in the subject
        const uint8_t* r = subject.val;
        const uint8_t* rend = subject.val + subject.len;
        while (r < rend && c.commonName.empty()) {
            Tlv rdn = readTlv(r, rend);
            const uint8_t* a = rdn.val;
            const uint8_t* aend = rdn.val + rdn.len;
            while (a < aend) {
                Tlv atv = readTlv(a, aend);
                const uint8_t* x = atv.val;
                Tlv oid = readTlv(x, atv.val + atv.len);
                Tlv value = readTlv(x, atv.val + atv.len);
                if (oidEquals(oid, {0x55, 0x04, 0x03})) c.commonName.assign(reinterpret_cast<const char*>(value.val), value.len);
            }
        }
    }
    Tlv spki = expect(t, tend, 0x30, "no subjectPublicKeyInfo");
    {
        const uint8_t* s = spki.val;
        const uint8_t* send = spki.val + spki.len;
        Tlv alg = expect(s, send, 0x30, "bad key algorithm");
        Tlv key = expect(s, send, 0x03, "bad public key");
        if (key.len < 1 || key.val[0] != 0) bad("bad public key bits");
        const uint8_t* a = alg.val;
        const uint8_t* aend = alg.val + alg.len;
        Tlv algOid = expect(a, aend, 0x06, "bad key algorithm OID");
        if (oidEquals(algOid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01})) {  // rsaEncryption
            const uint8_t* k = key.val + 1;
            const uint8_t* kend = key.val + key.len;
            Tlv seq = expect(k, kend, 0x30, "bad RSA key");
            const uint8_t* m = seq.val;
            const uint8_t* mend = seq.val + seq.len;
            Tlv n = expect(m, mend, 0x02, "bad RSA modulus");
            Tlv e = expect(m, mend, 0x02, "bad RSA exponent");
            const uint8_t* np = n.val;
            size_t nl = n.len;
            while (nl > 1 && np[0] == 0) { np++; nl--; }
            c.rsaN.assign(np, np + nl);
            const uint8_t* ep = e.val;
            size_t el = e.len;
            while (el > 1 && ep[0] == 0) { ep++; el--; }
            c.rsaE.assign(ep, ep + el);
            c.keyType = KeyType::Rsa;
        } else if (oidEquals(algOid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01})) {  // id-ecPublicKey
            Tlv curve = expect(a, aend, 0x06, "bad EC curve");
            Curve cv;
            if (oidEquals(curve, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07})) {
                cv = Curve::P256;
                c.keyType = KeyType::EcP256;
            } else if (oidEquals(curve, {0x2b, 0x81, 0x04, 0x00, 0x22})) {
                cv = Curve::P384;
                c.keyType = KeyType::EcP384;
            } else {
                c.keyType = KeyType::None;  // an EC curve we don't support: unusable for verification
                cv = Curve::P256;
            }
            if (c.keyType != KeyType::None && !ecDecodePoint(cv, key.val + 1, key.len - 1, c.ecPoint)) bad("invalid EC point");
        }
    }
    while (t < tend) {
        Tlv ext = readTlv(t, tend);
        if (ext.tag != 0xa3) continue;  // issuer/subject unique IDs
        const uint8_t* e = ext.val;
        Tlv list = expect(e, ext.val + ext.len, 0x30, "bad extensions");
        const uint8_t* l = list.val;
        const uint8_t* lend = list.val + list.len;
        while (l < lend) {
            Tlv one = expect(l, lend, 0x30, "bad extension");
            const uint8_t* x = one.val;
            const uint8_t* xend = one.val + one.len;
            Tlv oid = expect(x, xend, 0x06, "bad extension OID");
            Tlv v = readTlv(x, xend);
            if (v.tag == 0x01) v = readTlv(x, xend);  // critical flag
            if (v.tag != 0x04) bad("bad extension value");
            const uint8_t* val = v.val;
            const uint8_t* vend = v.val + v.len;
            if (oidEquals(oid, {0x55, 0x1d, 0x11})) {  // subjectAltName
                c.hasSan = true;
                Tlv names = expect(val, vend, 0x30, "bad SAN");
                const uint8_t* n = names.val;
                const uint8_t* nend = names.val + names.len;
                while (n < nend) {
                    Tlv gn = readTlv(n, nend);
                    if (gn.tag == 0x82) c.dnsNames.emplace_back(reinterpret_cast<const char*>(gn.val), gn.len);
                    else if (gn.tag == 0x87) c.ipAddresses.emplace_back(gn.val, gn.val + gn.len);
                }
            } else if (oidEquals(oid, {0x55, 0x1d, 0x13})) {  // basicConstraints
                c.hasBasicConstraints = true;
                Tlv seq = expect(val, vend, 0x30, "bad basicConstraints");
                if (seq.len > 0) {
                    const uint8_t* b = seq.val;
                    Tlv first = readTlv(b, seq.val + seq.len);
                    if (first.tag == 0x01 && first.len == 1) c.isCa = first.val[0] != 0;
                }
            } else if (oidEquals(oid, {0x55, 0x1d, 0x0f})) {  // keyUsage
                c.hasKeyUsage = true;
                Tlv bits = expect(val, vend, 0x03, "bad keyUsage");
                if (bits.len >= 2) c.keyUsage = static_cast<uint16_t>(bits.val[1] << 8) | (bits.len >= 3 ? bits.val[2] : 0);
            } else if (oidEquals(oid, {0x55, 0x1d, 0x25})) {  // extKeyUsage
                c.hasEku = true;
                Tlv seq = expect(val, vend, 0x30, "bad extKeyUsage");
                const uint8_t* u = seq.val;
                const uint8_t* uend = seq.val + seq.len;
                while (u < uend) {
                    Tlv purpose = readTlv(u, uend);
                    if (oidEquals(purpose, {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01}) ||  // serverAuth
                        oidEquals(purpose, {0x55, 0x1d, 0x25, 0x00}))                                // anyExtendedKeyUsage
                        c.ekuServerAuth = true;
                }
            }
        }
    }
    return c;
}

bool verifySignature(const Certificate& child, const Certificate& issuer) {
    const Bytes& oid = child.sigAlgOid;
    auto is = [&](std::initializer_list<uint8_t> o) { return oid.size() == o.size() && std::equal(o.begin(), o.end(), oid.begin()); };
    Hash h;
    bool rsa;
    if (is({0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b})) { h = Hash::Sha256; rsa = true; }
    else if (is({0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c})) { h = Hash::Sha384; rsa = true; }
    else if (is({0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d})) { h = Hash::Sha512; rsa = true; }
    else if (is({0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02})) { h = Hash::Sha256; rsa = false; }
    else if (is({0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03})) { h = Hash::Sha384; rsa = false; }
    else if (is({0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04})) { h = Hash::Sha512; rsa = false; }
    else return false;  // SHA-1 and anything exotic: refuse
    Bytes digest = hashOf(h, child.tbs);
    if (rsa) {
        return issuer.keyType == KeyType::Rsa && rsaPkcs1Verify(issuer.rsaN, issuer.rsaE, h, digest, child.signature);
    }
    if (issuer.keyType == KeyType::EcP256) return ecdsaVerify(Curve::P256, issuer.ecPoint, digest, child.signature);
    if (issuer.keyType == KeyType::EcP384) return ecdsaVerify(Curve::P384, issuer.ecPoint, digest, child.signature);
    return false;
}

// ------------------------------------------------------------------ PEM ----
Bytes base64Decode(const std::string& s) {
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else continue;  // '=', whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

std::vector<Bytes> pemToDerList(const std::string& pem) {
    std::vector<Bytes> out;
    const std::string begin = "-----BEGIN CERTIFICATE-----", endMark = "-----END CERTIFICATE-----";
    size_t pos = 0;
    while ((pos = pem.find(begin, pos)) != std::string::npos) {
        size_t bodyStart = pos + begin.size();
        size_t e = pem.find(endMark, bodyStart);
        if (e == std::string::npos) break;
        out.push_back(base64Decode(pem.substr(bodyStart, e - bodyStart)));
        pos = e + endMark.size();
    }
    return out;
}

// ---------------------------------------------------------- trust store ----
extern const char* const kEmbeddedRootCertsBase64[];  // tls_ca_bundle.cpp
extern const size_t kEmbeddedRootCertsCount;

TrustStore TrustStore::fromPem(const std::string& pem) {
    TrustStore ts;
    for (const Bytes& der : pemToDerList(pem)) {
        try {
            ts.roots_.push_back(parse(der));
        } catch (const std::exception&) {
            // A root using something we can't parse is simply unusable.
        }
    }
    return ts;
}

const TrustStore& TrustStore::system() {
    static const TrustStore store = [] {
        std::vector<std::string> candidates;
        if (const char* f = std::getenv("SSL_CERT_FILE")) candidates.push_back(f);
        candidates.push_back("/etc/ssl/certs/ca-certificates.crt");      // Debian, Ubuntu, Alpine
        candidates.push_back("/etc/pki/tls/certs/ca-bundle.crt");        // Fedora, RHEL
        candidates.push_back("/etc/ssl/ca-bundle.pem");                  // openSUSE
        candidates.push_back("/etc/ssl/cert.pem");                       // macOS, Alpine, BSD
        if (const char* prefix = std::getenv("PREFIX")) candidates.push_back(std::string(prefix) + "/etc/tls/cert.pem");  // Termux
        candidates.push_back("/data/data/com.termux/files/usr/etc/tls/cert.pem");
        for (const std::string& path : candidates) {
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            std::ostringstream buf;
            buf << f.rdbuf();
            TrustStore ts = fromPem(buf.str());
            if (!ts.roots_.empty()) return ts;
        }
        TrustStore ts;  // nothing on disk: the roots compiled into the binary
        for (size_t i = 0; i < kEmbeddedRootCertsCount; i++) {
            try {
                ts.roots_.push_back(parse(base64Decode(kEmbeddedRootCertsBase64[i])));
            } catch (const std::exception&) {
            }
        }
        return ts;
    }();
    return store;
}

// ------------------------------------------------------------- hostnames ----
namespace {

bool parseIp(const std::string& host, Bytes& out) {
    out.clear();
    if (host.find(':') != std::string::npos) {  // IPv6 (no zone ids)
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : host) {
            if (ch == ':') { parts.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        parts.push_back(cur);
        size_t gap = parts.size();
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i].empty() && i != 0 && i + 1 != parts.size()) { gap = i; break; }
        }
        std::vector<uint16_t> head, tail;
        bool inTail = false;
        for (size_t i = 0; i < parts.size(); i++) {
            if (parts[i].empty()) {
                if (i == gap || (i == 0 && parts.size() > 1 && parts[1].empty()) || (i + 1 == parts.size() && parts[i - 1].empty())) inTail = true;
                continue;
            }
            unsigned long v = std::strtoul(parts[i].c_str(), nullptr, 16);
            (inTail ? tail : head).push_back(static_cast<uint16_t>(v));
        }
        if (head.size() + tail.size() > 8) return false;
        std::vector<uint16_t> words = head;
        words.insert(words.end(), 8 - head.size() - tail.size(), 0);
        words.insert(words.end(), tail.begin(), tail.end());
        for (uint16_t w : words) {
            out.push_back(static_cast<uint8_t>(w >> 8));
            out.push_back(static_cast<uint8_t>(w));
        }
        return true;
    }
    int a[4], n = 0;
    std::stringstream ss(host);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (part.empty() || part.size() > 3 || n >= 4) return false;
        for (char ch : part) if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        a[n++] = std::atoi(part.c_str());
    }
    if (n != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (a[i] > 255) return false;
        out.push_back(static_cast<uint8_t>(a[i]));
    }
    return true;
}

bool dnsMatches(const std::string& patternRaw, const std::string& hostRaw) {
    std::string pattern = lower(patternRaw), host = lower(hostRaw);
    while (!pattern.empty() && pattern.back() == '.') pattern.pop_back();
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (pattern.empty() || host.empty()) return false;
    if (pattern.compare(0, 2, "*.") != 0) return pattern == host;
    // Wildcard: only as the whole left-most label, standing for exactly one label,
    // and the rest must itself have at least two labels (no "*.com").
    std::string suffix = pattern.substr(1);  // ".example.com"
    if (std::count(suffix.begin(), suffix.end(), '.') < 2) return false;
    size_t dot = host.find('.');
    if (dot == std::string::npos || dot == 0) return false;
    return host.substr(dot) == suffix;
}

}  // namespace

bool hostnameMatches(const Certificate& leaf, const std::string& host) {
    Bytes ip;
    if (parseIp(host, ip)) {
        for (const Bytes& a : leaf.ipAddresses) if (a == ip) return true;
        return false;
    }
    if (leaf.hasSan) {
        for (const std::string& n : leaf.dnsNames) if (dnsMatches(n, host)) return true;
        return false;
    }
    return !leaf.commonName.empty() && dnsMatches(leaf.commonName, host);  // legacy: no SAN at all
}

std::string validateChain(const std::vector<Bytes>& chainDer, const std::string& host, int64_t now,
                          const TrustStore& trust, Certificate* leafOut) {
    if (chainDer.empty()) return "server sent no certificate";
    std::vector<Certificate> chain;
    try {
        for (const Bytes& der : chainDer) chain.push_back(parse(der));
    } catch (const std::exception& e) {
        return e.what();
    }
    const Certificate& leaf = chain[0];
    if (leafOut) *leafOut = leaf;
    if (!hostnameMatches(leaf, host)) return "certificate is not valid for host '" + host + "'";
    if (leaf.hasEku && !leaf.ekuServerAuth) return "certificate is not meant for TLS server authentication";
    if (leaf.keyType == KeyType::None) return "certificate uses an unsupported public key";

    const Certificate* cur = &leaf;
    for (int depth = 0; depth < 8; depth++) {
        if (now < cur->notBefore) return "certificate is not valid yet";
        if (now > cur->notAfter) return "certificate has expired";
        // Anchored in the trust store?
        for (const Certificate& root : trust.roots()) {
            if (root.der == cur->der) return "";  // the server sent a root we trust
            if (root.subject == cur->issuer && verifySignature(*cur, root)) return "";
        }
        // Otherwise climb through the intermediates the server provided.
        const Certificate* parent = nullptr;
        for (size_t i = 1; i < chain.size(); i++) {
            const Certificate& cand = chain[i];
            if (&cand == cur || cand.subject != cur->issuer) continue;
            if (!cand.hasBasicConstraints || !cand.isCa) continue;
            if (cand.hasKeyUsage && !(cand.keyUsage & 0x0400)) continue;  // keyCertSign is bit 5 (0x04 of the first byte)
            if (!verifySignature(*cur, cand)) continue;
            parent = &cand;
            break;
        }
        if (!parent) return "unable to build a trusted chain (unknown issuer)";
        cur = parent;
    }
    return "certificate chain is too long";
}

}  // namespace x509
}  // namespace tls
