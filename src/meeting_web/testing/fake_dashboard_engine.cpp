#include "meeting_web/testing/fake_dashboard_engine.h"

#include <utility>

#include "meeting_state/store.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting_web::testing {

namespace {

// engine.py MAX_GUEST_NAME_LEN.
constexpr std::int64_t kMaxGuestNameLen = 80;

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json();
}

} // namespace

FakeDashboardEngine::FakeDashboardEngine(std::string meeting_id,
                                         std::shared_ptr<meeting_state::MeetingStateStore> store,
                                         std::filesystem::path spool_root)
    : meeting_id_(std::move(meeting_id)), store_(std::move(store)),
      spool_root_(std::move(spool_root)) {}

std::string FakeDashboardEngine::meeting_id() const {
    std::lock_guard lock(mutex_);
    return meeting_id_;
}

std::shared_ptr<meeting_state::MeetingStateStore> FakeDashboardEngine::store() const {
    std::lock_guard lock(mutex_);
    return store_;
}

bool FakeDashboardEngine::is_active() const {
    std::lock_guard lock(mutex_);
    return active_;
}

std::vector<meeting_state::OpResult> FakeDashboardEngine::apply_client_action(
    std::string_view actor, const std::optional<std::string>& participant_id, const Json& op) {
    record("apply_client_action",
           Json::array({std::string(actor), optional_json(participant_id), op}));
    ApplyFn apply;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    {
        std::lock_guard lock(mutex_);
        apply = apply_;
        store = store_;
    }
    if (apply) {
        return apply(actor, participant_id, op);
    }
    if (!store) {
        throw std::runtime_error("No active meeting");
    }
    return store->apply(actor, participant_id, Json::array({op}));
}

std::vector<meeting_state::OpResult>
FakeDashboardEngine::undo(std::int64_t seq, const std::optional<std::string>& participant_id) {
    record("undo", Json::array({seq, optional_json(participant_id)}));
    UndoFn undo;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    {
        std::lock_guard lock(mutex_);
        undo = undo_;
        store = store_;
    }
    if (undo) {
        return undo(seq, participant_id);
    }
    if (!store) {
        throw std::runtime_error("No active meeting");
    }
    return store->undo(seq, participant_id);
}

Json FakeDashboardEngine::add_guest(std::string_view name) {
    record("add_guest", Json::array({std::string(name)}));
    AddGuestFn add_guest;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    {
        std::lock_guard lock(mutex_);
        add_guest = add_guest_;
        store = store_;
    }
    if (add_guest) {
        return add_guest(name);
    }
    if (!store) {
        throw std::runtime_error("No active meeting");
    }
    std::string display = py::slice(py::strip(name), 0, kMaxGuestNameLen);
    if (display.empty()) {
        display = "Guest";
    }
    const auto results = store->apply("system", std::nullopt,
                                      Json::array({Json{{"op", "upsert_participant"},
                                                        {"display_name", display},
                                                        {"kind", "guest"},
                                                        {"is_provisional", false}}}));
    const auto& result = results.at(0);
    if (!result.ok || result.effect.is_null() ||
        (result.effect.is_object() && result.effect.empty())) {
        throw py::ValueError(result.reason.value_or("guest_rejected"));
    }
    return result.effect.at("participant");
}

void FakeDashboardEngine::end() {
    record("end", Json::array());
    run_hook("end", Json::array());
}

void FakeDashboardEngine::pause() {
    record("pause", Json::array());
    run_hook("pause", Json::array());
}

void FakeDashboardEngine::resume() {
    record("resume", Json::array());
    run_hook("resume", Json::array());
}

void FakeDashboardEngine::set_cloud_enabled(bool enabled) {
    record("set_cloud_enabled", Json::array({enabled}));
    run_hook("set_cloud_enabled", Json::array({enabled}));
}

void FakeDashboardEngine::request_note_adjustment(std::string text,
                                                  std::function<void(Json)> complete) {
    NoteRequestFn handler;
    {
        std::lock_guard lock(mutex_);
        handler = note_request_;
    }
    if (!handler) {
        DashboardEngine::request_note_adjustment(std::move(text), std::move(complete));
        return;
    }
    record("request_note_adjustment", Json::array({text}));
    handler(std::move(text), std::move(complete));
}

void FakeDashboardEngine::set_note_request(NoteRequestFn handler) {
    std::lock_guard lock(mutex_);
    note_request_ = std::move(handler);
}

void FakeDashboardEngine::set_lifecycle_hook(LifecycleHook hook) {
    std::lock_guard lock(mutex_);
    hook_ = std::move(hook);
}

void FakeDashboardEngine::run_hook(std::string_view method, const Json& args) {
    LifecycleHook hook;
    {
        std::lock_guard lock(mutex_);
        hook = hook_;
    }
    if (hook) {
        hook(method, args);
    }
}

Json FakeDashboardEngine::regenerate_tokens() {
    record("regenerate_tokens", Json::array());
    std::function<Json()> regenerate;
    {
        std::lock_guard lock(mutex_);
        regenerate = regenerate_;
    }
    if (regenerate) {
        return regenerate();
    }
    return Json{{"host_url", ""}, {"guest_url", ""}};
}

Json FakeDashboardEngine::recent_agent_activity() const {
    std::lock_guard lock(mutex_);
    return activity_;
}

std::filesystem::path FakeDashboardEngine::spool_root() const {
    std::lock_guard lock(mutex_);
    return spool_root_;
}

void FakeDashboardEngine::set_meeting_id(std::string meeting_id) {
    std::lock_guard lock(mutex_);
    meeting_id_ = std::move(meeting_id);
}

void FakeDashboardEngine::set_store(std::shared_ptr<meeting_state::MeetingStateStore> store) {
    std::lock_guard lock(mutex_);
    store_ = std::move(store);
}

void FakeDashboardEngine::set_active(bool active) {
    std::lock_guard lock(mutex_);
    active_ = active;
}

void FakeDashboardEngine::set_activity(Json ticks) {
    std::lock_guard lock(mutex_);
    activity_ = std::move(ticks);
}

void FakeDashboardEngine::set_apply(ApplyFn apply) {
    std::lock_guard lock(mutex_);
    apply_ = std::move(apply);
}

void FakeDashboardEngine::set_undo(UndoFn undo) {
    std::lock_guard lock(mutex_);
    undo_ = std::move(undo);
}

void FakeDashboardEngine::set_add_guest(AddGuestFn add_guest) {
    std::lock_guard lock(mutex_);
    add_guest_ = std::move(add_guest);
}

void FakeDashboardEngine::set_regenerate(std::function<Json()> regenerate) {
    std::lock_guard lock(mutex_);
    regenerate_ = std::move(regenerate);
}

std::vector<FakeDashboardEngine::Call> FakeDashboardEngine::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

std::vector<FakeDashboardEngine::Call> FakeDashboardEngine::calls(std::string_view method) const {
    std::lock_guard lock(mutex_);
    std::vector<Call> out;
    for (const auto& call : calls_) {
        if (call.method == method) {
            out.push_back(call);
        }
    }
    return out;
}

void FakeDashboardEngine::record(std::string method, Json args) {
    std::lock_guard lock(mutex_);
    calls_.push_back({std::move(method), std::move(args)});
}

} // namespace fv::meeting_web::testing
