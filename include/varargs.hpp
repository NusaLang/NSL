#pragma once

#include <memory>
#include <string>
#include <vector>

#include "value.hpp"

// Binding of `*rest` / `**kw` parameters. `args` holds the positional arguments (excluding a
// method's `ini`), optionally followed by a map tagged "__kw__" carrying keyword arguments that
// matched no named parameter. Afterwards `args` has exactly one entry per declared parameter.
// Returns an error message, or "" on success.
inline std::string packVarargs(std::vector<Value>& args, int restIdx, int kwIdx, int minRequired) {
    std::shared_ptr<ValueMap> kwMap;
    if (!args.empty() && args.back().type == ValueType::Map && args.back().map()->count("__kw__")) {
        kwMap = std::make_shared<ValueMap>(*args.back().map());
        kwMap->erase("__kw__");
        args.pop_back();
    }
    size_t fixed = static_cast<size_t>(restIdx >= 0 ? restIdx : kwIdx);
    if (static_cast<int>(args.size()) < minRequired) {
        return "argumen kurang: butuh minimal " + std::to_string(minRequired) + ", dapat " + std::to_string(args.size());
    }
    std::vector<Value> extra;
    if (args.size() > fixed) {
        extra.assign(args.begin() + static_cast<long>(fixed), args.end());
        args.resize(fixed);
    }
    while (args.size() < fixed) args.push_back(Value::null());
    if (restIdx >= 0) {
        args.push_back(Value::fromArray(std::make_shared<std::vector<Value>>(std::move(extra))));
    } else if (!extra.empty()) {
        return "terlalu banyak argumen";
    }
    if (kwIdx >= 0) args.push_back(Value::fromMap(kwMap ? kwMap : std::make_shared<ValueMap>()));
    return "";
}
