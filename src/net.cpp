#include "net.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <cctype>

#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "gil.hpp"

#ifndef __EMSCRIPTEN__
#include "bearssl.h"
extern "C" {
const br_x509_trust_anchor* nusaCaTrustAnchors(void);
size_t nusaCaTrustAnchorsCount(void);
}
#endif

namespace net {

namespace {
// Fds handed out by tcpConnect() that haven't been closed yet -- see
// closeAllTracked() / net.hpp docs. Single-threaded interpreter, so a
// plain vector + linear scan is plenty (connection counts are tiny).
std::vector<int> g_openFds;
}  // namespace

#ifdef __EMSCRIPTEN__

int tcpConnect(const std::string&, int) {
    throw std::runtime_error("tcp_konek(): socket jaringan tidak didukung di WASM browser");
}

long tcpSend(int, const std::string&) {
    throw std::runtime_error("tcp_kirim(): socket jaringan tidak didukung di WASM browser");
}

std::string tcpRecv(int, int) {
    throw std::runtime_error("tcp_baca(): socket jaringan tidak didukung di WASM browser");
}

void tcpClose(int) {}

void closeAllTracked() {}

HttpResponse httpRequest(const std::string&, const std::string&, const std::string&, const std::string&,
                          const std::unordered_map<std::string, std::string>&) {
    throw std::runtime_error("minta_http(): jaringan socket tidak didukung di WASM browser");
}

bool smtpSend(const std::string&, int, const std::string&, const std::string&, const std::string&, const std::string&) {
    throw std::runtime_error("kirim_email(): socket tidak didukung di WASM browser");
}

void httpServe(int, const std::function<HttpResponseOut(const HttpRequestIn&)>&) {
    throw std::runtime_error("http_dengar(): server HTTP tidak didukung di WASM browser");
}

#else

int tcpConnect(const std::string& host, int port) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    int gaiResult;
    {
        // DNS lookup + the connect() below both block -- release the
        // GIL so other goroutines get to run while this one waits.
        GilRelease release;
        gaiResult = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    }
    if (gaiResult != 0) return -1;

    int fd = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int connectResult;
        {
            GilRelease release;
            connectResult = connect(fd, p->ai_addr, p->ai_addrlen);
        }
        if (connectResult == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) g_openFds.push_back(fd);
    return fd;
}

long tcpSend(int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = send(fd, data.data() + total, data.size() - total, 0);
        if (n <= 0) return -1;
        total += static_cast<size_t>(n);
    }
    return static_cast<long>(total);
}

std::string tcpRecv(int fd, int maxLen) {
    if (maxLen <= 0) return "";
    std::string buf(static_cast<size_t>(maxLen), '\0');
    ssize_t n;
    {
        GilRelease release;
        n = recv(fd, buf.data(), buf.size(), 0);
    }
    if (n <= 0) return "";
    buf.resize(static_cast<size_t>(n));
    return buf;
}

void tcpClose(int fd) {
    if (fd < 0) return;
    close(fd);
    g_openFds.erase(std::remove(g_openFds.begin(), g_openFds.end(), fd), g_openFds.end());
}

void closeAllTracked() {
    for (int fd : g_openFds) close(fd);
    g_openFds.clear();
}

namespace {

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
    bool tls = false;
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl p;
    std::string rest = url;
    const std::string httpPrefix = "http://";
    const std::string httpsPrefix = "https://";
    if (rest.rfind(httpPrefix, 0) == 0) {
        rest = rest.substr(httpPrefix.size());
    } else if (rest.rfind(httpsPrefix, 0) == 0) {
        rest = rest.substr(httpsPrefix.size());
        p.tls = true;
        p.port = 443;
    }
    size_t slashPos = rest.find('/');
    std::string hostPort = slashPos == std::string::npos ? rest : rest.substr(0, slashPos);
    p.path = slashPos == std::string::npos ? "/" : rest.substr(slashPos);
    size_t colonPos = hostPort.find(':');
    if (colonPos == std::string::npos) {
        p.host = hostPort;
    } else {
        p.host = hostPort.substr(0, colonPos);
        p.port = std::atoi(hostPort.substr(colonPos + 1).c_str());
    }
    if (p.host.empty()) throw std::runtime_error("URL nggak valid: '" + url + "'");
    return p;
}

