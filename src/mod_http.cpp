// Plugin HTTP client (HTTP/1.1) dengan TLS bawaan (src/tls.cpp, tanpa libcurl/OpenSSL).
// `minta()` buffer seluruh body di memori dan mengikuti redirect; `minta_stream()`/`baca_stream()`/
// `tutup_stream()` menjalankan transfer di thread terpisah dan mengalirkan body lewat antrean
// mutex+condvar, buat body besar yang tidak mau ditunggu sekaligus. Mendukung proxy lewat
// http_proxy/https_proxy/no_proxy (CONNECT untuk https).

#include "plugin_abi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "json.hpp"
#include "tls.hpp"
#include "value.hpp"

namespace {

std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string joinUrl(const std::string& apiUrl, const std::string& path) {
    if (apiUrl.empty()) return path;
    if (path.empty()) return apiUrl;
    bool apiEndsSlash = apiUrl.back() == '/';
    bool pathStartsSlash = path.front() == '/';
    if (apiEndsSlash && pathStartsSlash) return apiUrl + path.substr(1);
    if (!apiEndsSlash && !pathStartsSlash) return apiUrl + "/" + path;
    return apiUrl + path;
}

NsValue nsString(const std::string& s) {
    NsValue v{};
    v.type = NS_STRING;
    v.str = strdup(s.c_str());
    return v;
}

NsValue nsNumber(double n) {
    NsValue v{};
    v.type = NS_NUMBER;
    v.number = n;
    return v;
}

NsValue errorEnvelope(const std::string& msg) {
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(false);
    (*m.map())["error"] = Value::fromString(msg);
    return nsString(json::encode(m));
}

// ------------------------------------------------------------------ HTTP/1.1 client

using Clock = std::chrono::steady_clock;

struct Url {
    bool tls = false;
    std::string host;
    int port = 80;
    std::string target = "/";
    std::string authority;  // host[:port] buat header Host
};

bool parseUrl(const std::string& in, Url& u, std::string& err) {
    std::string s = in;
    size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    size_t schemeLen;
    if (s.rfind("https://", 0) == 0) { u.tls = true; u.port = 443; schemeLen = 8; }
    else if (s.rfind("http://", 0) == 0) { u.tls = false; u.port = 80; schemeLen = 7; }
    else { err = "URL harus diawali http:// atau https://"; return false; }
    size_t pathStart = s.find_first_of("/?", schemeLen);
    std::string auth = s.substr(schemeLen, pathStart == std::string::npos ? std::string::npos : pathStart - schemeLen);
    u.target = pathStart == std::string::npos ? "/" : s.substr(pathStart);
    if (u.target[0] == '?') u.target = "/" + u.target;
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);  // userinfo diabaikan
    u.authority = auth;
    if (!auth.empty() && auth[0] == '[') {
        size_t close = auth.find(']');
        if (close == std::string::npos) { err = "URL: host IPv6 nggak valid"; return false; }
        u.host = auth.substr(1, close - 1);
        if (close + 1 < auth.size() && auth[close + 1] == ':') u.port = std::atoi(auth.c_str() + close + 2);
    } else {
        size_t colon = auth.rfind(':');
        if (colon != std::string::npos) { u.host = auth.substr(0, colon); u.port = std::atoi(auth.c_str() + colon + 1); }
        else u.host = auth;
    }
    if (u.host.empty() || u.port <= 0 || u.port > 65535) { err = "URL: host/port nggak valid"; return false; }
    return true;
}

struct ProxyCfg {
    bool use = false;
    std::string host;
    int port = 80;
    std::string auth;  // "user:pass" atau kosong
};

std::string envAny(const char* a, const char* b) {
    if (const char* v = std::getenv(a)) if (*v) return v;
    if (const char* v = std::getenv(b)) if (*v) return v;
    return "";
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += t[n & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned n = (unsigned char)in[i] << 16;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += '=';
    }
    return out;
}

