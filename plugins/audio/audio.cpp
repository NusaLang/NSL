#include "plugin_abi.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "value.hpp"

namespace {

struct WavAudio {
    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<unsigned char> data;  // raw PCM bytes, "data" chunk only
    bool ok = false;
};

uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t readU16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Generic RIFF/WAVE chunk walk -- only "fmt " (PCM) and "data" matter here,
// everything else (LIST, fact, ...) gets skipped. Odd-sized chunks are
// padded to an even boundary per the RIFF spec.
WavAudio loadWav(const std::string& path) {
    WavAudio w;
    std::ifstream f(path, std::ios::binary);
    if (!f) return w;
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 12) return w;
    if (memcmp(buf.data(), "RIFF", 4) != 0 || memcmp(buf.data() + 8, "WAVE", 4) != 0) return w;

    size_t pos = 12;
    bool haveFmt = false;
    bool haveData = false;
    while (pos + 8 <= buf.size()) {
        char id[5] = {0};
        memcpy(id, &buf[pos], 4);
        uint32_t size = readU32(&buf[pos + 4]);
        size_t bodyStart = pos + 8;
        if (bodyStart + size > buf.size()) break;

        if (memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            w.audioFormat = readU16(&buf[bodyStart]);
            w.channels = readU16(&buf[bodyStart + 2]);
            w.sampleRate = readU32(&buf[bodyStart + 4]);
            w.bitsPerSample = readU16(&buf[bodyStart + 14]);
            haveFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            w.data.assign(buf.begin() + static_cast<long>(bodyStart), buf.begin() + static_cast<long>(bodyStart + size));
            haveData = true;
        }
        pos = bodyStart + size + (size % 2);
    }

    if (!haveFmt || !haveData) return w;
    if (w.audioFormat != 1) return w;  // cuma PCM mentah, bukan compressed/extensible
    if (w.bitsPerSample != 8 && w.bitsPerSample != 16 && w.bitsPerSample != 24 && w.bitsPerSample != 32) return w;
    if (w.channels == 0) return w;
    w.ok = true;
    return w;
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

NsValue errorEnvelope(const std::string& msg) {
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(false);
    (*m.map())["error"] = Value::fromString(msg);
    return nsString(json::encode(m));
}

NsValue wavInfo(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return errorEnvelope("wav_info(path): argumen nggak valid");
    WavAudio w = loadWav(argv[0].str);
    if (!w.ok) return errorEnvelope("gagal buka/parse WAV (cuma dukung PCM mentah, 8/16/24/32-bit)");
    size_t bytesPerSample = static_cast<size_t>(w.bitsPerSample) / 8;
    size_t numSamples = bytesPerSample > 0 ? w.data.size() / bytesPerSample / w.channels : 0;
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(true);
    (*m.map())["sample_rate"] = Value::fromNumber(w.sampleRate);
    (*m.map())["channels"] = Value::fromNumber(w.channels);
    (*m.map())["bit_depth"] = Value::fromNumber(w.bitsPerSample);
    (*m.map())["num_samples"] = Value::fromNumber(static_cast<double>(numSamples));
    return nsString(json::encode(m));
}

// Ekstrak `nBit` bit terendah dari byte PALING RENDAH tiap sample (byte
// pertama dalam urutan little-endian file WAV), across semua channel
// interleave apa adanya -- konvensi umum stego audio LSB.
NsValue wavLsbExtract(int argc, const NsValue* argv) {
    if (argc != 2 || argv[0].type != NS_STRING || argv[1].type != NS_NUMBER) return nsNull();
    WavAudio w = loadWav(argv[0].str);
    if (!w.ok) return nsNull();
    int nBit = static_cast<int>(argv[1].number);
    if (nBit < 1 || nBit > 8) return nsNull();

    size_t bytesPerSample = static_cast<size_t>(w.bitsPerSample) / 8;
    size_t stride = bytesPerSample;  // byte pertama tiap sample = LSB byte (little-endian)

    std::vector<unsigned char> out;
    unsigned char cur = 0;
    int filled = 0;
    for (size_t off = 0; off + stride <= w.data.size(); off += stride) {
        unsigned char v = w.data[off];
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
    if (filled > 0) {
        cur = static_cast<unsigned char>(cur << (8 - filled));
        out.push_back(cur);
    }
    return nsString(toHex(out.data(), out.size()));
}

}  // namespace

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "wav_info", wavInfo);
    reg(registry, "wav_lsb_extract", wavLsbExtract);
}
