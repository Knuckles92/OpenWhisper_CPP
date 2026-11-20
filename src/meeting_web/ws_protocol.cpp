#include "meeting_web/ws_protocol.h"

#include <algorithm>
#include <utility>

#include "meeting_web/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_web {

namespace {

bool guest_id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

Json optional_text(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json();
}

Json optional_int(const std::optional<std::int64_t>& value) {
    return value ? Json(*value) : Json();
}

Json patch(std::int64_t seq, Json results) {
    return Json{{"type", "patch"}, {"seq", seq}, {"results", std::move(results)}};
}

Json patch_row(const meeting_state::OpResult& result) {
    return Json{{"op", result.op},
                {"target_id", optional_text(result.target_id)},
                {"effect", result.effect},
                {"seq", optional_int(result.seq)}};
}

} // namespace

std::optional<std::string> clean_guest_id(std::optional<std::string_view> value) {
    std::string candidate = py::strip(value.value_or(std::string_view()));
    if (candidate.empty() || candidate.size() > 64 ||
        !std::all_of(candidate.begin(), candidate.end(), guest_id_char)) {
        return std::nullopt;
    }
    return candidate;
}

std::optional<std::int64_t> coerce_seq(const Json& value) {
    const auto seq = py::py_int(value);
    if (!seq || *seq < 0 || *seq > kMaxEventSeq) {
        return std::nullopt;
    }
    return seq;
}

bool is_host_only_effect(const meeting_state::OpResult& result) {
    if (!result.effect.is_object()) {
        return false;
    }
    const Json& entity = detail::get(result.effect, "entity");
    if (!entity.is_string()) {
        return false;
    }
    const auto& name = entity.get_ref<const std::string&>();
    return std::find(kHostOnlyEntities.begin(), kHostOnlyEntities.end(), name) !=
           kHostOnlyEntities.end();
}

std::vector<Broadcast> split_store_batch(std::int64_t seq,
                                         const std::vector<meeting_state::OpResult>& applied) {
    Json shared = Json::array();
    Json host_rows = Json::array();
    for (const auto& result : applied) {
        Json row = patch_row(result);
        if (!is_host_only_effect(result)) {
            shared.push_back(row);
        }
        host_rows.push_back(std::move(row));
    }
    std::vector<Broadcast> out;
    const bool split = host_rows.size() != shared.size();
    if (!shared.empty()) {
        out.push_back({patch(seq, std::move(shared)), false});
    }
    if (split) {
        out.push_back({patch(seq, std::move(host_rows)), true});
    }
    return out;
}

std::variant<Json, FrameError> check_text_frame(std::string_view text) {
    if (py::len(text) > kMaxMessageChars) {
        return FrameError{"message_too_large", "Message exceeds the size limit."};
    }
    Json parsed;
    try {
        parsed = py::loads(text);
    } catch (const py::ValueError&) {
        return FrameError{"bad_json", "Message was not valid JSON."};
    } catch (const py::RecursionError&) {
        return FrameError{"bad_json", "Message was not valid JSON."};
    }
    if (!parsed.is_object()) {
        return FrameError{"bad_message", "Message must be a JSON object."};
    }
    return parsed;
}

FrameError binary_frame_error() {
    return {"binary_unsupported", "Binary frames are not supported."};
}

FrameError unknown_type_error(const Json& type) {
    return {"unknown_type", "Unknown message type: " + detail::py_repr(type)};
}

FrameError internal_error() {
    return {"internal_error", "The server could not process that message."};
}

Json error_message(const FrameError& error) {
    return Json{{"type", "error"}, {"code", error.code}, {"message", error.message}};
}

Json pong_message() {
    return Json{{"type", "pong"}};
}

Json op_result_wire(const meeting_state::OpResult& result) {
    return Json{{"ok", result.ok},
                {"reason", optional_text(result.reason)},
                {"target_id", optional_text(result.target_id)},
                {"seq", optional_int(result.seq)},
                {"effect", result.effect}};
}

Json rejection(std::string_view reason) {
    return Json::array({Json{{"ok", false},
                             {"reason", std::string(reason)},
                             {"target_id", nullptr},
                             {"seq", nullptr},
                             {"effect", nullptr}}});
}

Json action_result(const Json& client_action_id, Json results) {
    return Json{{"type", "action_result"},
                {"client_action_id", client_action_id},
                {"results", std::move(results)}};
}

Json presence(std::string_view event, const Json& participant) {
    return Json{{"type", "presence"}, {"event", std::string(event)}, {"participant", participant}};
}

std::string encode(const Json& message) {
    py::DumpOptions options;
    options.ensure_ascii = false;
    return py::dumps(message, options);
}

} // namespace fv::meeting_web
