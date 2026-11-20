// The Remote backend's runtime fields on the record tabs' engine card, filled only from what the
// paired host reports (tests/test_remote_runtime.py's card cases: RemoteEngineControls inside
// UploadFileTab, ui_qt/widgets/remote_engine_controls.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QSignalSpy>

#include "base/identity.h"
#include "settings/testing/memory_store.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/upload_file_tab.h"
#include "ui_widgets/remote_engine_controls.h"
#include "ui_widgets/remote_model_notice.h"
#include "ui_widgets/wrapped_label.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::RemoteEngineControls;
using fv::UploadFileTab;
using fv::remote::HostModel;
using fv::remote::RemoteLink;
using fv::remote::RemoteModels;

namespace {

const QString kRemote = QStringLiteral("Remote computer");

Json pairedWithDevbox() {
    return Json{{"remote_engine_client", Json{{"host", "192.168.1.40"},
                                              {"port", 47821},
                                              {"fingerprint", "ab:cd"},
                                              {"host_name", "devbox"}}}};
}

// tests/test_remote_runtime.py RuntimeEngine.describe(): an OpenWhisper host running Whisper on
// its CPU (FioriVoce's client pairs with such hosts; its own hosts run Parakeet).
Json whisperEngine() {
    return Json{{"family", "local_whisper"}, {"model", "turbo"},       {"label", "Whisper turbo"},
                {"device", "cpu"},           {"compute_type", "int8"}, {"available", true}};
}

// _choices(engine): the host's runtime_state with can_configure.
RemoteModels choicesFor(const Json& engine, Json runtime) {
    const HostModel current{engine["family"].get<std::string>(), engine["model"].get<std::string>(),
                            engine["label"].get<std::string>()};
    runtime["can_configure"] = true;
    RemoteModels state;
    state.host = "devbox";
    state.models = std::vector<HostModel>{current};
    state.current = current;
    state.runtime = std::move(runtime);
    state.engine = engine;
    return state;
}

// runtime_state for the Whisper host: CPU and an installed CUDA runtime on a GTX 1050 Ti.
Json whisperRuntime() {
    return Json{{"family", "local_whisper"},
                {"model", "turbo"},
                {"selected", Json{{"device", "cpu"}, {"compute_type", "int8"}}},
                {"devices", Json::array({"auto", "cpu", "cuda"})},
                {"compute_types", Json{{"cpu", Json::array({"int8", "float32"})},
                                       {"cuda", Json::array({"int8", "float32", "int8_float32"})}}},
                {"gpu", Json{{"name", "NVIDIA GTX 1050 Ti"}, {"total_mib", 4096}}}};
}

struct RemoteTab {
    RemoteTab() : settings(pairedWithDevbox()) {
        tab = std::make_unique<UploadFileTab>(settings, fv::speechEngineCatalog());
        tab->chooseBackend(kRemote);
        tab->show();
        QApplication::processEvents();
    }
    ~RemoteTab() {
        tab.reset();
        QApplication::processEvents();
    }
    UploadFileTab* operator->() const { return tab.get(); }

    fv::MemorySettingsStore settings;
    std::unique_ptr<UploadFileTab> tab;
};

} // namespace

// from tests/test_remote_runtime.py::test_card_uses_host_choices_and_locks_during_activity
TEST_CASE("test_card_uses_host_choices_and_locks_during_activity",
          "[test_remote_runtime][widget]") {
    RemoteTab tab;
    tab->setRemoteModels(choicesFor(whisperEngine(), whisperRuntime()));
    RemoteLink link;
    link.state = "connected";
    link.host = "devbox";
    link.device = "cuda";
    link.compute_type = "int8_float32";
    link.gpu_name = "GTX 1050 Ti";
    link.gpu_memory_mib = 4096;
    tab->setRemoteLink(link);
    QApplication::processEvents();
    RemoteEngineControls* controls = tab->remoteEngine();
    QComboBox* device = controls->deviceCombo();
    CHECK(controls->isVisible());
    CHECK(device->isEnabled());
    CHECK(controls->computeCombo()->isVisible());
    CHECK_FALSE(controls->languageCombo()->isVisible());
    CHECK_THAT(tab->remoteRuntimeLabel()->text().toStdString(), ContainsSubstring("4 GB VRAM"));
    CHECK_THAT(tab->remoteRuntimeLabel()->text().toStdString(),
               ContainsSubstring("Running on devbox: GPU"));

    const Json before = *tab.settings.snapshot();
    QSignalSpy emitted(tab.tab.get(), &fv::TranscriptionTabBase::remoteRuntimeSelected);
    Q_EMIT device->activated(device->findData(QStringLiteral("cuda")));
    REQUIRE(emitted.count() == 1);
    CHECK(emitted[0][0].toString() == QStringLiteral("local_whisper"));
    CHECK(emitted[0][1].toString() == QStringLiteral("turbo"));
    CHECK(emitted[0][2].toMap() == QVariantMap{{QStringLiteral("device"), QStringLiteral("cuda")}});
    CHECK_FALSE(device->isEnabled());
    // The UI must not save the host's settings here.
    CHECK(*tab.settings.snapshot() == before);
    tab->setRemoteModels(choicesFor(whisperEngine(), whisperRuntime()));
    CHECK(emitted.count() == 1);
    CHECK(device->isEnabled());

    tab->setBackendEnabled(false);
    CHECK_FALSE(device->isEnabled());
    tab->setBackendEnabled(true);
    CHECK(device->isEnabled());
    tab->setEngineBusy(true);
    CHECK_FALSE(device->isEnabled());
    tab->setEngineBusy(false);
    CHECK(device->isEnabled());

    RemoteLink gone;
    gone.state = "offline";
    gone.host = "devbox";
    tab->setRemoteLink(gone);
    CHECK_FALSE(device->isEnabled());
    CHECK(tab->remoteRuntimeLabel()->isHidden());
    tab->chooseBackend(QStringLiteral("Parakeet"));
    QApplication::processEvents();
    CHECK_FALSE(controls->isVisible());
}

