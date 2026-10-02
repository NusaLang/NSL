// Validates a PEM certificate chain (leaf first) for a host against the system trust store:
//   tls_chaintest <chain.pem> <host> [unix-time]
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "tls_x509.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "pakai: tls_chaintest <chain.pem> <host> [waktu-unix]\n");
        return 2;
    }
    std::ifstream f(argv[1]);
    std::ostringstream buf;
    buf << f.rdbuf();
    auto chain = tls::x509::pemToDerList(buf.str());
    int64_t now = argc > 3 ? std::atoll(argv[3]) : static_cast<int64_t>(std::time(nullptr));
    const auto& trust = tls::x509::TrustStore::system();
    std::string err = tls::x509::validateChain(chain, argv[2], now, trust);
    std::printf("%s: %zu sertifikat, %zu akar tepercaya -> %s\n", argv[2], chain.size(), trust.roots().size(),
                err.empty() ? "VALID" : err.c_str());
    return err.empty() ? 0 : 1;
}
