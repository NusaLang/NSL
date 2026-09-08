// ws.cpp -- klien WebSocket (RFC 6455) di atas socket POSIX. TLS (wss://)
// lewat OpenSSL yang di-dlopen() malas; ws:// polos gak nyentuh pustaka
// apa pun selain libc/libstdc++. Verifikasi sertifikat selalu wajib buat
// wss://, gak ada opsi matiin. Frame biner keluar sebagai hex di JSON hasil.
#include "plugin_abi.h"
#include <string>
#include <map>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <thread>
#include <random>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// ---------------------------------------------------------------- NsValue

static NsValue nsStr(const std::string& s) {
    NsValue v{}; v.type = NS_STRING; v.boolean = 0; v.number = 0;
    v.str = (char*)malloc(s.size() + 1);
    memcpy(v.str, s.data(), s.size());
    v.str[s.size()] = 0;
    v.str_len = (int)s.size();
    return v;
}
static std::string toStr(const NsValue* v) { return std::string(v->str, (size_t)v->str_len); }

static std::string jescape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
static std::string toHex(const std::string& s) {
    static const char* H = "0123456789abcdef";
    std::string o; o.reserve(s.size() * 2);
    for (unsigned char c : s) { o.push_back(H[c >> 4]); o.push_back(H[c & 15]); }
    return o;
}

static std::map<std::string, std::string> parseHeaders(const std::string& js) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while ((i = js.find('"', i)) != std::string::npos) {
        size_t k0 = ++i;
        while (i < js.size() && js[i] != '"') { if (js[i] == '\\') i++; i++; }
        if (i >= js.size()) break;
        std::string key = js.substr(k0, i - k0);
        i++;
        while (i < js.size() && (js[i] == ' ' || js[i] == ':')) i++;
        if (i >= js.size() || js[i] != '"') break;
        size_t v0 = ++i;
        while (i < js.size() && js[i] != '"') { if (js[i] == '\\') i++; i++; }
        out[key] = js.substr(v0, i - v0);
        i++;
    }
    return out;
}

// ------------------------------------------------- SHA1 + base64 (mandiri)
// Dipakai cuma buat Sec-WebSocket-Key/Accept. Ditulis sendiri supaya jalur
// ws:// (tanpa TLS) tidak perlu menarik libcrypto sama sekali.

struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t total = 0;
    unsigned char buf[64];
    size_t buflen = 0;

    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    void block(const unsigned char* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const unsigned char* p, size_t n) {
        total += n;
        while (n) {
            size_t take = 64 - buflen; if (take > n) take = n;
            memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    void final(unsigned char out[20]) {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (buflen != 56) update(&z, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++) len[i] = (unsigned char)((bits >> (56 - i * 8)) & 0xFF);
        // update() menaikkan total, tapi total sudah tidak dipakai lagi
        total += 8;
        for (int i = 0; i < 8; i++) { buf[buflen++] = len[i]; }
        block(buf); buflen = 0;
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (unsigned char)((h[i] >> 24) & 0xFF);
            out[i*4+1] = (unsigned char)((h[i] >> 16) & 0xFF);
            out[i*4+2] = (unsigned char)((h[i] >> 8) & 0xFF);
            out[i*4+3] = (unsigned char)(h[i] & 0xFF);
        }
    }
};

static const char* B64ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64encode(const unsigned char* p, size_t n) {
    std::string o;
    o.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i+1] << 8;
        if (i + 2 < n) v |= (uint32_t)p[i+2];
        o.push_back(B64ALPHA[(v >> 18) & 63]);
        o.push_back(B64ALPHA[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? B64ALPHA[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? B64ALPHA[v & 63] : '=');
    }
    return o;
}

static std::string wsAcceptOf(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sh;
    sh.update((const unsigned char*)key.data(), key.size());
    sh.update((const unsigned char*)GUID, strlen(GUID));
    unsigned char dg[20];
    sh.final(dg);
    return b64encode(dg, 20);
}

