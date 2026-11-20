#include "meeting_state/ids.h"

#include "base/ids.h"

namespace fv::meeting_state {

std::string RandomIds::hex12() {
    return uuid4_hex().substr(0, 12);
}

IdSource& random_ids() {
    static RandomIds ids;
    return ids;
}

std::string new_id(std::string_view prefix, IdSource& ids) {
    std::string id(prefix);
    id += '_';
    id += ids.hex12();
    return id;
}

} // namespace fv::meeting_state
