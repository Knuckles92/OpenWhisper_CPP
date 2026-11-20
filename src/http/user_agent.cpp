#include "http/user_agent.h"

#include "base/identity.h"
#include "http/version.h"

namespace fv::http {

std::string user_agent() {
    std::string text(identity().product);
    text += '/';
    text += kAppVersion;
    return text;
}

} // namespace fv::http
