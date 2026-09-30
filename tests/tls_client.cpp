// Small TLS test client:  tls_client <host> <port> [--insecure] [--cafile file] [--path /x] [--alpn h2]
// Prints the negotiated protocol/cipher, then the HTTP response of a GET.
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "tls.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "pakai: tls_client <host> <port> [--insecure] [--cafile f] [--path /x]\n");
        return 2;
    }
    std::string host = argv[1], path = "/";
    tls::Options opts;
    tls::x509::TrustStore custom;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--insecure") opts.verify = false;
        else if (a == "--cafile" && i + 1 < argc) {
            std::ifstream f(argv[++i]);
            std::ostringstream b;
            b << f.rdbuf();
            custom = tls::x509::TrustStore::fromPem(b.str());
            opts.trust = &custom;
        } else if (a == "--path" && i + 1 < argc) path = argv[++i];
        else if (a == "--alpn" && i + 1 < argc) opts.alpn.push_back(argv[++i]);
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), argv[2], &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "resolve gagal\n");
        return 1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        std::perror("connect");
        return 1;
    }
    freeaddrinfo(res);
    tls::Transport t;
    t.recv = [fd](uint8_t* b, size_t n) -> long { return ::recv(fd, b, n, 0); };
    t.send = [fd](const uint8_t* b, size_t n) -> long { return ::send(fd, b, n, 0); };
    try {
        tls::Connection c(t, host, opts);
        std::printf("== %s  %s  alpn=%s\n", c.protocol().c_str(), c.cipherSuite().c_str(), c.alpn().c_str());
        c.write("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\nUser-Agent: nusa-tls-test\r\n\r\n");
        std::string all;
        uint8_t buf[8192];
        size_t n;
        while ((n = c.read(buf, sizeof buf)) > 0) all.append(reinterpret_cast<char*>(buf), n);
        size_t eol = all.find("\r\n");
        std::printf("== %zu bytes; %s\n", all.size(), all.substr(0, eol).c_str());
        if (std::getenv("TLS_SHOW_BODY")) std::printf("%s\n", all.c_str());
        c.shutdown();
    } catch (const std::exception& e) {
        std::printf("GAGAL: %s\n", e.what());
        return 1;
    }
    close(fd);
    return 0;
}