// --------------------------------------------------- OpenSSL lewat dlopen()
// Hanya prototipe yang benar-benar dipakai. Semua tipe OpenSSL 3.x bersifat
// opaque, jadi cukup void*. Konstanta di bawah stabil sejak OpenSSL 1.1.0.

#define SSL_CTRL_MODE                      33
#define SSL_CTRL_SET_TLSEXT_HOSTNAME       55
#define SSL_CTRL_SET_MIN_PROTO_VERSION    123
#define TLSEXT_NAMETYPE_host_name           0
#define TLS1_2_VERSION                 0x0303
#define SSL_VERIFY_PEER                  0x01
#define SSL_MODE_ENABLE_PARTIAL_WRITE     0x1
#define SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 0x2
#define SSL_ERROR_NONE                      0
#define SSL_ERROR_SSL                       1
#define SSL_ERROR_WANT_READ                 2
#define SSL_ERROR_WANT_WRITE                3
#define SSL_ERROR_SYSCALL                   5
#define SSL_ERROR_ZERO_RETURN               6
#define X509_V_OK                           0

struct TlsApi {
    void* h_ssl = nullptr;
    void* h_crypto = nullptr;
    bool ok = false;
    std::string err;

    const void* (*TLS_client_method)(void) = nullptr;
    void* (*SSL_CTX_new)(const void*) = nullptr;
    void  (*SSL_CTX_free)(void*) = nullptr;
    long  (*SSL_CTX_ctrl)(void*, int, long, void*) = nullptr;
    void  (*SSL_CTX_set_verify)(void*, int, void*) = nullptr;
    int   (*SSL_CTX_set_default_verify_paths)(void*) = nullptr;
    int   (*SSL_CTX_load_verify_locations)(void*, const char*, const char*) = nullptr;
    void* (*SSL_new)(void*) = nullptr;
    void  (*SSL_free)(void*) = nullptr;
    int   (*SSL_set_fd)(void*, int) = nullptr;
    long  (*SSL_ctrl)(void*, int, long, void*) = nullptr;
    int   (*SSL_set1_host)(void*, const char*) = nullptr;
    int   (*SSL_connect)(void*) = nullptr;
    int   (*SSL_read)(void*, void*, int) = nullptr;
    int   (*SSL_write)(void*, const void*, int) = nullptr;
    int   (*SSL_get_error)(const void*, int) = nullptr;
    int   (*SSL_shutdown)(void*) = nullptr;
    long  (*SSL_get_verify_result)(const void*) = nullptr;
    unsigned long (*ERR_get_error)(void) = nullptr;
    void  (*ERR_error_string_n)(unsigned long, char*, size_t) = nullptr;
};

static TlsApi g_tls;
static std::once_flag g_tls_once;

static void* dlsymOrFail(void* h, const char* name, TlsApi& a) {
    void* p = dlsym(h, name);
    if (!p && a.err.empty()) a.err = std::string("simbol OpenSSL hilang: ") + name;
    return p;
}