bool noProxyMatches(const std::string& host) {
    std::string np = toLowerAscii(envAny("no_proxy", "NO_PROXY"));
    if (np.empty()) return false;
    std::string h = toLowerAscii(host);
    size_t pos = 0;
    while (pos <= np.size()) {
        size_t comma = np.find(',', pos);
        std::string item = np.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t b = item.find_first_not_of(" \t"), e = item.find_last_not_of(" \t");
        item = b == std::string::npos ? "" : item.substr(b, e - b + 1);
        if (item == "*") return true;
        if (!item.empty()) {
            if (item[0] == '.') item.erase(0, 1);
            if (h == item || (h.size() > item.size() && h.compare(h.size() - item.size(), item.size(), item) == 0 &&
                              h[h.size() - item.size() - 1] == '.')) return true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

ProxyCfg proxyFor(const Url& u) {
    ProxyCfg p;
    if (noProxyMatches(u.host)) return p;
    std::string v = u.tls ? envAny("https_proxy", "HTTPS_PROXY") : envAny("http_proxy", "HTTP_PROXY");
    if (v.empty()) return p;
    size_t s = v.find("://");
    if (s != std::string::npos) v = v.substr(s + 3);
    size_t slash = v.find('/');
    if (slash != std::string::npos) v.resize(slash);
    size_t at = v.rfind('@');
    if (at != std::string::npos) { p.auth = v.substr(0, at); v = v.substr(at + 1); }
    size_t colon = v.rfind(':');
    if (colon != std::string::npos) { p.host = v.substr(0, colon); p.port = std::atoi(v.c_str() + colon + 1); }
    else p.host = v;
    p.use = !p.host.empty();
    return p;
}

struct Deadline {
    Clock::time_point at;
    const std::atomic<bool>* abort = nullptr;
    int leftMs() const {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(at - Clock::now()).count();
        return d < 0 ? 0 : (d > 1000000 ? 1000000 : (int)d);
    }
    bool aborted() const { return abort && abort->load(); }
};

struct Sock {
    int fd = -1;
    Deadline dl;
    std::unique_ptr<tls::Connection> tls;
    ~Sock() { if (fd >= 0) ::close(fd); }

    // 0 siap, -1 timeout/abort/error
    int waitFor(short ev) {
        for (;;) {
            if (dl.aborted()) return -1;
            int left = dl.leftMs();
            if (left <= 0) return -1;
            struct pollfd p{fd, ev, 0};
            int r = ::poll(&p, 1, std::min(left, 100));
            if (r > 0) return 0;
            if (r < 0 && errno != EINTR) return -1;
        }
    }
    long rawRecv(uint8_t* buf, size_t len) {
        for (;;) {
            if (waitFor(POLLIN) != 0) return -1;
            ssize_t n = ::recv(fd, buf, len, 0);
            if (n >= 0) return n;
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
    }
    long rawSend(const uint8_t* buf, size_t len) {
        size_t off = 0;
        while (off < len) {
            if (waitFor(POLLOUT) != 0) return -1;
            ssize_t n = ::send(fd, buf + off, len - off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return -1;
            }
            off += (size_t)n;
        }
        return (long)len;
    }
    // Aplikasi: lewat TLS bila ada.
    long read(uint8_t* buf, size_t len) {
        if (tls) return (long)tls->read(buf, len);
        return rawRecv(buf, len);
    }
    bool write(const std::string& s) {
        if (tls) { tls->write(s); return true; }
        return rawSend(reinterpret_cast<const uint8_t*>(s.data()), s.size()) >= 0;
    }
};

bool tcpConnect(Sock& s, const std::string& host, int port, std::string& err) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) { err = "resolve " + host + " gagal: " + gai_strerror(rc); return false; }
    std::string last = "connect gagal";
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno != EINPROGRESS) { last = std::strerror(errno); ::close(fd); continue; }
        s.fd = fd;
        if (r != 0) {
            if (s.waitFor(POLLOUT) != 0) { last = "connect timeout"; ::close(fd); s.fd = -1; continue; }
            int soerr = 0; socklen_t sl = sizeof soerr;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) { last = std::strerror(soerr); ::close(fd); s.fd = -1; continue; }
        }
        freeaddrinfo(res);
        return true;
    }
    freeaddrinfo(res);
    err = "connect ke " + host + ":" + std::to_string(port) + " gagal: " + last;
    return false;
}

