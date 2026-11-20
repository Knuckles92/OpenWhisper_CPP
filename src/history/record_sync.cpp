#include "history/record_sync.h"

namespace fv {

RecordSync& null_record_sync() {
    static NullRecordSync sync;
    return sync;
}

} // namespace fv
