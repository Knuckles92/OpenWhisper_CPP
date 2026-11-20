#include "meeting_state/segment_ops.h"

#include <exception>
#include <optional>
#include <utility>

#include "base/log.h"
#include "meeting_state/detail/py_value.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.state.segment_ops");

using detail::get;
using detail::truthy;

} // namespace

Json handle_segment_op(MeetingRepository& repository, std::string_view meeting_id,
                       OpResult& result) {
    const Json effect = truthy(result.effect) ? result.effect : Json::object();
    const Json op_name = get(result.op, "op");
    const Json segment_id = get(effect, "segment_id");
    std::optional<Json> prior;
    if (segment_id.is_string()) {
        try {
            prior = repository.get_segment(meeting_id, segment_id.get_ref<const std::string&>());
        } catch (const std::exception& error) {
            logger.error_with(error, "Failed to read prior segment " + detail::py_str(segment_id));
        }
    }
    if (!prior) {
        return Json();
    }

    if (op_name.is_string() && op_name.get_ref<const std::string&>() == "revise_segment_text") {
        // Persistence is on_ops_applied's; the broadcast effect gains the post-apply row.
        const Json& raw_text = get(effect, "text");
        const Json new_text = truthy(raw_text) ? raw_text : Json("");
        Json updated = *prior;
        updated["text"] = new_text;
        updated["original_text"] = new_text;
        Json enriched = Json::object();
        enriched["entity"] = "segment_text";
        enriched["segment_id"] = segment_id;
        enriched["text"] = new_text;
        enriched["segment"] = std::move(updated);
        result.effect = std::move(enriched);
        // Reads project active human corrections into "text"; undo restores the stored ASR or
        // polish text, never that view.
        const Json& original = get(*prior, "original_text");
        const Json& shown = get(*prior, "text");
        Json inverse = Json::object();
        inverse["op"] = "revise_segment_text";
        inverse["segment_id"] = segment_id;
        inverse["text"] = original.is_string() ? original : (truthy(shown) ? shown : Json(""));
        inverse["evidence"] = Json::array({segment_id});
        return inverse;
    }

    Json inverse = Json::object();
    inverse["op"] = "reassign_segment_speaker";
    inverse["segment_id"] = segment_id;
    inverse["participant_id"] = get(*prior, "speaker_participant_id");
    inverse["_source"] =
        detail::has(*prior, "speaker_source") ? get(*prior, "speaker_source") : Json("channel");
    inverse["_pinned"] = truthy(get(*prior, "speaker_pinned"));
    inverse["force"] = true;
    return inverse;
}

SegmentHandler make_segment_handler(std::shared_ptr<MeetingRepository> repository,
                                    std::string meeting_id) {
    return [repository = std::move(repository),
            meeting_id = std::move(meeting_id)](OpResult& result) -> Json {
        if (!repository) {
            // Python's handler over None fails its prior-row read, logs it and has no inverse.
            logger.error("Failed to read prior segment {}: there is no repository",
                         detail::py_str(get(result.effect, "segment_id")));
            return Json();
        }
        return handle_segment_op(*repository, meeting_id, result);
    };
}

} // namespace fv::meeting_state