// Membaca sampai "\r\n\r\n"; sisa byte setelahnya ditinggal di `rest`.
bool readHead(Sock& s, std::string& head, std::string& rest, std::string& err) {
    std::string buf;
    uint8_t tmp[8192];
    for (;;) {
        size_t p = buf.find("\r\n\r\n");
        if (p != std::string::npos) {
            head = buf.substr(0, p);
            rest = buf.substr(p + 4);
            return true;
        }
        if (buf.size() > (1u << 20)) { err = "header respons terlalu besar"; return false; }
        long n = s.read(tmp, sizeof tmp);
        if (n <= 0) { err = n == 0 ? "koneksi ditutup sebelum header lengkap" : "timeout/kesalahan membaca respons"; return false; }
        buf.append(reinterpret_cast<char*>(tmp), (size_t)n);
    }
}

struct Request {
    std::string method, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    long timeoutMs = 0;
    bool follow = true;
    const std::atomic<bool>* abort = nullptr;
};

struct Sink {
    std::function<void(long status, ValueMap&)> onHead;
    std::function<bool(const char*, size_t)> onBody;  // false = batalkan
};

// Resolve Location relatif terhadap URL saat ini.
std::string resolveLocation(const Url& cur, const std::string& loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
    std::string base = std::string(cur.tls ? "https://" : "http://") + cur.authority;
    if (loc.rfind("//", 0) == 0) return std::string(cur.tls ? "https:" : "http:") + loc;
    if (!loc.empty() && loc[0] == '/') return base + loc;
    std::string dir = cur.target.substr(0, cur.target.find('?'));
    size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
    return base + dir + loc;
}

