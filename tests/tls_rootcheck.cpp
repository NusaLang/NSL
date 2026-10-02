// Real-world check of the X.509 code without network access: every root certificate in the
// Mozilla bundle is self-signed, so verifying each one's signature with its own key exercises
// the actual key types and signature algorithms used on the public web.
//   tls_rootcheck <bundle.pem>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "tls_x509.hpp"

int main(int argc, char** argv) {
    std::ifstream f(argc > 1 ? argv[1] : "/etc/ssl/certs/ca-certificates.crt");
    std::ostringstream buf;
    buf << f.rdbuf();
    auto ders = tls::x509::pemToDerList(buf.str());
    std::map<std::string, int> okBy, failBy;
    int parsed = 0, verified = 0, failed = 0, parseFail = 0;
    for (const auto& der : ders) {
        tls::x509::Certificate c;
        try {
            c = tls::x509::parse(der);
        } catch (const std::exception& e) {
            parseFail++;
            std::printf("parse gagal: %s\n", e.what());
            continue;
        }
        parsed++;
        const char* kt = c.keyType == tls::x509::KeyType::Rsa ? "RSA" : c.keyType == tls::x509::KeyType::EcP256 ? "P-256"
                         : c.keyType == tls::x509::KeyType::EcP384 ? "P-384" : "lain";
        if (c.subject != c.issuer) continue;  // not self-signed (cross-signed root)
        std::string label = std::string(kt) + " (" + std::to_string(c.rsaN.size() * 8) + " bit)";
        if (c.keyType != tls::x509::KeyType::Rsa) label = kt;
        if (tls::x509::verifySignature(c, c)) {
            verified++;
            okBy[label]++;
        } else {
            failed++;
            failBy[label + " alg=" + std::to_string(c.sigAlgOid.empty() ? 0 : c.sigAlgOid.back())]++;
        }
    }
    std::printf("%zu sertifikat, %d berhasil di-parse (%d gagal), self-signature valid: %d, ditolak: %d\n", ders.size(), parsed, parseFail, verified, failed);
    for (auto& [k, v] : okBy) std::printf("  valid   %-18s %d\n", k.c_str(), v);
    for (auto& [k, v] : failBy) std::printf("  ditolak %-24s %d\n", k.c_str(), v);
    return parseFail ? 1 : 0;
}
