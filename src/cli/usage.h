#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fv::cli {

// A command line that cannot be run: main prints the message and the usage and exits 2.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What --help prints (main.py's _USAGE with this executable's commands). `product` is
// identity().product and `env_prefix` identity().env_prefix; the engine-only build has no
// identity and passes the same two literals.
std::string usage_text(std::string_view product, std::string_view env_prefix);

} // namespace fv::cli
