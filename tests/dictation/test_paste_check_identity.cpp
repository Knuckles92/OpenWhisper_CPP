// The paste check where the focused app is known only asynchronously
// (tests/test_personalize_fix_f2_paste_target.py): the check still answers, through the capture
// service's bounded identity_now, and an identity read that hangs counts as unknown within
// PASTE_CHECK_TIMEOUT. Windows reads the identity synchronously; the bound guards the UI thread
// wherever the read could block.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

#include "base/json.h"
#include "cleanup_support.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/focus_capture.h"
#include "focus/testing/fake_platform.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<fv::focus::FocusCapture>
capture_over(const std::shared_ptr<fv::focus::FakeFocusPlatform>& platform) {
    fv::focus::FocusCaptureOptions options;
    options.deadline = 1000ms;
    options.settings = [] {
        return std::make_shared<const fv::Json>(fv::Json::object());
    };
    options.metrics = [](const fv::focus::CaptureSample&) {
    };
    return std::make_shared<fv::focus::FocusCapture>(platform, std::move(options));
}

} // namespace

// from
// tests/test_personalize_fix_f2_paste_target.py::test_linux_answers_the_paste_check_although_its_identity_is_asynchronous
TEST_CASE("the paste check answers although the identity is asynchronous",
          "[test_personalize_fix_f2_paste_target]") {
    const auto notepad = app("notepad.exe", "Notepad", 4, "0x4");
    auto platform = std::make_shared<fv::focus::FakeFocusPlatform>(notepad, false, false);
    auto service = capture_over(platform);
    DictationJob job;
    job.mode = JobMode::Transform;
    job.target = service->request(false, true);
    REQUIRE(resolved(job.target, 2000ms));
    CHECK_FALSE(service->current_identity());

    CHECK(paste_target(&job, service.get()) == PasteTarget::Same);

    platform->set_current(app("chrome.exe", "Google Chrome", 8, "0x8"));
    CHECK(paste_target(&job, service.get()) == PasteTarget::Changed);
    CHECK_FALSE(paste_target_ok(&job, service.get()));
    service->shutdown();
}

// from
// tests/test_personalize_fix_f2_paste_target.py::test_a_hung_linux_identity_read_is_bounded_and_unknown
TEST_CASE("a hung identity read is bounded and unknown", "[test_personalize_fix_f2_paste_target]") {
    const auto notepad = app("notepad.exe", "Notepad", 4, "0x4");
    auto platform = std::make_shared<fv::focus::FakeFocusPlatform>(notepad, false, false);
    auto service = capture_over(platform);
    DictationJob job;
    job.mode = JobMode::Command;
    job.target = fv::focus::FocusRequest::resolved(fv::focus::FocusSnapshot{notepad, std::nullopt});
    platform->set_identity_delay(PASTE_CHECK_TIMEOUT * 3);

    const auto started = std::chrono::steady_clock::now();
    CHECK(paste_target(&job, service.get()) == PasteTarget::Unknown);
    CHECK(std::chrono::steady_clock::now() - started < PASTE_CHECK_TIMEOUT * 2);
    service->shutdown();
}
