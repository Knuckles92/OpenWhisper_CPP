// The Paste walkthrough shortcut as the keyboard hook sees it (tests/test_walkthrough_paste.py's
// dispatch cases): ordinary Ctrl+V passes by, a virtual-key shortcut takes its modifiers from the
// OS, and modifiers the hook missed are still waited for.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <set>
#include <string>

#include "dictation_support.h"
#include "focus/capture_service.h"
#include "focus/synthetic_keys.h"
#include "focus/testing/fake_platform.h"
#include "hotkeys/key_names.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;

namespace {

struct ScopedCaptureService {
    explicit ScopedCaptureService(std::shared_ptr<fv::focus::CaptureService> service)
        : previous(fv::focus::get_service()) {
        fv::focus::set_service(std::move(service));
    }
    ~ScopedCaptureService() { fv::focus::set_service(previous); }
    std::shared_ptr<fv::focus::CaptureService> previous;
};

} // namespace

// from tests/test_walkthrough_paste.py::test_shortcut_dispatch_leaves_normal_ctrl_v_untouched
TEST_CASE("shortcut dispatch leaves normal ctrl v untouched", "[dictation][hotkey_runtime]") {
    ScopedCaptureService installed(
        std::make_shared<fv::focus::FakeCaptureService>(fv::focus::FocusSnapshot{}));
    Rig rig({{"paste_walkthrough", "ctrl+shift+f12"}});
    rig.manager->set_spawner([](std::function<void()> call) { call(); });
    Emitted<std::optional<fv::focus::AppIdentity>> pasted(rig.runtime->paste_walkthrough_requested,
                                                          rig.inline_ui);
    rig.runtime->setup_hotkeys();

    // The app gets its own Ctrl+V.
    CHECK(rig.hook->send(key_event(true, "v", false, fv::hotkeys::kCtrl)));
    CHECK(rig.hook->send(key_event(false, "v", false, fv::hotkeys::kCtrl)));
    CHECK(pasted.values().empty());

    const std::uint8_t held = fv::hotkeys::kCtrl | fv::hotkeys::kShift;
    CHECK_FALSE(rig.hook->send(key_event(true, "f12", false, held)));
    CHECK_FALSE(rig.hook->send(key_event(false, "f12", false, held)));
    CHECK(pasted.values().size() == 1);
}

// from tests/test_walkthrough_paste.py::test_virtual_key_shortcut_uses_os_modifiers
TEST_CASE("a virtual key shortcut uses the os modifiers", "[dictation][hotkey_runtime]") {
    ScopedCaptureService installed(
        std::make_shared<fv::focus::FakeCaptureService>(fv::focus::FocusSnapshot{}));
    Rig rig({{"paste_walkthrough", "ctrl+shift+f12"}});
    rig.manager->set_spawner([](std::function<void()> call) { call(); });
    Emitted<std::optional<fv::focus::AppIdentity>> pasted(rig.runtime->paste_walkthrough_requested,
                                                          rig.inline_ui);
    rig.runtime->setup_hotkeys();

    // An accessibility tool's virtual keys: no scan code, so the hook's record holds no
    // modifier; the OS says Ctrl and Shift are down.
    auto event = key_event(true, "f12", false, 0);
    event.scan_code = -123;
    event.os_modifiers = fv::hotkeys::kCtrl | fv::hotkeys::kShift;

    CHECK_FALSE(rig.hook->send(event));
    CHECK(pasted.values().size() == 1);
}

// from
// tests/test_walkthrough_paste.py::test_virtual_modifiers_are_waited_for_even_when_the_hook_misses_them
TEST_CASE("virtual modifiers are waited for even when the hook misses them",
          "[dictation][hotkey_runtime]") {
    const std::set<std::string> none;
    // Only VK_CONTROL is down, as the OS reads it; the hook saw no modifier.
    CHECK(fv::focus::windows_modifiers_held(
        [](std::uint16_t vk) { return vk == 0x11; },
        [&none](std::string_view name) { return none.contains(std::string(name)); }));
}
