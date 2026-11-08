// `nusa update` -- replace this binary (and its plugins) with the latest release.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "http_client.hpp"
#include "i18n.hpp"
#include "sysplugin.hpp"

namespace {

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string versionOf(const std::string& binary) {
    std::string cmd = shellQuote(binary) + " --version 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[128];
    std::string out;
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

int runUpdate(const std::string& currentVersion) {
    std::string exe = sysplugin::selfExePath();
    if (exe.empty()) {
        std::cerr << i18n::tr("nusa update: nggak bisa menemukan lokasi binary ini\n", "nusa update: cannot locate this binary\n");
        return 1;
    }
    std::string asset, pluginAsset;
#if defined(__ANDROID__)
    asset = "nusa-android-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    asset = "nusa-linux-x86_64";
    pluginAsset = "nusantara-plugins-linux-x86_64.tar.gz";
#elif defined(__linux__) && defined(__aarch64__)
    asset = "nusa-linux-arm64";
    pluginAsset = "nusantara-plugins-linux-arm64.tar.gz";
#else
    std::cerr << i18n::tr("nusa update: belum ada binary rilis untuk platform ini -- bangun dari source (git pull && make)\n",
                          "nusa update: no prebuilt release for this platform -- rebuild from source (git pull && make)\n");
    return 1;
#endif
    const char* env = std::getenv("NUSA_UPDATE_URL");
    std::string base = env && *env ? env : "https://github.com/NusaLang/NSL/releases/latest/download";
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string dir = sysplugin::dirNameOf(exe);

    std::cout << i18n::tr("Memperbarui ", "Updating ") << exe << " (" << currentVersion << ")\n" << std::flush;
    std::string tmp = exe + ".new";
    std::string err;
    std::cout << "  " << i18n::tr("unduh ", "download ") << asset << " ...\n" << std::flush;
    if (!httpclient::downloadFile(base + "/" + asset, tmp, err)) {
        std::cerr << i18n::tr("nusa update: gagal mengunduh: ", "nusa update: download failed: ") << err << "\n";
        return 1;
    }
    chmod(tmp.c_str(), 0755);
    std::string newVersion = versionOf(tmp);
    if (newVersion.empty()) {
        std::remove(tmp.c_str());
        std::cerr << i18n::tr("nusa update: binary yang diunduh tidak bisa dijalankan; tidak diganti\n",
                              "nusa update: the downloaded binary does not run; left unchanged\n");
        return 1;
    }
    if (std::rename(tmp.c_str(), exe.c_str()) != 0) {
        std::remove(tmp.c_str());
        std::cerr << i18n::tr("nusa update: gagal mengganti binary (izin folder?): ", "nusa update: could not replace the binary (permissions?): ")
                  << exe << "\n";
        return 1;
    }
    std::cout << "  " << i18n::tr("terpasang: ", "installed: ") << newVersion << "\n";

    if (!pluginAsset.empty()) {
        std::string plugDir = dir + "/nusantara-plugins";
        struct stat st;
        bool isLink = lstat(plugDir.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        if (!isLink) {
            std::string tar = dir + "/.nusa-plugins.tar.gz";
            std::cout << "  " << i18n::tr("unduh ", "download ") << pluginAsset << " ...\n" << std::flush;
            if (httpclient::downloadFile(base + "/" + pluginAsset, tar, err)) {
                std::string cmd = "rm -rf " + shellQuote(plugDir) + " && mkdir -p " + shellQuote(plugDir) + " && tar -xzf " +
                                  shellQuote(tar) + " -C " + shellQuote(plugDir);
                int rc = std::system(cmd.c_str());
                std::remove(tar.c_str());
                if (rc != 0) {
                    std::cerr << i18n::tr("  peringatan: plugin gagal dipasang; binary tetap diperbarui\n",
                                          "  warning: plugins were not installed; the binary was still updated\n");
                }
            } else {
                std::cerr << i18n::tr("  peringatan: plugin gagal diunduh (", "  warning: plugins failed to download (") << err
                          << i18n::tr("); binary tetap diperbarui\n", "); the binary was still updated\n");
            }
        }
    }
    std::cout << i18n::tr("Selesai.\n", "Done.\n");
    return 0;
}