// Satu transfer (dengan redirect bila diminta). Mengembalikan "" bila sukses, selain itu pesan error.
std::string fetch(Request req, Sink& sink) {
    long total = req.timeoutMs > 0 ? req.timeoutMs : 30000;
    Deadline dl;
    dl.at = Clock::now() + std::chrono::milliseconds(total);
    dl.abort = req.abort;

    for (int hop = 0; hop <= 10; hop++) {
        Url u;
        std::string err;
        if (!parseUrl(req.url, u, err)) return err;
        ProxyCfg px = proxyFor(u);

        Sock s;
        s.dl = dl;
        Deadline connectDl = dl;
        auto tenSec = Clock::now() + std::chrono::seconds(10);
        if (tenSec < connectDl.at) connectDl.at = tenSec;
        s.dl = connectDl;
        if (px.use) { if (!tcpConnect(s, px.host, px.port, err)) return err; }
        else if (!tcpConnect(s, u.host, u.port, err)) return err;
        s.dl = dl;

        std::string target = u.target;
        if (px.use && u.tls) {
            std::string c = "CONNECT " + u.host + ":" + std::to_string(u.port) + " HTTP/1.1\r\nHost: " + u.host + ":" +
                            std::to_string(u.port) + "\r\n";
            if (!px.auth.empty()) c += "Proxy-Authorization: Basic " + base64(px.auth) + "\r\n";
            c += "\r\n";
            if (!s.write(c)) return "kirim CONNECT ke proxy gagal";
            std::string head, rest;
            if (!readHead(s, head, rest, err)) return "proxy: " + err;
            if (head.compare(0, 12, "HTTP/1.1 200") != 0 && head.compare(0, 12, "HTTP/1.0 200") != 0)
                return "proxy menolak CONNECT: " + head.substr(0, head.find("\r\n"));
        } else if (px.use) {
            target = std::string("http://") + u.authority + u.target;
        }

        if (u.tls) {
            tls::Transport tr;
            Sock* sp = &s;
            tr.recv = [sp](uint8_t* b, size_t n) -> long { return sp->rawRecv(b, n); };
            tr.send = [sp](const uint8_t* b, size_t n) -> long { return sp->rawSend(b, n); };
            tls::Options opt;
            opt.alpn = {"http/1.1"};
            try {
                s.tls = std::make_unique<tls::Connection>(tr, u.host, opt);
            } catch (const std::exception& e) {
                return std::string("TLS gagal: ") + e.what();
            }
        }

        std::string msg = req.method + " " + target + " HTTP/1.1\r\nHost: " + u.authority + "\r\n";
        bool hasUA = false, hasAccept = false;
        for (const auto& [k, v] : req.headers) {
            std::string lk = toLowerAscii(k);
            if (lk == "host" || lk == "content-length" || lk == "connection") continue;
            if (lk == "user-agent") hasUA = true;
            if (lk == "accept") hasAccept = true;
            msg += k + ": " + v + "\r\n";
        }
        if (!hasUA) msg += "User-Agent: nusa\r\n";
        if (!hasAccept) msg += "Accept: */*\r\n";
        bool sendBody = !req.body.empty();
        if (sendBody || req.method == "POST" || req.method == "PUT" || req.method == "PATCH")
            msg += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        msg += "Connection: close\r\n\r\n";
        msg += req.body;
        try {
            if (!s.write(msg)) return "kirim request gagal";
        } catch (const std::exception& e) {
            return std::string("kirim request gagal: ") + e.what();
        }

        try {
            std::string head, rest;
            if (!readHead(s, head, rest, err)) return err;
            size_t eol = head.find("\r\n");
            std::string statusLine = head.substr(0, eol);
            size_t sp1 = statusLine.find(' ');
            if (statusLine.compare(0, 5, "HTTP/") != 0 || sp1 == std::string::npos) return "status line nggak valid";
            long status = std::atol(statusLine.c_str() + sp1 + 1);
            ValueMap headers;
            size_t pos = eol == std::string::npos ? head.size() : eol + 2;
            while (pos < head.size()) {
                size_t e2 = head.find("\r\n", pos);
                std::string line = head.substr(pos, e2 == std::string::npos ? std::string::npos : e2 - pos);
                pos = e2 == std::string::npos ? head.size() : e2 + 2;
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                size_t vs = line.find_first_not_of(" \t", colon + 1);
                headers[toLowerAscii(line.substr(0, colon))] =
                    Value::fromString(vs == std::string::npos ? "" : line.substr(vs));
            }

            bool redirect = req.follow && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
                            headers.count("location");
            if (redirect) {
                req.url = resolveLocation(u, headers["location"].str());
                if (status == 303 || ((status == 301 || status == 302) && req.method != "HEAD")) {
                    if (req.method != "GET" && req.method != "HEAD") req.method = "GET";
                    req.body.clear();
                }
                continue;  // hop berikutnya (koneksi lama ditutup oleh ~Sock)
            }

            sink.onHead(status, headers);
            bool noBody = req.method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
            if (noBody) return "";

            auto te = headers.find("transfer-encoding");
            bool chunked = te != headers.end() && toLowerAscii(te->second.str()).find("chunked") != std::string::npos;
            long long remaining = -1;
            auto cl = headers.find("content-length");
            if (!chunked && cl != headers.end()) remaining = std::atoll(cl->second.str().c_str());

            std::string buf = rest;
            uint8_t tmp[16384];
            auto fill = [&]() -> bool {
                long n = s.read(tmp, sizeof tmp);
                if (n <= 0) return false;
                buf.append(reinterpret_cast<char*>(tmp), (size_t)n);
                return true;
            };
            if (chunked) {
                for (;;) {
                    size_t nl;
                    while ((nl = buf.find("\r\n")) == std::string::npos) {
                        if (!fill()) return "koneksi putus di tengah body chunked";
                    }
                    long long size = std::strtoll(buf.substr(0, nl).c_str(), nullptr, 16);
                    buf.erase(0, nl + 2);
                    if (size == 0) break;  // trailer diabaikan
                    while ((long long)buf.size() < size + 2) {
                        if (!fill()) return "koneksi putus di tengah chunk";
                    }
                    if (!sink.onBody(buf.data(), (size_t)size)) return "";
                    buf.erase(0, (size_t)size + 2);
                }
            } else if (remaining >= 0) {
                while (remaining > 0) {
                    if (buf.empty() && !fill()) return "koneksi putus sebelum body lengkap";
                    size_t n = (size_t)std::min<long long>(remaining, (long long)buf.size());
                    if (!sink.onBody(buf.data(), n)) return "";
                    buf.erase(0, n);
                    remaining -= (long long)n;
                }
            } else {
                for (;;) {
                    if (!buf.empty()) {
                        if (!sink.onBody(buf.data(), buf.size())) return "";
                        buf.clear();
                    }
                    if (!fill()) break;  // EOF = akhir body
                }
            }
            return "";
        } catch (const tls::WouldBlock&) {
            return "timeout";
        } catch (const std::exception& e) {
            return std::string("kesalahan jaringan: ") + e.what();
        }
    }
    return "terlalu banyak redirect";
}