// br_sslio_* callback pair -- same shape as BearSSL's own samples/client_basic.c,
// just released the GIL around the blocking syscall like every other
// socket read/write in this file.
int tlsSockRead(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    for (;;) {
        ssize_t n;
        {
            GilRelease release;
            n = recv(fd, buf, len, 0);
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;  // EOF
        return static_cast<int>(n);
    }
}

int tlsSockWrite(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    for (;;) {
        ssize_t n;
        {
            GilRelease release;
            n = send(fd, buf, len, 0);
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return static_cast<int>(n);
    }
}

}  // namespace

HttpResponse httpRequest(const std::string& method, const std::string& url,
                          const std::string& body, const std::string& contentType,
                          const std::unordered_map<std::string, std::string>& headers) {
    ParsedUrl u = parseUrl(url);
    int fd = tcpConnect(u.host, u.port);
    if (fd < 0) throw std::runtime_error("gagal konek ke " + u.host);

    std::ostringstream req;
    req << method << " " << u.path << " HTTP/1.1\r\n";
    req << "Host: " << u.host << "\r\n";
    req << "Connection: close\r\n";
    req << "User-Agent: nusantara/1.0\r\n";
    if (method == "POST") {
        req << "Content-Type: " << (contentType.empty() ? "text/plain" : contentType) << "\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    for (const auto& [k, v] : headers) {
        req << k << ": " << v << "\r\n";
    }
    req << "\r\n";
    if (method == "POST") req << body;
    std::string reqStr = req.str();

    std::string raw;

    if (u.tls) {
        br_ssl_client_context sc;
        br_x509_minimal_context xc;
        std::vector<unsigned char> iobuf(BR_SSL_BUFSIZE_BIDI);
        br_sslio_context ioc;

        br_ssl_client_init_full(&sc, &xc, nusaCaTrustAnchors(), nusaCaTrustAnchorsCount());
        br_ssl_engine_set_buffer(&sc.eng, iobuf.data(), iobuf.size(), 1);
        br_ssl_client_reset(&sc, u.host.c_str(), 0);
        br_sslio_init(&ioc, &sc.eng, tlsSockRead, &fd, tlsSockWrite, &fd);

        bool sendOk = br_sslio_write_all(&ioc, reqStr.data(), reqStr.size()) == 0;
        if (sendOk) br_sslio_flush(&ioc);

        if (sendOk) {
            char chunk[4096];
            for (;;) {
                int n = br_sslio_read(&ioc, reinterpret_cast<unsigned char*>(chunk), sizeof(chunk));
                if (n < 0) break;
                raw.append(chunk, static_cast<size_t>(n));
            }
        }
        tcpClose(fd);

        if (raw.empty()) {
            int err = br_ssl_engine_last_error(&sc.eng);
            throw std::runtime_error("TLS/HTTPS gagal ke " + u.host + " (kode error BearSSL " + std::to_string(err) + ")");
        }
    } else {
        if (tcpSend(fd, reqStr) < 0) {
            tcpClose(fd);
            throw std::runtime_error("gagal kirim request HTTP");
        }
        char chunk[4096];
        while (true) {
            ssize_t n;
            {
                GilRelease release;
                n = recv(fd, chunk, sizeof(chunk), 0);
            }
            if (n <= 0) break;
            raw.append(chunk, static_cast<size_t>(n));
        }
        tcpClose(fd);
    }

    // Just enough parsing to split status line / headers / body -- no
    // chunked transfer-encoding support, that's future work.
    HttpResponse resp;
    size_t headerEnd = raw.find("\r\n\r\n");
    std::string headerPart = headerEnd == std::string::npos ? raw : raw.substr(0, headerEnd);
    resp.body = headerEnd == std::string::npos ? "" : raw.substr(headerEnd + 4);

    size_t firstSpace = headerPart.find(' ');
    if (firstSpace != std::string::npos) {
        resp.status = std::atoi(headerPart.c_str() + firstSpace + 1);
    }
    return resp;
}

namespace {

std::string smtpReadReply(int fd) {
    std::string buf(4096, '\0');
    ssize_t n;
    {
        GilRelease release;
        n = recv(fd, buf.data(), buf.size(), 0);
    }
    if (n <= 0) return "";
    buf.resize(static_cast<size_t>(n));
    return buf;
}

bool smtpOk(const std::string& reply) {
    return reply.size() >= 3 && (reply[0] == '2' || reply[0] == '3');
}

}  // namespace

bool smtpSend(const std::string& host, int port, const std::string& from,
              const std::string& to, const std::string& subject, const std::string& body) {
    int fd = tcpConnect(host, port);
    if (fd < 0) return false;

    auto sendLine = [&](const std::string& line) { return tcpSend(fd, line + "\r\n") >= 0; };
    auto step = [&](const std::string& line) {
        return sendLine(line) && smtpOk(smtpReadReply(fd));
    };

    bool ok = smtpOk(smtpReadReply(fd)) &&                          // server greeting
              step("EHLO nusantara") &&
              step("MAIL FROM:<" + from + ">") &&
              step("RCPT TO:<" + to + ">") &&
              step("DATA");

    if (ok) {
        std::ostringstream msg;
        msg << "From: " << from << "\r\n";
        msg << "To: " << to << "\r\n";
        msg << "Subject: " << subject << "\r\n";
        msg << "\r\n" << body << "\r\n.";
        ok = sendLine(msg.str()) && smtpOk(smtpReadReply(fd));
    }

    sendLine("QUIT");
    tcpClose(fd);
    return ok;
}

namespace {

std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads one full HTTP/1.1 request off `fd`. No chunked encoding, no
// pipelining/keep-alive -- just enough for a simple JSON REST API.
bool readHttpRequestIn(int fd, HttpRequestIn& out) {
    std::string raw;
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n;
        {
            GilRelease release;
            n = recv(fd, chunk, sizeof(chunk), 0);
        }
        if (n <= 0) return false;
        raw.append(chunk, static_cast<size_t>(n));
        headerEnd = raw.find("\r\n\r\n");
        if (raw.size() > (1u << 20)) return false;  // 1MB header guard
    }

    std::string headerPart = raw.substr(0, headerEnd);
    std::string bodySoFar = raw.substr(headerEnd + 4);

    std::istringstream headerStream(headerPart);
    std::string requestLine;
    std::getline(headerStream, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
    std::istringstream lineStream(requestLine);
    lineStream >> out.method >> out.path;  // ignores the trailing "HTTP/1.1" token
    if (out.method.empty() || out.path.empty()) return false;

    std::string headerLine;
    while (std::getline(headerStream, headerLine)) {
        if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
        if (headerLine.empty()) continue;
        size_t colon = headerLine.find(':');
        if (colon == std::string::npos) continue;
        std::string key = toLowerAscii(headerLine.substr(0, colon));
        size_t valueStart = headerLine.find_first_not_of(' ', colon + 1);
        out.headers[key] = valueStart == std::string::npos ? "" : headerLine.substr(valueStart);
    }

    size_t contentLength = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) contentLength = static_cast<size_t>(std::atoi(it->second.c_str()));

    out.body = bodySoFar;
    while (out.body.size() < contentLength) {
        ssize_t n;
        {
            GilRelease release;
            n = recv(fd, chunk, sizeof(chunk), 0);
        }
        if (n <= 0) break;
        out.body.append(chunk, static_cast<size_t>(n));
    }
    if (out.body.size() > contentLength) out.body.resize(contentLength);
    return true;
}

}  // namespace

