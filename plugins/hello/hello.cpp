// contoh minimal plugin native -- spek ABI di include/plugin_abi.h, build: make plugins

#include "plugin_abi.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// argv[0] dijamin NS_STRING kalau dipanggil dari examples/plugin_demo.ns,
// tapi tetep dicek di sini -- plugin nggak boleh percaya begitu aja tipe
// argumen yang dikirim dari sisi Nusantara.
NsValue sapa(int argc, const NsValue* argv) {
    NsValue out{};
    out.type = NS_STRING;
    if (argc != 1 || argv[0].type != NS_STRING) {
        out.str = strdup("sapa(): butuh 1 argumen teks");
        return out;
    }
    std::string msg = std::string("halo, ") + argv[0].str + "! (dari plugin native C++)";
    out.str = strdup(msg.c_str());  // heap-allocated -- host yang free()
    return out;
}

NsValue tambah(int argc, const NsValue* argv) {
    NsValue out{};
    if (argc != 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_NUMBER) {
        out.type = NS_NULL;
        return out;
    }
    out.type = NS_NUMBER;
    out.number = argv[0].number + argv[1].number;
    return out;
}

}  // namespace

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "sapa", sapa);
    reg(registry, "tambah", tambah);
}
