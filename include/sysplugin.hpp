#pragma once

#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <climits>
#else
#include <unistd.h>
#endif

// Shared between interpreter.cpp (muat_plugin("name")) and main.cpp
// (`nusa get`, which needs the http plugin for HTTPS registry calls):
// where bundled "system" plugins live, and how to tell a bare module
// name apart from an explicit path.
namespace sysplugin {

inline std::string dirNameOf(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

// A bare name (no '/', doesn't end in ".so") is a *system* module --
// muat_plugin("http") instead of a path, same idea as Go's `import
// "net/http"` needing no filesystem path. Anything containing '/' or
// ending in ".so" is an explicit path instead (resolved relative to
// the caller's own directory) -- this check is purely additive.
inline bool isBareName(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".so") == 0) return false;
    if (name.size() >= 6 && name.compare(name.size() - 6, 6, ".dylib") == 0) return false;
    return true;
}

// Real path of the running executable (/proc/self/exe on Linux,
// _NSGetExecutablePath on macOS -- never argv[0]). "" if undeterminable.
inline std::string selfExePath() {
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) return "";
    return std::string(resolved);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::string(buf);
#endif
}

// "nusantara-plugins" next to the running binary -- system modules
// resolve here (see isBareName), same idea as Go's GOROOT.
inline std::string systemPluginDir() {
    std::string exe = selfExePath();
    if (exe.empty()) return "";
    return dirNameOf(exe) + "/nusantara-plugins";
}

// nusantara_modules next to the binary -- global installs (`nusa get
// -g`); local ./nusantara_modules/ shadows this when both exist.
inline std::string globalModulesDir() {
    std::string exe = selfExePath();
    if (exe.empty()) return "";
    return dirNameOf(exe) + "/nusantara_modules";
}

}  // namespace sysplugin