// header_json ({"Nama": "nilai"}) -> daftar header.
bool parseHeaders(const std::string& headerJson, std::vector<std::pair<std::string, std::string>>& out, std::string& err) {
    Value headerIn;
    try {
        headerIn = json::decode(headerJson);
    } catch (const std::exception& e) {
        err = std::string("header_json invalid: ") + e.what();
        return false;
    }
    if (headerIn.type != ValueType::Map) {
        err = "header_json harus JSON object (mis. \"{}\" kalau nggak ada)";
        return false;
    }
    for (const auto& [key, val] : *headerIn.map()) {
        if (val.type != ValueType::String) { err = "header_json: value tiap key harus teks"; return false; }
        out.emplace_back(key, val.str());
    }
    return true;
}

NsValue httpMinta(int argc, const NsValue* argv) {
    if ((argc != 5 && argc != 6) || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_STRING || argv[3].type != NS_STRING || argv[4].type != NS_STRING ||
        (argc == 6 && argv[5].type != NS_NUMBER)) {
        return errorEnvelope("minta(metode, api_url, path, header_json, tubuh, [timeout_ms]): argumen nggak valid");
    }
    Request req;
    req.method = argv[0].str;
    req.url = joinUrl(argv[1].str, argv[2].str);
    req.body = argv[4].str;
    req.timeoutMs = argc == 6 ? static_cast<long>(argv[5].number) : 0;
    std::string herr;
    if (!parseHeaders(argv[3].str, req.headers, herr)) return errorEnvelope(herr);

    std::string responseBody;
    long statusCode = 0;
    ValueMap responseHeaders;
    Sink sink;
    sink.onHead = [&](long st, ValueMap& h) { statusCode = st; responseHeaders = h; };
    sink.onBody = [&](const char* p, size_t n) { responseBody.append(p, n); return true; };
    std::string err = fetch(req, sink);
    if (!err.empty()) return errorEnvelope("request gagal: " + err);

    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["status"] = Value::fromNumber(static_cast<double>(statusCode));
    (*out.map())["header"] = Value::fromMap(std::make_shared<ValueMap>(responseHeaders));
    (*out.map())["tubuh"] = Value::fromString(responseBody);
    return nsString(json::encode(out));
}

// ---- streaming: minta_stream / baca_stream / info_stream / tutup_stream ----

struct HttpStreamState {
    std::mutex mu;
    std::condition_variable cv;
    std::string buffer;       // body bytes received but not yet baca_stream()'d out
    bool headersReady = false;
    bool done = false;        // transfer finished (success or failure)
    long status = 0;
    std::string error;        // non-empty if the transfer failed
    ValueMap headers;
    std::atomic<bool> abort{false};
    std::thread worker;
};

std::mutex g_streamTableMu;
std::unordered_map<int, HttpStreamState*> g_streamTable;
int g_nextStreamHandle = 1;

void streamWorkerMain(HttpStreamState* st, std::string metode, std::string url, std::string headerJson,
                       std::string tubuh, long timeoutMs) {
    Request req;
    req.method = metode;
    req.url = url;
    req.body = tubuh;
    req.timeoutMs = timeoutMs;
    req.follow = false;  // caller yang butuh redirect cek status 3xx lalu minta_stream() lagi
    req.abort = &st->abort;
    std::string herr;
    if (!parseHeaders(headerJson, req.headers, herr)) {
        std::lock_guard<std::mutex> lock(st->mu);
        st->error = herr;
        st->done = true;
        st->headersReady = true;
        st->cv.notify_all();
        return;
    }
    Sink sink;
    sink.onHead = [&](long code, ValueMap& h) {
        std::lock_guard<std::mutex> lock(st->mu);
        st->status = code;
        st->headers = h;
        st->headersReady = true;
        st->cv.notify_all();
    };
    sink.onBody = [&](const char* p, size_t n) {
        if (st->abort.load()) return false;
        {
            std::lock_guard<std::mutex> lock(st->mu);
            st->buffer.append(p, n);
        }
        st->cv.notify_one();
        return true;
    };
    std::string err = fetch(req, sink);
    std::lock_guard<std::mutex> lock(st->mu);
    if (!err.empty() && !st->abort.load()) st->error = err;
    st->headersReady = true;
    st->done = true;
    st->cv.notify_all();
}

