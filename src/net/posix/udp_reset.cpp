#include "net/detail/udp_reset.h"

#include <cstring>

namespace fv::net::detail {

void disable_udp_connreset(std::uintptr_t /*native_socket*/) {}

std::string os_error_text(int code) {
    std::string text = std::strerror(code);
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) <= ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

} // namespace fv::net::detail
