#pragma once

#include <string>

#include "base/env.h"

namespace fv::agents::detail {

// os.path.expanduser("~") over `env`: USERPROFILE, else HOMEDRIVE and HOMEPATH on Windows;
// HOME elsewhere. Empty when there is none.
inline std::string home_folder(const EnvReader& env) {
#ifdef _WIN32
    if (auto profile = env.get("USERPROFILE"); profile && !profile->empty()) {
        return *profile;
    }
    if (auto path = env.get("HOMEPATH")) {
        return env.get("HOMEDRIVE").value_or("") + *path;
    }
    return {};
#else
    return env.get("HOME").value_or("");
#endif
}

} // namespace fv::agents::detail