static void tlsInit() {
    TlsApi& a = g_tls;
    // Kalau proses sudah memuat libssl (mis. plugin http lewat libcurl),
    // dlopen di bawah cuma menaikkan refcount -- tidak ada biaya tambahan.
    const char* cands[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
    for (const char* c : cands) {
        a.h_ssl = dlopen(c, RTLD_LAZY | RTLD_LOCAL);
        if (a.h_ssl) break;
    }
    if (!a.h_ssl) { a.err = "libssl tidak ditemukan (wss:// butuh OpenSSL terpasang)"; return; }
    const char* ccands[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"};
    for (const char* c : ccands) {
        a.h_crypto = dlopen(c, RTLD_LAZY | RTLD_LOCAL);
        if (a.h_crypto) break;
    }

    #define LD(fld, name) a.fld = (decltype(a.fld))dlsymOrFail(a.h_ssl, name, a)
    LD(TLS_client_method, "TLS_client_method");
    LD(SSL_CTX_new, "SSL_CTX_new");
    LD(SSL_CTX_free, "SSL_CTX_free");
    LD(SSL_CTX_ctrl, "SSL_CTX_ctrl");
    LD(SSL_CTX_set_verify, "SSL_CTX_set_verify");
    LD(SSL_CTX_set_default_verify_paths, "SSL_CTX_set_default_verify_paths");
    LD(SSL_CTX_load_verify_locations, "SSL_CTX_load_verify_locations");
    LD(SSL_new, "SSL_new");
    LD(SSL_free, "SSL_free");
    LD(SSL_set_fd, "SSL_set_fd");
    LD(SSL_ctrl, "SSL_ctrl");
    LD(SSL_set1_host, "SSL_set1_host");
    LD(SSL_connect, "SSL_connect");
    LD(SSL_read, "SSL_read");
    LD(SSL_write, "SSL_write");
    LD(SSL_get_error, "SSL_get_error");
    LD(SSL_shutdown, "SSL_shutdown");
    LD(SSL_get_verify_result, "SSL_get_verify_result");
    #undef LD
    if (a.h_crypto) {
        a.ERR_get_error = (unsigned long (*)(void))dlsym(a.h_crypto, "ERR_get_error");
        a.ERR_error_string_n = (void (*)(unsigned long, char*, size_t))dlsym(a.h_crypto, "ERR_error_string_n");
    }

    if (!a.err.empty()) return;
    a.ok = a.TLS_client_method && a.SSL_CTX_new && a.SSL_new && a.SSL_connect &&
           a.SSL_read && a.SSL_write && a.SSL_get_error && a.SSL_set1_host &&
           a.SSL_get_verify_result;
    if (!a.ok && a.err.empty()) a.err = "OpenSSL tidak lengkap";
}

static bool tlsReady(std::string& err) {
    std::call_once(g_tls_once, tlsInit);
    if (!g_tls.ok) { err = g_tls.err.empty() ? "OpenSSL tidak tersedia" : g_tls.err; return false; }
    return true;
}

static std::string tlsLastError() {
    if (!g_tls.ERR_get_error || !g_tls.ERR_error_string_n) return "";
    unsigned long e = g_tls.ERR_get_error();
    if (!e) return "";
    char b[256];
    g_tls.ERR_error_string_n(e, b, sizeof b);
    return std::string(b);
}

// ----------------------------------------------------------------- transport

enum IoRes { IO_OK, IO_AGAIN, IO_EOF, IO_ERR };

struct WSSession {
    int fd = -1;
    void* ssl = nullptr;       // SSL*
    void* ssl_ctx = nullptr;   // SSL_CTX*
    bool upgraded = false;
    std::string rbuf;
    std::mt19937 rng{std::random_device{}()};
    std::mutex wmu;            // serialisasi penulisan frame (satu frame utuh)
};

static void connClose(WSSession* s) {
    if (s->ssl) {
        if (g_tls.SSL_shutdown) g_tls.SSL_shutdown(s->ssl);
        if (g_tls.SSL_free) g_tls.SSL_free(s->ssl);
        s->ssl = nullptr;
    }
    if (s->ssl_ctx) {
        if (g_tls.SSL_CTX_free) g_tls.SSL_CTX_free(s->ssl_ctx);
        s->ssl_ctx = nullptr;
    }
    if (s->fd >= 0) { ::close(s->fd); s->fd = -1; }
}

// Semantik sengaja dibikin sama persis dengan curl_easy_send/recv di mode
// CONNECT_ONLY: non-blocking, IO_AGAIN kalau belum ada data, IO_EOF kalau
// peer benar-benar menutup, IO_ERR untuk error socket.
static IoRes connRecv(WSSession* s, char* buf, size_t cap, size_t* got) {
    *got = 0;
    if (s->ssl) {
        int n = g_tls.SSL_read(s->ssl, buf, (int)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap));
        if (n > 0) { *got = (size_t)n; return IO_OK; }
        int e = g_tls.SSL_get_error(s->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return IO_AGAIN;
        if (e == SSL_ERROR_ZERO_RETURN) return IO_EOF;
        if (e == SSL_ERROR_SYSCALL && n == 0) return IO_EOF;
        if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) return IO_AGAIN;
        return IO_ERR;
    }
    ssize_t n = ::recv(s->fd, buf, cap, 0);
    if (n > 0) { *got = (size_t)n; return IO_OK; }
    if (n == 0) return IO_EOF;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IO_AGAIN;
    return IO_ERR;
}

static IoRes connSendOnce(WSSession* s, const char* buf, size_t len, size_t* sent) {
    *sent = 0;
    if (s->ssl) {
        int n = g_tls.SSL_write(s->ssl, buf, (int)(len > 0x7FFFFFFF ? 0x7FFFFFFF : len));
        if (n > 0) { *sent = (size_t)n; return IO_OK; }
        int e = g_tls.SSL_get_error(s->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return IO_AGAIN;
        if (e == SSL_ERROR_ZERO_RETURN) return IO_EOF;
        if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) return IO_AGAIN;
        return IO_ERR;
    }
    ssize_t n = ::send(s->fd, buf, len, MSG_NOSIGNAL);
    if (n > 0) { *sent = (size_t)n; return IO_OK; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return IO_AGAIN;
    return IO_ERR;
}

// Kirim seluruh buffer. Menunggu socket writable lewat poll() alih-alih
// sleep-loop 2 ms seperti versi libcurl -- lebih responsif dan tidak boros CPU.
static bool connSendAll(WSSession* s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        size_t sent = 0;
        IoRes r = connSendOnce(s, data.data() + off, data.size() - off, &sent);
        if (r == IO_OK) { off += sent; continue; }
        if (r == IO_AGAIN) {
            struct pollfd p{s->fd, POLLOUT, 0};
            if (::poll(&p, 1, 100) < 0 && errno != EINTR) return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool setNonBlocking(int fd, bool nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    fl = nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl) == 0;
}

// TCP connect non-blocking dengan deadline; mencoba semua hasil getaddrinfo
// (IPv6 lalu IPv4, sesuai urutan resolver).
static int tcpConnect(const std::string& host, const std::string& port,
                      int timeout_ms, std::string& err) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        err = std::string("resolve gagal: ") + gai_strerror(rc);
        return -1;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int fd = -1;
    err = "tidak ada alamat yang bisa dihubungi";
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { err = std::string("socket: ") + strerror(errno); continue; }
        setNonBlocking(fd, true);
        int cr = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) break;
        if (errno != EINPROGRESS) {
            err = std::string("connect: ") + strerror(errno);
            ::close(fd); fd = -1; continue;
        }
        bool connected = false;
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { err = "connect: timeout"; break; }
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            struct pollfd p{fd, POLLOUT, 0};
            int pr = ::poll(&p, 1, left);
            if (pr < 0) { if (errno == EINTR) continue; err = std::string("poll: ") + strerror(errno); break; }
            if (pr == 0) { err = "connect: timeout"; break; }
            int soerr = 0; socklen_t sl = sizeof soerr;
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
                err = std::string("getsockopt: ") + strerror(errno); break;
            }
            if (soerr == 0) { connected = true; break; }
            err = std::string("connect: ") + strerror(soerr);
            break;
        }
        if (connected) break;
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    }
    return fd;
}

