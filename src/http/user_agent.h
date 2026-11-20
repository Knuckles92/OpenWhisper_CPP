#pragma once

#include <string>

namespace fv::http {

// "<product>/<version>" ("OpenWhisper_CPP/0.1.0"), the User-Agent of downloads and update checks.
std::string user_agent();

} // namespace fv::http
