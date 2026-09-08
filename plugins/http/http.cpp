// Plugin HTTP client dengan TLS beneran, via libcurl -- link ke
// libcurl cuma buat .so ini sendiri, binary nusa inti tetep gak
// nyentuh OpenSSL. `minta()` buffer seluruh body di memori;
// `minta_stream()`/`baca_stream()`/`tutup_stream()` jalanin
// curl_easy_perform() di thread terpisah, ngalir lewat queue
// mutex+condvar, buat body gede yang gak mau ditunggu sekaligus.

#include "plugin_abi.h"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "json.hpp"
#include "value.hpp"

namespace {

std::once_flag g_curlInitFlag;

void ensureCurlInit() {
    std::call_once(g_curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    // sengaja gak pernah curl_global_cleanup() -- .so ini hidup sepanjang proses
}

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

size_t writeBodyCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Header keys selalu lowercase (HTTP header case-insensitive by spec,
// sama konvensi http_dengar()). Map di-clear tiap "HTTP/..." status
// line baru muncul -- redirect ngirim header per-hop, tanpa ini header
// dari respons redirect nyampur sama respons final.
size_t writeHeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::unordered_map<std::string, Value>*>(userdata);
    size_t len = size * nitems;
    std::string line(buffer, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();
        return len;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;  // blank line / continuation
    std::string key = toLowerAscii(line.substr(0, colon));
    size_t valueStart = line.find_first_not_of(' ', colon + 1);
    std::string value = valueStart == std::string::npos ? "" : line.substr(valueStart);
    (*headers)[key] = Value::fromString(value);
    return len;
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

// Parses header_json (a JSON object of name->text) into a curl_slist,
// or returns an error string via `errOut` (empty on success). Shared
// by minta() and minta_stream().
bool buildHeaderList(const std::string& headerJson, struct curl_slist** outList, std::string& errOut) {
    Value headerIn;
    try {
        headerIn = json::decode(headerJson);
    } catch (const std::exception& e) {
        errOut = std::string("header_json invalid: ") + e.what();
        return false;
    }
    if (headerIn.type != ValueType::Map) {
        errOut = "header_json harus JSON object (mis. \"{}\" kalau nggak ada)";
        return false;
    }
    struct curl_slist* list = nullptr;
    for (const auto& [key, val] : *headerIn.map()) {
        if (val.type != ValueType::String) {
            curl_slist_free_all(list);
            errOut = "header_json: value tiap key harus teks";
            return false;
        }
        std::string line = key + ": " + val.str();
        list = curl_slist_append(list, line.c_str());
    }
    *outList = list;
    return true;
}

void applyCommonOpts(CURL* curl, const std::string& metode, const std::string& url, long timeoutMs = 0) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, metode.c_str());
    if (metode == "HEAD") {
        // CURLOPT_CUSTOMREQUEST doang gak cukup -- curl tetep nunggu body
        // sepanjang Content-Length dan lapor "Transferred a partial file"
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    if (timeoutMs > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // verifikasi sertifikat TLS selalu nyala, gak ada opsi matiin
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

NsValue httpMinta(int argc, const NsValue* argv) {
    if ((argc != 5 && argc != 6) || argv[0].type != NS_STRING || argv[1].type != NS_STRING ||
        argv[2].type != NS_STRING || argv[3].type != NS_STRING || argv[4].type != NS_STRING ||
        (argc == 6 && argv[5].type != NS_NUMBER)) {
        return errorEnvelope("minta(metode, api_url, path, header_json, tubuh, [timeout_ms]): argumen nggak valid");
    }
    std::string metode = argv[0].str;
    std::string url = joinUrl(argv[1].str, argv[2].str);
    std::string tubuh = argv[4].str;
    long timeoutMs = argc == 6 ? static_cast<long>(argv[5].number) : 0;

    struct curl_slist* headerList = nullptr;
    std::string headerErr;
    if (!buildHeaderList(argv[3].str, &headerList, headerErr)) return errorEnvelope(headerErr);

    ensureCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headerList);
        return errorEnvelope("curl_easy_init() gagal");
    }

    std::string responseBody;
    std::unordered_map<std::string, Value> responseHeaders;

    applyCommonOpts(curl, metode, url, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBodyCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);

    if (!tubuh.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, tubuh.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(tubuh.size()));
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return errorEnvelope("request gagal: " + err);
    }

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["status"] = Value::fromNumber(static_cast<double>(statusCode));
    (*out.map())["header"] = Value::fromMap(std::make_shared<std::unordered_map<std::string, Value>>(responseHeaders));
    (*out.map())["tubuh"] = Value::fromString(responseBody);
    return nsString(json::encode(out));
}

// ---- streaming: minta_stream / baca_stream / status_stream / header_stream / tutup_stream ----

struct HttpStreamState {
    std::mutex mu;
    std::condition_variable cv;
    std::string buffer;       // body bytes received but not yet baca_stream()'d out
    bool headersReady = false;
    bool done = false;        // transfer finished (success or failure)
    long status = 0;
    std::string error;        // non-empty if the transfer failed
    std::unordered_map<std::string, Value> headers;
    std::atomic<bool> abort{false};
    std::thread worker;
};

std::mutex g_streamTableMu;
std::unordered_map<int, HttpStreamState*> g_streamTable;
int g_nextStreamHandle = 1;

size_t streamWriteBodyCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* st = static_cast<HttpStreamState*>(userdata);
    size_t len = size * nmemb;
    if (st->abort.load()) return 0;  // 0 != len -> curl treats this as a write error, aborts the transfer
    {
        std::lock_guard<std::mutex> lock(st->mu);
        st->buffer.append(ptr, len);
    }
    st->cv.notify_one();
    return len;
}

size_t streamWriteHeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* st = static_cast<HttpStreamState*>(userdata);
    size_t len = size * nitems;
    std::string line(buffer, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::lock_guard<std::mutex> lock(st->mu);
    if (line.rfind("HTTP/", 0) == 0) {
        // status line baru -- awal blok header baru, di-clear tiap hop redirect
        st->headers.clear();
        size_t firstSpace = line.find(' ');
        if (firstSpace != std::string::npos) {
            st->status = std::atol(line.c_str() + firstSpace + 1);
        }
        return len;
    }
    if (line.empty()) {
        // baris kosong = akhir blok header ini
        st->headersReady = true;
        st->cv.notify_one();
        return len;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;  // continuation line
    std::string key = toLowerAscii(line.substr(0, colon));
    size_t valueStart = line.find_first_not_of(' ', colon + 1);
    std::string value = valueStart == std::string::npos ? "" : line.substr(valueStart);
    st->headers[key] = Value::fromString(value);
    return len;
}

int streamProgressCb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* st = static_cast<HttpStreamState*>(clientp);
    return st->abort.load() ? 1 : 0;  // non-zero aborts the transfer (CURLE_ABORTED_BY_CALLBACK)
}

void streamWorkerMain(HttpStreamState* st, std::string metode, std::string url, std::string headerJson,
                       std::string tubuh, long timeoutMs) {
    struct curl_slist* headerList = nullptr;
    std::string headerErr;
    CURLcode res = CURLE_OK;

    if (!buildHeaderList(headerJson, &headerList, headerErr)) {
        std::lock_guard<std::mutex> lock(st->mu);
        st->error = headerErr;
        st->done = true;
        st->headersReady = true;
        st->cv.notify_all();
        return;
    }

    ensureCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headerList);
        std::lock_guard<std::mutex> lock(st->mu);
        st->error = "curl_easy_init() gagal";
        st->done = true;
        st->headersReady = true;
        st->cv.notify_all();
        return;
    }

    applyCommonOpts(curl, metode, url, timeoutMs);
    // stream gak auto-follow redirect (beda dari minta()) -- info_stream()
    // gak ada cara reliable nebak masih ada hop lagi apa nggak; caller yang
    // butuh ngikutin redirect cek status 3xx terus minta_stream() lagi manual
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteBodyCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, st);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamWriteHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, streamProgressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, st);
    if (!tubuh.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, tubuh.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(tubuh.size()));
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    res = curl_easy_perform(curl);

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    std::lock_guard<std::mutex> lock(st->mu);
    st->status = statusCode;
    st->headersReady = true;
    if (res != CURLE_OK && !st->abort.load()) st->error = curl_easy_strerror(res);
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
    (*out.map())["header"] = Value::fromMap(std::make_shared<std::unordered_map<std::string, Value>>(st->headers));
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

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "minta", httpMinta);
    reg(registry, "minta_stream", httpMintaStream);
    reg(registry, "baca_stream", httpBacaStream);
    reg(registry, "info_stream", httpInfoStream);
    reg(registry, "tutup_stream", httpTutupStream);
}