void httpServe(int port, const std::function<HttpResponseOut(const HttpRequestIn&)>& onRequest) {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) throw std::runtime_error("http_dengar(): gagal bikin socket");
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<unsigned short>(port));

    if (bind(listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listenFd);
        throw std::runtime_error("http_dengar(): gagal bind ke port " + std::to_string(port) + " (kepake proses lain?)");
    }
    if (listen(listenFd, 16) < 0) {
        close(listenFd);
        throw std::runtime_error("http_dengar(): gagal listen di port " + std::to_string(port));
    }

    static const std::unordered_map<int, std::string> kReasons = {
        {200, "OK"}, {201, "Created"}, {204, "No Content"}, {400, "Bad Request"},
        {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"},
        {405, "Method Not Allowed"}, {500, "Internal Server Error"},
    };

    while (true) {
        int connFd;
        struct sockaddr_in peerAddr {};
        socklen_t peerLen = sizeof(peerAddr);
        {
            // Can block indefinitely -- release the GIL so earlier
            // connections' goroutines can run while we wait.
            GilRelease release;
            connFd = accept(listenFd, reinterpret_cast<struct sockaddr*>(&peerAddr), &peerLen);
        }
        if (connFd < 0) continue;

        char ipBuf[INET_ADDRSTRLEN] = {0};
        std::string peerIp = inet_ntop(AF_INET, &peerAddr.sin_addr, ipBuf, sizeof(ipBuf)) ? ipBuf : "";

        // Each connection gets its own thread; no cap on concurrent
        // connections yet. `onRequest` copied by value (not by reference)
        // since httpServe() only unwinds if std::thread's constructor throws.
        auto handleOneConnection = [connFd, onRequest, peerIp]() {
            // Must lock the GIL before readHttpRequestIn() -- its internal
            // GilRelease assumes the GIL is already held.
            GIL::instance().lock();

            HttpRequestIn req;
            HttpResponseOut resp;
            req.fd = connFd;
            req.ip = peerIp;
            if (readHttpRequestIn(connFd, req)) {
                resp = onRequest(req);
            } else {
                resp.status = 400;
                resp.contentType = "text/plain";
                resp.body = "Bad Request";
            }

            if (!resp.streamed) {
                auto reasonIt = kReasons.find(resp.status);
                std::string reason = reasonIt != kReasons.end() ? reasonIt->second : "Unknown";

                std::ostringstream out;
                out << "HTTP/1.1 " << resp.status << " " << reason << "\r\n";
                out << "Content-Type: " << resp.contentType << "\r\n";
                out << "Content-Length: " << resp.body.size() << "\r\n";
                out << "X-Frame-Options: DENY\r\n";
                out << "X-Content-Type-Options: nosniff\r\n";
                out << "Referrer-Policy: strict-origin-when-cross-origin\r\n";
                out << "X-XSS-Protection: 1; mode=block\r\n";
                out << "X-Powered-By: next-ns (Nusantara WebAssembly)\r\n";
                out << "Connection: close\r\n\r\n";
                out << resp.body;
                tcpSend(connFd, out.str());
                close(connFd);
            }
            // resp.streamed == true: handler already wrote its response and
            // closed connFd itself via tcp_kirim()/tcp_tutup().

            GIL::instance().unlock();
        };
        try {
            std::thread(handleOneConnection).detach();
        } catch (const std::exception&) {
            // Thread creation can fail under resource exhaustion -- drop
            // this connection and keep accepting rather than unwind.
            close(connFd);
        }
    }
}
#endif

}  // namespace net
