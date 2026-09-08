#include "plugin_abi.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "value.hpp"

namespace {

struct BmpImage {
    int32_t width = 0;
    int32_t height = 0;
    int bpp = 0;
    int bytesPerPixel = 0;
    size_t rowSize = 0;
    bool bottomUp = true;
    std::vector<unsigned char> pixels;
    bool ok = false;
};

uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t readU16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int32_t readI32(const unsigned char* p) { return static_cast<int32_t>(readU32(p)); }

BmpImage loadBmp(const std::string& path) {
    BmpImage img;
    std::ifstream f(path, std::ios::binary);
    if (!f) return img;
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 54) return img;
    if (buf[0] != 'B' || buf[1] != 'M') return img;
    uint32_t offBits = readU32(&buf[10]);
    int32_t width = readI32(&buf[18]);
    int32_t height = readI32(&buf[22]);
    uint16_t bpp = readU16(&buf[28]);
    uint32_t compression = readU32(&buf[30]);
    if (compression != 0) return img;
    if (bpp != 24 && bpp != 32) return img;
    if (width <= 0) return img;

    img.width = width;
    img.bottomUp = height > 0;
    img.height = height > 0 ? height : -height;
    if (img.height <= 0) return img;
    img.bpp = bpp;
    img.bytesPerPixel = bpp / 8;
    img.rowSize = ((static_cast<size_t>(width) * static_cast<size_t>(img.bytesPerPixel) + 3) / 4) * 4;

    size_t needed = static_cast<size_t>(offBits) + img.rowSize * static_cast<size_t>(img.height);
    if (buf.size() < needed) return img;

    img.pixels.resize(img.rowSize * static_cast<size_t>(img.height));
    memcpy(img.pixels.data(), buf.data() + offBits, img.pixels.size());
    img.ok = true;
    return img;
}

bool getPixel(const BmpImage& img, int x, int y, unsigned char& r, unsigned char& g, unsigned char& b,
              unsigned char& a) {
    if (!img.ok || x < 0 || y < 0 || x >= img.width || y >= img.height) return false;
    int fileRow = img.bottomUp ? (img.height - 1 - y) : y;
    size_t off = static_cast<size_t>(fileRow) * img.rowSize + static_cast<size_t>(x) * img.bytesPerPixel;
    b = img.pixels[off];
    g = img.pixels[off + 1];
    r = img.pixels[off + 2];
    a = img.bytesPerPixel == 4 ? img.pixels[off + 3] : 255;
    return true;
}

void setPixel(BmpImage& img, int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    int fileRow = img.bottomUp ? (img.height - 1 - y) : y;
    size_t off = static_cast<size_t>(fileRow) * img.rowSize + static_cast<size_t>(x) * img.bytesPerPixel;
    img.pixels[off] = b;
    img.pixels[off + 1] = g;
    img.pixels[off + 2] = r;
}

bool writeBmp(const std::string& path, const BmpImage& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t offBits = 54;
    uint32_t imageSize = static_cast<uint32_t>(img.rowSize * static_cast<size_t>(img.height));
    uint32_t fileSize = offBits + imageSize;
    unsigned char header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    auto put32 = [&](int pos, uint32_t v) {
        header[pos] = static_cast<unsigned char>(v & 0xFF);
        header[pos + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        header[pos + 2] = static_cast<unsigned char>((v >> 16) & 0xFF);
        header[pos + 3] = static_cast<unsigned char>((v >> 24) & 0xFF);
    };
    auto put16 = [&](int pos, uint16_t v) {
        header[pos] = static_cast<unsigned char>(v & 0xFF);
        header[pos + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    };
    put32(2, fileSize);
    put32(10, offBits);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(img.width));
    put32(22, static_cast<uint32_t>(img.bottomUp ? img.height : -img.height));
    put16(26, 1);
    put16(28, static_cast<uint16_t>(img.bpp));
    put32(34, imageSize);
    f.write(reinterpret_cast<const char*>(header), 54);
    f.write(reinterpret_cast<const char*>(img.pixels.data()), static_cast<std::streamsize>(img.pixels.size()));
    return true;
}

std::string toHex(const unsigned char* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out += d[(data[i] >> 4) & 0xF];
        out += d[data[i] & 0xF];
    }
    return out;
}

NsValue nsString(const std::string& s) {
    NsValue v{};
    v.type = NS_STRING;
    v.str = strdup(s.c_str());
    return v;
}
NsValue nsNull() {
    NsValue v{};
    v.type = NS_NULL;
    return v;
}
NsValue nsBool(bool b) {
    NsValue v{};
    v.type = NS_BOOL;
    v.boolean = b ? 1 : 0;
    return v;
}

NsValue errorEnvelope(const std::string& msg) {
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(false);
    (*m.map())["error"] = Value::fromString(msg);
    return nsString(json::encode(m));
}

NsValue bmpInfo(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return errorEnvelope("bmp_info(path): argumen nggak valid");
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok) return errorEnvelope("gagal buka/parse BMP (cuma dukung 24/32-bit uncompressed)");
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(true);
    (*m.map())["width"] = Value::fromNumber(img.width);
    (*m.map())["height"] = Value::fromNumber(img.height);
    (*m.map())["bit_depth"] = Value::fromNumber(img.bpp);
    return nsString(json::encode(m));
}

