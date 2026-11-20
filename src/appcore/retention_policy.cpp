#include "appcore/retention_policy.h"

#include <format>

#include "history/format_utils.h"
#include "history/retention_settings.h"
#include "settings/keys.h"

namespace fv::appcore {

namespace {

std::string_view noun(std::int64_t count) {
    return count == 1 ? "recording" : "recordings";
}

} // namespace

RetentionLimits retention_limits(const Json& settings) {
    return {resolve_max_saved_recordings(settings), resolve_max_saved_recordings_bytes(settings)};
}

RetentionLimits retention_limits(const Json& mode, const Json& count, const Json& mb) {
    return retention_limits(retention_edits(mode, count, mb));
}

Json retention_edits(const Json& mode, const Json& count, const Json& mb) {
    Json edits = Json::object();
    edits[std::string(keys::recording_retention_mode.key())] = mode;
    edits[std::string(keys::max_saved_recordings.key())] = count;
    edits[std::string(keys::max_saved_recordings_mb.key())] = mb;
    return edits;
}

bool retention_changed(const RetentionLimits& saved, const RetentionLimits& pending) {
    return saved != pending;
}

bool shrinks(const RetentionLimits& saved, const RetentionLimits& pending) {
    return retention_changed(saved, pending) &&
           (pending.max_recordings.has_value() || pending.max_bytes.has_value());
}

std::string delete_confirm_title(std::int64_t count) {
    return std::format("Delete {} saved {}?", count, noun(count));
}

std::string delete_confirm_text(std::int64_t count, std::int64_t bytes) {
    return std::format("The new limit permanently deletes {} older saved {} ({}) from disk right "
                       "away. This can't be undone.\n\nTranscription history text is kept.",
                       count, noun(count), format_file_size(static_cast<double>(bytes)));
}

std::string recordings_usage_text(std::int64_t count, std::int64_t bytes) {
    return std::format("Currently {} {} using {}.", count, noun(count),
                       format_file_size(static_cast<double>(bytes)));
}

std::string retention_check_failed(std::string_view error) {
    return std::format("Couldn't check saved recordings: {}", error);
}

} // namespace fv::appcore
