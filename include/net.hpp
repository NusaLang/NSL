#pragma once

#include <functional>
#include <string>
#include <unordered_map>

// POSIX-socket networking primitives. No Windows support yet (would need
// a winsock shim).
namespace net {

// Returns a connected socket fd, or -1 on failure.
int tcpConnect(const std::string& host, int port);
// Sends all of `data`, retrying short writes. Returns bytes sent, or -1
// on error.
long tcpSend(int fd, const std::string& data);
// One recv() call, up to maxLen bytes. Returns "" on EOF or error --
// callers that want "read everything" should loop until they get "".
std::string tcpRecv(int fd, int maxLen);
void tcpClose(int fd);

// Safety net for forgotten tcp_tutup() calls -- force-closes every fd
// tcpConnect() opened and tcpClose() hasn't untracked. Called once after
// each script run (main.cpp).
void closeAllTracked();

struct HttpResponse {
    int status = 0;
    std::string body;
};

// Minimal HTTP/1.1 client over plain TCP or TLS (vendored BearSSL for
// https://). body/contentType are ignored for GET.
HttpResponse httpRequest(const std::string& method, const std::string& url,
                          const std::string& body, const std::string& contentType,
                          const std::unordered_map<std::string, std::string>& headers = {});

// Raw SMTP (EHLO/MAIL FROM/RCPT TO/DATA) -- no auth, no STARTTLS.
bool smtpSend(const std::string& host, int port, const std::string& from,
              const std::string& to, const std::string& subject, const std::string& body);

struct HttpRequestIn {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;  // lowercased keys
    std::string body;
    std::string ip;  // client address, dotted-decimal/IPv6 text form
    int fd = -1;      // the raw connection socket -- see HttpResponseOut::streamed
};

struct HttpResponseOut {
    int status = 200;
    std::string contentType = "text/plain";
    std::string body;
    // True: handler already wrote its response to HttpRequestIn::fd and
    // closed it itself (e.g. tcp_kirim()/tcp_tutup()) -- httpServe() skips both.
    bool streamed = false;
};

// Minimal HTTP/1.1 server: bind+listen on `port`, each connection handled
// on its own thread. No keep-alive. Throws if bind()/listen() fails.
void httpServe(int port, const std::function<HttpResponseOut(const HttpRequestIn&)>& onRequest);

}  // namespace net
