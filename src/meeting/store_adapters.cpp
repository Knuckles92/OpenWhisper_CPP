#include "meeting/store_adapters.h"

#include <utility>
#include <vector>

#include "meeting/detail/py_value.h"

namespace fv::meeting {

StoreCaptureSink::StoreCaptureSink(
    StoreLookup store, std::function<void(std::string note)> emit_status,
    std::function<void(std::string code, std::string message)> emit_error)
    : store_(std::move(store)), emit_status_(std::move(emit_status)),
      emit_error_(std::move(emit_error)) {}

Json StoreCaptureSink::capture_state() {
    const auto store = store_();
    if (!store) {
        return Json();
    }
    return store->with_state([](const meeting_state::MeetingState& state) {
        return state.capture.is_object() ? state.capture : Json::object();
    });
}

bool StoreCaptureSink::update_capture(const Json& capture) {
    const auto store = store_();
    if (!store) {
        return false;
    }
    return store->update_runtime_fields(Json{{"capture", capture}});
}

void StoreCaptureSink::emit_status(std::string note) {
    emit_status_(std::move(note));
}

void StoreCaptureSink::emit_error(std::string code, std::string message) {
    emit_error_(std::move(code), std::move(message));
}

StoreDiarizerHost::StoreDiarizerHost(StoreLookup store,
                                     std::function<void(const diarize::RelabelOps&)> relabel)
    : store_(std::move(store)), relabel_(std::move(relabel)) {}

std::expected<std::string, std::string>
StoreDiarizerHost::create_participant(std::string display_name) {
    const auto store = store_();
    if (!store) {
        return std::unexpected("no result");
    }
    const Json ops = Json::array({Json{{"op", "upsert_participant"},
                                       {"display_name", std::move(display_name)},
                                       {"kind", "others_cluster"},
                                       {"is_provisional", true}}});
    const std::vector<meeting_state::OpResult> results =
        store->apply("system", std::string("diarizer"), ops);
    if (results.empty()) {
        return std::unexpected("no result");
    }
    if (!results.front().ok) {
        return std::unexpected(results.front().reason.value_or("None"));
    }
    const std::string id =
        detail::str_or_empty(detail::get(detail::get(results.front().effect, "participant"), "id"));
    if (!id.empty()) {
        return id;
    }
    return results.front().target_id.value_or("");
}

void StoreDiarizerHost::relabel(const diarize::RelabelOps& segment_to_participant) {
    if (relabel_) {
        relabel_(segment_to_participant);
    }
}

std::map<std::string, std::string> StoreDiarizerHost::participant_names() {
    const auto store = store_();
    if (!store) {
        return {};
    }
    return store->with_state([](const meeting_state::MeetingState& state) {
        std::map<std::string, std::string> names;
        for (const auto& [id, participant] : state.participants) {
            names.emplace(id, participant.display_name);
        }
        return names;
    });
}

} // namespace fv::meeting
