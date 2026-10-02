#include "plugin_abi.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include "quirc.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

namespace {

std::vector<std::string> decode(const std::string& imageBytes) {
    std::vector<std::string> out;

    int w = 0, h = 0, channels = 0;
    unsigned char* pix = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(imageBytes.data()),
        static_cast<int>(imageBytes.size()), &w, &h, &channels, 1);
    if (!pix) return out;

    struct quirc* q = quirc_new();
    if (!q) {
        stbi_image_free(pix);
        return out;
    }
    if (quirc_resize(q, w, h) < 0) {
        quirc_destroy(q);
        stbi_image_free(pix);
        return out;
    }

    int qw = 0, qh = 0;
    uint8_t* buf = quirc_begin(q, &qw, &qh);
    std::memcpy(buf, pix, static_cast<size_t>(qw) * static_cast<size_t>(qh));
    quirc_end(q);

    int count = quirc_count(q);
    // heap, not stack -- quirc_code/quirc_data are several KB each
    auto code = std::make_unique<quirc_code>();
    auto data = std::make_unique<quirc_data>();
    for (int i = 0; i < count; i++) {
        quirc_extract(q, i, code.get());
        if (quirc_decode(code.get(), data.get()) == QUIRC_SUCCESS) {
            out.emplace_back(reinterpret_cast<const char*>(data->payload), data->payload_len);
        }
    }

    quirc_destroy(q);
    stbi_image_free(pix);
    return out;
}

// Hasil: tiap payload sebagai "<panjang>:<byte>" berurutan (aman untuk data biner).
NsValue qrBaca(int argc, const NsValue* argv) {
    NsValue out{};
    out.type = NS_STRING;
    std::string packed;
    if (argc == 1 && argv[0].type == NS_STRING && argv[0].str) {
        size_t n = argv[0].str_len > 0 ? static_cast<size_t>(argv[0].str_len) : std::strlen(argv[0].str);
        for (const std::string& p : decode(std::string(argv[0].str, n))) packed += std::to_string(p.size()) + ":" + p;
    }
    out.str = static_cast<char*>(std::malloc(packed.size() + 1));
    std::memcpy(out.str, packed.data(), packed.size());
    out.str[packed.size()] = '\0';
    out.str_len = static_cast<int>(packed.size());
    return out;
}

}  // namespace

extern "C" int nusa_abi_version(void) { return NS_PLUGIN_ABI_VERSION; }

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "qr_baca", qrBaca);
}