NsValue httpMintaStream(int argc, const NsValue* argv) {
    if ((argc != 5 && argc != 6) || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_STRING || argv[3].type != NS_STRING || argv[4].type != NS_STRING ||
        (argc == 6 && argv[5].type != NS_NUMBER)) {
        return errorEnvelope(
            "minta_stream(metode, api_url, path, header_json, tubuh, [timeout_ms]): argumen nggak valid");
    }
    std::string metode = argv[0].str;
    std::string url = joinUrl(argv[1].str, argv[2].str);
    std::string headerJson = argv[3].str;
    std::string tubuh = argv[4].str;
    long timeoutMs = argc == 6 ? static_cast<long>(argv[5].number) : 0;

    auto* st = new HttpStreamState();
    st->worker = std::thread(streamWorkerMain, st, metode, url, headerJson, tubuh, timeoutMs);

    int handle;
    {
        std::lock_guard<std::mutex> lock(g_streamTableMu);
        handle = g_nextStreamHandle++;
        g_streamTable[handle] = st;
    }

    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["handle"] = Value::fromNumber(handle);
    return nsString(json::encode(out));
}

HttpStreamState* lookupStream(double handleNum) {
    std::lock_guard<std::mutex> lock(g_streamTableMu);
    auto it = g_streamTable.find(static_cast<int>(handleNum));
    return it == g_streamTable.end() ? nullptr : it->second;
}

NsValue httpBacaStream(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_NUMBER) {
        NsValue v{};
        v.type = NS_STRING;
        v.str = strdup("");
        return v;
    }
    HttpStreamState* st = lookupStream(argv[0].number);
    if (!st) {
        NsValue v{};
        v.type = NS_STRING;
        v.str = strdup("");
        return v;
    }
    size_t maxLen = argv[1].number > 0 ? static_cast<size_t>(argv[1].number) : 65536;

    std::unique_lock<std::mutex> lock(st->mu);
    st->cv.wait(lock, [&] { return !st->buffer.empty() || st->done; });
    std::string chunk;
    if (!st->buffer.empty()) {
        size_t n = std::min(maxLen, st->buffer.size());
        chunk = st->buffer.substr(0, n);
        st->buffer.erase(0, n);
    }
    return nsString(chunk);
}

NsValue httpInfoStream(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_NUMBER) return errorEnvelope("info_stream(handle): argumen nggak valid");
    HttpStreamState* st = lookupStream(argv[0].number);
    if (!st) return errorEnvelope("handle stream nggak valid");

    std::unique_lock<std::mutex> lock(st->mu);
    st->cv.wait(lock, [&] { return st->headersReady || st->done; });
    if (!st->error.empty()) {
        std::string err = st->error;
        lock.unlock();
        return errorEnvelope(err);
    }
    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["status"] = Value::fromNumber(static_cast<double>(st->status));
    (*out.map())["header"] = Value::fromMap(std::make_shared<ValueMap>(st->headers));
    lock.unlock();
    return nsString(json::encode(out));
}

NsValue httpTutupStream(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_NUMBER) return nsNumber(0);
    HttpStreamState* st = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_streamTableMu);
        auto it = g_streamTable.find(static_cast<int>(argv[0].number));
        if (it != g_streamTable.end()) {
            st = it->second;
            g_streamTable.erase(it);
        }
    }
    if (!st) return nsNumber(0);
    // sinyal abort + kosongin buffer biar write callback worker gak nunggu selamanya
    st->abort.store(true);
    {
        std::unique_lock<std::mutex> lock(st->mu);
        st->buffer.clear();
    }
    st->worker.join();
    delete st;
    return nsNumber(1);
}

}  // namespace

extern "C" void ns_plugin_init_http(void* registry, NsRegisterFn reg) {
    reg(registry, "minta", httpMinta);
    reg(registry, "minta_stream", httpMintaStream);
    reg(registry, "baca_stream", httpBacaStream);
    reg(registry, "info_stream", httpInfoStream);
    reg(registry, "tutup_stream", httpTutupStream);
}
