// MeetingRuntime's engine factory with a paired computer (meeting/asr/remote.py ensure_ready):
// a host that could not be reached and gave no reason is named in the failure.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>

#include "runtime_support.h"
#include "speech/service.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

namespace {

// A paired computer's backend that never connects and says nothing about why.
class SilentHost final : public fv::speech::Backend {
public:
    std::string_view id() const override { return "remote"; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {fv::speech::ReadyState::Offline, "", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob&) override {
        throw fv::EngineError("not connected");
    }
    void release() override {}
};

} // namespace

TEST_CASE("a remote meeting whose host cannot be reached names the host",
          "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.confirm_live_audio = [](const Json&) {
            return true;
        };
        deps.engine_factory = {}; // the app's: it connects to the paired computer first
        deps.live_remote_backend = [](std::shared_ptr<Json>) {
            return std::make_unique<SilentHost>();
        };
    });
    const std::string pin(64, 'A');
    rig.settings.update(Json{{"meeting_speech_separate", true},
                             {"meeting_asr_source", "remote"},
                             {"fv_meeting_remote_fingerprint", pin},
                             {"remote_engine_client",
                              {{"host", "127.0.0.1"},
                               {"port", 8750},
                               {"host_name", "devbox"},
                               {"device_id", "me"},
                               {"fingerprint", pin}}}});

    rig.rt->start_meeting(false);
    rig.drain();

    const auto said = [&rig](std::string_view text) {
        const auto has = [text](const std::string& line) {
            return line.find(text) != std::string::npos;
        };
        return std::ranges::any_of(rig.got.errors, has) ||
               std::ranges::any_of(rig.got.statuses, has);
    };
    INFO("errors: " << Json(rig.got.errors).dump()
                    << " statuses: " << Json(rig.got.statuses).dump());
    CHECK(said("Cannot connect to devbox."));
    CHECK_FALSE(rig.probe().controller_active());
}
