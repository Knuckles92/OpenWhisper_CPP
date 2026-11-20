#include "speech/messages.h"

#include "base/identity.h"

namespace fv::speech {

std::string engine_stopped_message() {
    return "The speech engine stopped unexpectedly. Details are in " +
           std::string(identity().log_file) + ".";
}

} // namespace fv::speech