NsValue bmpPixel(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_STRING || argv[1].type != NS_NUMBER || argv[2].type != NS_NUMBER) {
        return errorEnvelope("bmp_pixel(path, x, y): argumen nggak valid");
    }
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok) return errorEnvelope("gagal buka/parse BMP");
    unsigned char r, g, b, a;
    if (!getPixel(img, static_cast<int>(argv[1].number), static_cast<int>(argv[2].number), r, g, b, a)) {
        return errorEnvelope("koordinat di luar batas gambar");
    }
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(true);
    (*m.map())["r"] = Value::fromNumber(r);
    (*m.map())["g"] = Value::fromNumber(g);
    (*m.map())["b"] = Value::fromNumber(b);
    if (img.bytesPerPixel == 4) (*m.map())["a"] = Value::fromNumber(a);
    return nsString(json::encode(m));
}

int channelIndex(char c) {
    if (c == 'r' || c == 'R') return 0;
    if (c == 'g' || c == 'G') return 1;
    if (c == 'b' || c == 'B') return 2;
    if (c == 'a' || c == 'A') return 3;
    return -1;
}

NsValue bmpChannelHex(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_STRING) return nsNull();
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok || strlen(argv[1].str) != 1) return nsNull();
    int ch = channelIndex(argv[1].str[0]);
    if (ch < 0 || (ch == 3 && img.bytesPerPixel != 4)) return nsNull();
    std::vector<unsigned char> out;
    out.reserve(static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            unsigned char r, g, b, a;
            getPixel(img, x, y, r, g, b, a);
            unsigned char v = ch == 0 ? r : ch == 1 ? g : ch == 2 ? b : a;
            out.push_back(v);
        }
    }
    return nsString(toHex(out.data(), out.size()));
}

NsValue bmpBitplaneHex(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_STRING || argv[1].type != NS_STRING || argv[2].type != NS_NUMBER) {
        return nsNull();
    }
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok || strlen(argv[1].str) != 1) return nsNull();
    int ch = channelIndex(argv[1].str[0]);
    int bit = static_cast<int>(argv[2].number);
    if (ch < 0 || bit < 0 || bit > 7 || (ch == 3 && img.bytesPerPixel != 4)) return nsNull();

    std::vector<unsigned char> out;
    unsigned char cur = 0;
    int filled = 0;
    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            unsigned char r, g, b, a;
            getPixel(img, x, y, r, g, b, a);
            unsigned char v = ch == 0 ? r : ch == 1 ? g : ch == 2 ? b : a;
            unsigned char bitVal = (v >> bit) & 1;
            cur = static_cast<unsigned char>((cur << 1) | bitVal);
            filled++;
            if (filled == 8) {
                out.push_back(cur);
                cur = 0;
                filled = 0;
            }
        }
    }
    if (filled > 0) {
        cur = static_cast<unsigned char>(cur << (8 - filled));
        out.push_back(cur);
    }
    return nsString(toHex(out.data(), out.size()));
}

NsValue bmpLsbExtract(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_STRING || argv[1].type != NS_STRING || argv[2].type != NS_NUMBER) {
        return nsNull();
    }
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok) return nsNull();
    std::string channels = argv[1].str;
    int nBit = static_cast<int>(argv[2].number);
    if (channels.empty() || nBit < 1 || nBit > 8) return nsNull();
    std::vector<int> chIdx;
    for (char c : channels) {
        int idx = channelIndex(c);
        if (idx < 0 || (idx == 3 && img.bytesPerPixel != 4)) return nsNull();
        chIdx.push_back(idx);
    }

    std::vector<unsigned char> out;
    unsigned char cur = 0;
    int filled = 0;
    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            unsigned char r, g, b, a;
            getPixel(img, x, y, r, g, b, a);
            unsigned char comp[4] = {r, g, b, a};
            for (int idx : chIdx) {
                unsigned char v = comp[idx];
                for (int bit = nBit - 1; bit >= 0; bit--) {
                    unsigned char bitVal = (v >> bit) & 1;
                    cur = static_cast<unsigned char>((cur << 1) | bitVal);
                    filled++;
                    if (filled == 8) {
                        out.push_back(cur);
                        cur = 0;
                        filled = 0;
                    }
                }
            }
        }
    }
    if (filled > 0) {
        cur = static_cast<unsigned char>(cur << (8 - filled));
        out.push_back(cur);
    }
    return nsString(toHex(out.data(), out.size()));
}

NsValue bmpFilter(int argc, const NsValue* argv) {
    if (argc != 3 || argv[0].type != NS_STRING || argv[1].type != NS_STRING || argv[2].type != NS_STRING) {
        return nsBool(false);
    }
    BmpImage img = loadBmp(argv[0].str);
    if (!img.ok) return nsBool(false);
    std::string filter = argv[2].str;
    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            unsigned char r, g, b, a;
            getPixel(img, x, y, r, g, b, a);
            if (filter == "grayscale") {
                unsigned char gray = static_cast<unsigned char>((r * 299 + g * 587 + b * 114) / 1000);
                setPixel(img, x, y, gray, gray, gray);
            } else if (filter == "invert") {
                setPixel(img, x, y, static_cast<unsigned char>(255 - r), static_cast<unsigned char>(255 - g),
                         static_cast<unsigned char>(255 - b));
            } else if (filter == "r") {
                setPixel(img, x, y, r, 0, 0);
            } else if (filter == "g") {
                setPixel(img, x, y, 0, g, 0);
            } else if (filter == "b") {
                setPixel(img, x, y, 0, 0, b);
            }
        }
    }
    return nsBool(writeBmp(argv[1].str, img));
}

}  // namespace

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "bmp_info", bmpInfo);
    reg(registry, "bmp_pixel", bmpPixel);
    reg(registry, "bmp_channel_hex", bmpChannelHex);
    reg(registry, "bmp_bitplane_hex", bmpBitplaneHex);
    reg(registry, "bmp_lsb_extract", bmpLsbExtract);
    reg(registry, "bmp_filter", bmpFilter);
}