// TLS handshake di atas fd non-blocking, dengan deadline sendiri.
// Verifikasi rantai CA + hostname WAJIB; tidak ada jalan mematikannya.
static bool tlsHandshake(WSSession* s, const std::string& host, int timeout_ms, std::string& err) {
    if (!tlsReady(err)) return false;
    TlsApi& a = g_tls;
    s->ssl_ctx = a.SSL_CTX_new(a.TLS_client_method());
    if (!s->ssl_ctx) { err = "SSL_CTX_new gagal"; return false; }
    a.SSL_CTX_ctrl(s->ssl_ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, nullptr);
    a.SSL_CTX_ctrl(s->ssl_ctx, SSL_CTRL_MODE,
                   SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, nullptr);
    a.SSL_CTX_set_verify(s->ssl_ctx, SSL_VERIFY_PEER, nullptr);

    // Muat trust store. SSL_CTX_set_default_verify_paths() sendiri sudah
    // menghormati env SSL_CERT_FILE / SSL_CERT_DIR (dipakai tes lokal dengan
    // CA sendiri); kalau OpenSSL dikompilasi dengan prefix lain, coba lokasi
    // distro yang umum sebagai cadangan.
    int haveCA = a.SSL_CTX_set_default_verify_paths(s->ssl_ctx);
    if (haveCA != 1) {
        static const char* files[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/cert.pem", nullptr };
        for (int i = 0; files[i]; i++)
            if (a.SSL_CTX_load_verify_locations(s->ssl_ctx, files[i], nullptr) == 1) { haveCA = 1; break; }
        if (haveCA != 1 && a.SSL_CTX_load_verify_locations(s->ssl_ctx, nullptr, "/etc/ssl/certs") == 1)
            haveCA = 1;
    }
    if (haveCA != 1) { err = "tidak menemukan trust store CA sistem"; return false; }

    s->ssl = a.SSL_new(s->ssl_ctx);
    if (!s->ssl) { err = "SSL_new gagal"; return false; }
    a.SSL_set_fd(s->ssl, s->fd);
    // SNI
    a.SSL_ctrl(s->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
               (void*)host.c_str());
    // Pencocokan hostname terhadap SAN/CN saat verifikasi rantai.
    if (a.SSL_set1_host(s->ssl, host.c_str()) != 1) { err = "SSL_set1_host gagal"; return false; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int r = a.SSL_connect(s->ssl);
        if (r == 1) break;
        int e = a.SSL_get_error(s->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { err = "tls: timeout handshake"; return false; }
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            struct pollfd p{s->fd, (short)(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
            int pr = ::poll(&p, 1, left);
            if (pr == 0) { err = "tls: timeout handshake"; return false; }
            if (pr < 0 && errno != EINTR) { err = std::string("tls: poll: ") + strerror(errno); return false; }
            continue;
        }
        std::string detail = tlsLastError();
        long vr = a.SSL_get_verify_result(s->ssl);
        if (vr != X509_V_OK)
            err = "tls: verifikasi sertifikat gagal (kode " + std::to_string(vr) + ")";
        else
            err = "tls: handshake gagal" + (detail.empty() ? std::string() : (": " + detail));
        return false;
    }
    long vr = a.SSL_get_verify_result(s->ssl);
    if (vr != X509_V_OK) {
        err = "tls: verifikasi sertifikat gagal (kode " + std::to_string(vr) + ")";
        return false;
    }
    return true;
}

// -------------------------------------------------------------- sesi & frame

static std::mutex g_mu;
static std::unordered_map<long, WSSession*> g_sess;
static long g_next_id = 1;

static std::string maskFrame(WSSession* s, const std::string& payload, int opcode) {
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(s->rng() & 0xFF);
    std::string f;
    f.reserve(payload.size() + 14);
    f.push_back((char)(0x80 | opcode));
    size_t len = payload.size();
    if (len < 126) f.push_back((char)(0x80 | len));
    else if (len < 65536) {
        f.push_back((char)(0x80 | 126));
        f.push_back((char)((len >> 8) & 0xFF)); f.push_back((char)(len & 0xFF));
    } else {
        f.push_back((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) f.push_back((char)(((uint64_t)len >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; i++) f.push_back((char)mask[i]);
    for (size_t i = 0; i < len; i++) f.push_back(payload[i] ^ mask[i % 4]);
    return f;
}

static bool sendFrame(WSSession* s, const std::string& payload, int opcode) {
    std::string f = maskFrame(s, payload, opcode);
    std::lock_guard<std::mutex> lk(s->wmu);
    return connSendAll(s, f);
}

struct Frame { bool fin; int opcode; std::string payload; };
static bool tryParseOne(WSSession* s, Frame& out, size_t& consumed) {
    const std::string& b = s->rbuf;
    if (b.size() < 2) return false;
    int opcode = b[0] & 0x0F;
    bool fin = (b[0] & 0x80) != 0;
    bool masked = (b[1] & 0x80) != 0;
    uint64_t len = b[1] & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (b.size() < pos + 2) return false;
        len = ((unsigned char)b[2] << 8) | (unsigned char)b[3];
        pos += 2;
    } else if (len == 127) {
        if (b.size() < pos + 8) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | (unsigned char)b[pos + i];
        pos += 8;
    }
    unsigned char mk[4] = {0,0,0,0};
    if (masked) {
        // Server RFC-compliant tidak me-mask, tapi tangani saja kalau ada.
        if (b.size() < pos + 4) return false;
        for (int i = 0; i < 4; i++) mk[i] = (unsigned char)b[pos + i];
        pos += 4;
    }
    if (b.size() < pos + len) return false;
    out.fin = fin; out.opcode = opcode;
    out.payload = b.substr(pos, len);
    if (masked)
        for (size_t i = 0; i < out.payload.size(); i++)
            out.payload[i] = (char)(out.payload[i] ^ mk[i % 4]);
    consumed = pos + len;
    return true;
}

static void destroySession(long id) {
    WSSession* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_sess.find(id);
        if (it == g_sess.end()) return;
        s = it->second;
        g_sess.erase(it);
    }
    connClose(s);
    delete s;
}

// ------------------------------------------------------------------- ws_buka

extern "C" NsValue wsBuka(int argc, const NsValue* argv);
extern "C" NsValue wsBuka(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING)
        return nsStr("{\"ok\":false,\"error\":\"argumen salah\"}");
    std::string url = toStr(&argv[0]);

    bool secure;
    size_t schemeLen;
    if (url.rfind("wss://", 0) == 0)      { secure = true;  schemeLen = 6; }
    else if (url.rfind("ws://", 0) == 0)  { secure = false; schemeLen = 5; }
    else if (url.rfind("https://", 0) == 0) { secure = true;  schemeLen = 8; }
    else if (url.rfind("http://", 0) == 0)  { secure = false; schemeLen = 7; }
    else return nsStr("{\"ok\":false,\"error\":\"skema url tidak dikenal\"}");

    // host = authority apa adanya (termasuk :port), dipakai untuk header Host
    // dan Origin -- sama persis dengan perilaku versi libcurl.
    std::string host, path = "/";
    {
        size_t sp = url.find('/', schemeLen);
        if (sp == std::string::npos) host = url.substr(schemeLen);
        else { host = url.substr(schemeLen, sp - schemeLen); path = url.substr(sp); }
    }
    if (host.empty()) return nsStr("{\"ok\":false,\"error\":\"host kosong\"}");

    // Pisahkan hostname dan port untuk resolve (dukung [::1]:port).
    std::string hostname = host, port = secure ? "443" : "80";
    if (!host.empty() && host[0] == '[') {
        size_t rb = host.find(']');
        if (rb == std::string::npos) return nsStr("{\"ok\":false,\"error\":\"host ipv6 tidak valid\"}");
        hostname = host.substr(1, rb - 1);
        if (rb + 1 < host.size() && host[rb + 1] == ':') port = host.substr(rb + 2);
    } else {
        size_t cl = host.rfind(':');
        if (cl != std::string::npos && host.find(':') == cl) {
            hostname = host.substr(0, cl);
            port = host.substr(cl + 1);
        }
    }
    if (hostname.empty()) return nsStr("{\"ok\":false,\"error\":\"host kosong\"}");

    auto* s = new WSSession();
    std::string cerr;
    s->fd = tcpConnect(hostname, port, 15000, cerr);
    if (s->fd < 0) {
        delete s;
        return nsStr("{\"ok\":false,\"error\":\"connect: " + jescape(cerr) + "\"}");
    }
    if (secure && !tlsHandshake(s, hostname, 15000, cerr)) {
        connClose(s); delete s;
        return nsStr("{\"ok\":false,\"error\":\"connect: " + jescape(cerr) + "\"}");
    }
    setNonBlocking(s->fd, true);

    long idSnap;
    { std::lock_guard<std::mutex> lk(g_mu); idSnap = g_next_id++; g_sess[idSnap] = s; }

    unsigned char key_raw[16];
    for (int i = 0; i < 16; i++) key_raw[i] = (unsigned char)(s->rng() & 0xFF);
    std::string wskey = b64encode(key_raw, 16);
    std::string expect_accept = wsAcceptOf(wskey);

    auto hdrs = parseHeaders(toStr(&argv[1]));
    if (hdrs.find("Origin") == hdrs.end() && hdrs.find("origin") == hdrs.end()) {
        std::string proto = secure ? "https://" : "http://";
        hdrs["Origin"] = proto + host;
    }
    if (hdrs.find("User-Agent") == hdrs.end() && hdrs.find("user-agent") == hdrs.end()) {
        hdrs["User-Agent"] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                      wskey + "\r\nSec-WebSocket-Version: 13\r\n";
    for (auto& kv : hdrs) req += kv.first + ": " + kv.second + "\r\n";
    req += "\r\n";
    if (!connSendAll(s, req)) {
        destroySession(idSnap);
        return nsStr("{\"ok\":false,\"error\":\"gagal kirim upgrade\"}");
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool sawHead = false;
    std::string head;
    while (std::chrono::steady_clock::now() < deadline) {
        size_t hend = s->rbuf.find("\r\n\r\n");
        if (hend != std::string::npos) {
            head = s->rbuf.substr(0, hend);
            s->rbuf.erase(0, hend + 4);
            sawHead = true;
            break;
        }
        if (s->rbuf.size() > 65536) break;  // header ngawur, jangan tumbuh terus
        size_t got = 0; char tmp[4096];
        IoRes r2 = connRecv(s, tmp, sizeof tmp, &got);
        if (r2 == IO_OK && got > 0) { s->rbuf.append(tmp, got); continue; }
        if (r2 == IO_EOF || r2 == IO_ERR) break;
        struct pollfd p{s->fd, POLLIN, 0};
        ::poll(&p, 1, 50);
    }

    std::string failReason;
    if (!sawHead) {
        failReason = "upgrade gagal";
    } else {
        // Status line harus 101.
        size_t eol = head.find("\r\n");
        std::string status = head.substr(0, eol == std::string::npos ? head.size() : eol);
        if (status.find(" 101") == std::string::npos) failReason = "upgrade gagal";
        else {
            // Validasi Sec-WebSocket-Accept (RFC 6455 §4.1) -- versi lama cuma
            // mencari "101" di mana saja pada blok header, yang bisa lolos
            // pada respons yang bukan upgrade sama sekali.
            std::string accept;
            size_t p = 0;
            while (p < head.size()) {
                size_t e = head.find("\r\n", p);
                if (e == std::string::npos) e = head.size();
                std::string line = head.substr(p, e - p);
                size_t c = line.find(':');
                if (c != std::string::npos) {
                    std::string k = line.substr(0, c);
                    for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                    if (k == "sec-websocket-accept") {
                        std::string v = line.substr(c + 1);
                        size_t b = v.find_first_not_of(" \t");
                        size_t en = v.find_last_not_of(" \t");
                        accept = (b == std::string::npos) ? "" : v.substr(b, en - b + 1);
                    }
                }
                p = e + 2;
            }
            if (accept.empty()) failReason = "sec-websocket-accept tidak ada";
            else if (accept != expect_accept) failReason = "sec-websocket-accept tidak cocok";
        }
    }
    if (!failReason.empty()) {
        std::string shown = sawHead ? head : s->rbuf;
        if (shown.size() > 220) shown.resize(220);
        destroySession(idSnap);
        return nsStr("{\"ok\":false,\"error\":\"" + jescape(failReason) +
                     "\",\"head\":\"" + jescape(shown) + "\"}");
    }
    s->upgraded = true;
    return nsStr("{\"ok\":true,\"id\":" + std::to_string(idSnap) + "}");
}

// ------------------------------------------------------------------ ws_kirim

extern "C" NsValue wsKirim(int argc, const NsValue* argv);
extern "C" NsValue wsKirim(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_NUMBER || argv[1].type != NS_STRING || argv[2].type != NS_NUMBER)
        return nsStr("{\"ok\":false,\"error\":\"argumen salah\"}");
    long id = (long)argv[0].number;
    WSSession* s = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_sess.find(id); if (it != g_sess.end()) s = it->second; }
    if (!s || !s->upgraded) return nsStr("{\"ok\":false,\"error\":\"sesi tidak ada\"}");
    if (!sendFrame(s, toStr(&argv[1]), (int)argv[2].number))
        return nsStr("{\"ok\":false,\"error\":\"kirim gagal\"}");
    return nsStr("{\"ok\":true}");
}

// ----------------------------------------------------------------- ws_terima

extern "C" NsValue wsTerima(int argc, const NsValue* argv);
extern "C" NsValue wsTerima(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_NUMBER)
        return nsStr("{\"ok\":false,\"error\":\"argumen salah\"}");
    long id = (long)argv[0].number;
    long tmo = (long)argv[1].number;
    WSSession* s = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_sess.find(id); if (it != g_sess.end()) s = it->second; }
    if (!s || !s->upgraded) return nsStr("{\"ok\":false,\"closed\":true}");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(tmo);
    std::string message;
    int msg_opcode = -1;
    for (;;) {
        Frame f; size_t used = 0;
        if (tryParseOne(s, f, used)) {
            s->rbuf.erase(0, used);
            if (f.opcode == 9) { sendFrame(s, f.payload, 10); continue; }   // PING->PONG
            if (f.opcode == 8) {                                            // CLOSE
                sendFrame(s, "", 8);
                destroySession(id);
                return nsStr("{\"ok\":true,\"closed\":true}");
            }
            if (f.opcode == 0 || f.opcode == 1 || f.opcode == 2) {
                message += f.payload;
                if (msg_opcode == -1 && f.opcode != 0) msg_opcode = f.opcode;
                if (f.fin) {
                    if (msg_opcode == 2)
                        return nsStr("{\"ok\":true,\"opcode\":2,\"hex\":\"" + toHex(message) + "\"}");
                    return nsStr("{\"ok\":true,\"opcode\":1,\"data\":\"" + jescape(message) + "\"}");
                }
                continue;
            }
            continue;   // PONG dsb: abaikan
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        size_t got = 0; char tmp[16384];
        IoRes r2 = connRecv(s, tmp, sizeof tmp, &got);
        if (r2 == IO_OK && got > 0) { s->rbuf.append(tmp, got); continue; }
        if (r2 == IO_EOF || r2 == IO_ERR) {
            // EOF asli atau error socket (connection reset dsb) -- sama-sama
            // harus dilaporkan closed, bukan timeout biasa, kalau tidak caller
            // menunggu timeout penuh berkali-kali di socket yang sudah mati.
            destroySession(id);
            return nsStr("{\"ok\":true,\"closed\":true}");
        }
        // IO_AGAIN: tidur di poll() sampai ada data atau deadline habis.
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (left <= 0) break;
        if (left > 100) left = 100;
        struct pollfd p{s->fd, POLLIN, 0};
        ::poll(&p, 1, left);
    }
    return nsStr("{\"ok\":false,\"timeout\":true}");
}

// ------------------------------------------------------------------ ws_tutup

extern "C" NsValue wsTutup(int argc, const NsValue* argv);
extern "C" NsValue wsTutup(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_NUMBER) return nsStr("{\"ok\":false}");
    long id = (long)argv[0].number;
    WSSession* s = nullptr;
    { std::lock_guard<std::mutex> lk(g_mu); auto it = g_sess.find(id); if (it != g_sess.end()) s = it->second; }
    // Kirim CLOSE dulu supaya server tahu ini penutupan bersih, baru bongkar.
    if (s && s->upgraded) sendFrame(s, "", 8);
    destroySession(id);
    return nsStr("{\"ok\":true}");
}

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg);
extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "ws_buka",   wsBuka);
    reg(registry, "ws_kirim",  wsKirim);
    reg(registry, "ws_terima", wsTerima);
    reg(registry, "ws_tutup",  wsTutup);
}
