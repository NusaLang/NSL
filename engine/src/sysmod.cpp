#include "sysmod.hpp"

#include <map>
#include <mutex>
#include <stdexcept>

#include "plugin.hpp"
#include "sysplugin.hpp"

namespace sysmod {

Value call(const std::string& plugin, const std::string& function, std::vector<Value> args) {
    static std::mutex mu;
    static std::map<std::string, Value> loaded;
    Value module;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = loaded.find(plugin);
        if (it == loaded.end()) {
            std::string dir = sysplugin::systemPluginDir();
            std::string path = dir + "/" + plugin + ".so";
            try {
                it = loaded.emplace(plugin, ::plugin::load(path)).first;
            } catch (const std::exception& e) {
                throw std::runtime_error("modul sistem '" + plugin + "' nggak ditemukan (" + path +
                                         "): jalankan `make plugins`. " + e.what());
            }
        }
        module = it->second;
    }
    auto fn = module.map()->find(function);
    if (fn == module.map()->end() || !fn->second.native()) {
        throw std::runtime_error("modul sistem '" + plugin + "' nggak punya fungsi '" + function + "'");
    }
    return ::plugin::call(*fn->second.native(), args);
}

}  // namespace sysmod
