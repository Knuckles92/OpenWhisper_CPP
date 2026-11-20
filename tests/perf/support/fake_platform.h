#pragma once

// The controller's platform over fakes, for a measuring test that runs the real Bootstrap (which
// builds its own context, so tests/ui_controller's ControllerRig, which builds one too, cannot
// be used). The same stand-ins as ControllerRig::platform(): a fake keyboard and mouse hook,
// fake synthetic keys, a capture service naming one app, a recorder on the fake audio system
// with a 50 ms post-roll, an installed engine resolved under the data folder, no adoption,
// recovery or crash, a development build with no update source, and every dialog, browser,
// process start and exit recorded instead of done. Nothing reaches the desktop.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "../../ui_controller/controller_support.h"

namespace fvperf {

struct FakePlatformState {
    fvtest::CallLog calls;
    fv::MapEnvReader noKeys;
    fv::hotkeys::FakeKeyboardHook* hook = nullptr;
    fv::focus::FakeSyntheticKeys* keys = nullptr;
};

inline fv::ControllerPlatform fakePlatform(FakePlatformState& state) {
    fv::ControllerPlatform made;
    made.keyboardHook = [&state] {
        auto hook = std::make_unique<fv::hotkeys::FakeKeyboardHook>();
        state.hook = hook.get();
        return hook;
    };
    made.mouseHook = []() -> std::unique_ptr<fv::hotkeys::MouseHookSource> {
        return std::make_unique<fv::hotkeys::FakeMouseHook>();
    };
    made.modelServices = [](fv::ModelServiceOptions& services) {
        fv::models::Host host;
        host.platform = std::string(fv::models::platform::kWinAmd64);
        services.store.host = host;
        services.store.free_bytes = [](const std::filesystem::path&) {
            return std::uint64_t{1} << 40;
        };
    };
    made.applyTheme = [&state](const QString& preference) {
        state.calls.add("apply_theme:" + preference.toStdString());
        return true;
    };
    made.applyFontScale = [&state](int percent) {
        state.calls.add("apply_font_scale:" + std::to_string(percent));
    };
    made.captureService = [](const fv::SettingsStore&) {
        return std::make_shared<fvtest::NamedAppCapture>("notepad.exe", "Notepad");
    };
    made.syntheticKeys = [&state] {
        auto keys = std::make_shared<fv::focus::FakeSyntheticKeys>();
        state.keys = keys.get();
        return keys;
    };
    made.recorder = [](fv::SettingsStore&, fv::audio::Backend& backend, const fv::Paths& paths,
                       fv::Executor&) {
        fv::RecorderOptions options;
        options.timings.post_roll = std::chrono::milliseconds(50);
        options.timings.first_block_grace = std::chrono::minutes(5);
        options.timings.stall_after_blocks = std::chrono::minutes(5);
        return std::make_unique<fv::Recorder>(backend, paths, std::move(options));
    };
    made.resolveEngine = [](const fv::Json&, const fv::Paths& paths) {
        const std::filesystem::path root = paths.data_root();
        fv::models::ResolvedEngine engine;
        engine.model_key = "parakeet-v3";
        engine.model_label = "Parakeet TDT 0.6B v3";
        engine.device = "cpu";
        engine.requested_device = "auto";
        engine.runtime_id = "asr-nvidia-cpu";
        engine.runtime_dir = root / "components" / "asr-nvidia-cpu";
        engine.model_path = root / "speech-models" / "parakeet-v3" / "model.gguf";
        return engine;
    };
    made.adoptMissing = [](const fv::Json&, const fv::Paths&, const fv::CancelToken&) {
        return fv::models::AdoptionReport{};
    };
    made.recoverComponents = [](const fv::Paths&, fv::http::Client*) {
    };
    made.lastCrash = [](const fv::Paths&) {
        return std::optional<fv::log::CrashInfo>();
    };
    made.offerCpuRuntime = [](QWidget*) {
        return false;
    };
    made.ensureEngine = [](QWidget*) {
        return true;
    };
    made.acquireComponent = [&state](std::string_view id) {
        state.calls.add("acquire:" + std::string(id));
    };
    made.installProbe = [] {
        fv::update::InstallProbe probe;
        probe.running_dir =
            fv::Paths::resolve(fv::identity(), fv::system_env()).data_root() / "bin";
        return probe;
    };
    made.updateFeed = [] {
        return fv::update::FeedConfig{};
    };
    made.startDetached =
        [&state](const std::filesystem::path& program, std::span<const std::string>,
                 const std::filesystem::path&) -> std::expected<std::uint32_t, std::string> {
        state.calls.add("start_detached:" + program.filename().string());
        return 4242;
    };
    made.exitApp = [&state](int code) {
        state.calls.add("exit:" + std::to_string(code));
    };
    made.armWatchdog = [] {
    };
    made.openUrl = [&state](const QString& url) {
        state.calls.add("open_url:" + url.toStdString());
    };
    made.question = [&state](QWidget*, const QString& title, const QString&) {
        state.calls.add("question:" + title.toStdString());
        return false;
    };
    made.warning = [&state](QWidget*, const QString& title, const QString&) {
        state.calls.add("warning:" + title.toStdString());
    };
    made.codeChanged = [] {
        return false;
    };
    made.execRestartOffer = [&state](QMessageBox&) {
        state.calls.add("restart_offer");
    };
    made.keyEnvironment = &state.noKeys;
    return made;
}

} // namespace fvperf
