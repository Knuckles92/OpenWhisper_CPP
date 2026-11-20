#include "dictation/rewrite_jobs.h"

#include "dictation/delivery.h"

namespace fv::dictation {

std::string rewrite_copied_status(bool copied, PasteTarget target) {
    if (!copied) {
        return std::string(COPY_FAILED);
    }
    return target == PasteTarget::Changed
               ? "Rewrite copied — the app changed; paste it where you want"
               : "Rewrite copied — couldn't confirm the app; paste it where you want";
}

std::optional<PointerNotice> forced_outcome_notice(bool pasted, bool had_selection,
                                                   std::string_view delivery_status) {
    if (!pasted) {
        if (delivery_status.empty()) {
            return std::nullopt;
        }
        return PointerNotice{std::string(delivery_status), false};
    }
    return PointerNotice{had_selection ? "Rewritten" : "Inserted", true};
}

} // namespace fv::dictation
