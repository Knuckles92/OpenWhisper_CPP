#include "remote/detail/normcase.h"

namespace fv::remote::detail {

std::string normcase(std::string_view path) {
    return std::string(path);
}

} // namespace fv::remote::detail