// from tests/test_remote_runtime.py::test_legacy_runtime_fields_are_read_only
TEST_CASE("test_legacy_runtime_fields_are_read_only", "[test_remote_runtime][widget]") {
    RemoteEngineControls widget;
    RemoteModels state;
    state.host = "old-host";
    state.engine = whisperEngine();
    widget.setState(state);
    CHECK(widget.deviceCombo()->currentData().toString() == QStringLiteral("cpu"));
    CHECK(widget.computeCombo()->currentData().toString() == QStringLiteral("int8"));
    CHECK_FALSE(widget.deviceCombo()->isEnabled());
    CHECK_THAT(widget.deviceCombo()->toolTip().toStdString(),
               ContainsSubstring("Update " + std::string(fv::identity().product)));
}

// from tests/test_remote_runtime.py::test_missing_gpu_runtime_has_setup_route_in_main_card:
// FioriVoce's host reports the GPU runtime it could install as an installable dependency
// (remote_host::runtime_state), so the card offers its setup without asking this computer.
TEST_CASE("test_missing_gpu_runtime_has_setup_route_in_main_card",
          "[test_remote_runtime][widget]") {
    RemoteTab tab;
    const Json engine{{"family", "parakeet"},
                      {"model", "parakeet-v3"},
                      {"label", "Parakeet TDT 0.6B v3"},
                      {"device", "cpu"},
                      {"available", true}};
    const Json runtime{{"family", "parakeet"},
                       {"model", "parakeet-v3"},
                       {"selected", Json{{"device", "cpu"}, {"language", "auto"}}},
                       {"devices", Json::array({"auto", "cpu"})},
                       {"compute_types", Json::object()},
                       {"languages", Json::array({"en", "auto"})},
                       {"dependencies", Json::array({Json{{"device", "cpu"},
                                                          {"label", "CPU runtime"},
                                                          {"ready", true},
                                                          {"installable", false}},
                                                     Json{{"device", "cuda"},
                                                          {"label", "GPU Acceleration"},
                                                          {"ready", false},
                                                          {"installable", true}}})}};
    tab->setRemoteModels(choicesFor(engine, runtime));
    RemoteLink link;
    link.state = "connected";
    link.host = "devbox";
    link.device = "cpu";
    tab->setRemoteLink(link);
    QApplication::processEvents();
    QSignalSpy destinations(tab.tab.get(), &fv::TranscriptionTabBase::helpRequested);
    QSignalSpy changes(tab.tab.get(), &fv::TranscriptionTabBase::remoteRuntimeSelected);
    fv::RemoteModelNotice* manage = tab->remoteManageButton();
    CHECK(manage->isVisible());
    CHECK(manage->available() == QStringList{QStringLiteral("GPU Acceleration")});
    CHECK_THAT(manage->toolTip().toStdString(), ContainsSubstring("GPU Acceleration"));
    manage->click();
    REQUIRE(destinations.count() == 1);
    CHECK(destinations[0][0].toString() == QStringLiteral("remote_models"));
    QComboBox* combo = tab->remoteEngine()->deviceCombo();
    const int index = combo->findData(QStringLiteral("setup:cuda"));
    REQUIRE(index >= 0);
    CHECK(combo->isEnabled());
    Q_EMIT combo->activated(index);
    REQUIRE(destinations.count() == 2);
    CHECK(destinations[1][0].toString() == QStringLiteral("remote_models"));
    CHECK(changes.count() == 0);
}

// from tests/test_remote_runtime.py::test_optional_card_shows_language_and_only_host_devices: a
// non-Whisper engine shows its language rather than a precision, and a host with one device and
// one language leaves nothing to choose. (OpenWhisper's case is Moonshine; FioriVoce's is
// Parakeet, the only other family it knows.)
TEST_CASE("test_optional_card_shows_language_and_only_host_devices",
          "[test_remote_runtime][widget]") {
    const Json engine{{"family", "parakeet"},
                      {"model", "parakeet-v3"},
                      {"label", "Parakeet TDT 0.6B v3"},
                      {"device", "cpu"}};
    const Json runtime{{"family", "parakeet"},
                       {"model", "parakeet-v3"},
                       {"selected", Json{{"device", "cpu"}, {"language", "en"}}},
                       {"devices", Json::array({"cpu"})},
                       {"compute_types", Json::object()},
                       {"languages", Json::array({"en"})}};
    RemoteEngineControls widget;
    widget.setState(choicesFor(engine, runtime));
    widget.show();
    QApplication::processEvents();
    CHECK_FALSE(widget.computeCombo()->isVisible());
    CHECK(widget.languageCombo()->isVisible());
    CHECK(widget.languageCombo()->currentText() == QStringLiteral("English"));
    CHECK_FALSE(widget.languageCombo()->isEnabled());
    CHECK_FALSE(widget.deviceCombo()->isEnabled());
    CHECK(widget.deviceCombo()->count() == 1);
}
