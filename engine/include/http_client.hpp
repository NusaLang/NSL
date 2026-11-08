#pragma once

#include <string>

// The HTTP/1.1 + TLS client built into the binary (src/mod_http.cpp).
namespace httpclient {

// GET `url` (following redirects) into the file `dest`. False with `err` set on any failure,
// including a non-200 status.
bool downloadFile(const std::string& url, const std::string& dest, std::string& err);

}  // namespace httpclient
