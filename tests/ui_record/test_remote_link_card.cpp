// The Remote backend on the record tabs' engine card: the link glyph and its readout, the host's
// Model and runtime fields, Manage host models, the host's serving row and the remote stats
// (tests/test_remote_link_ui.py, tests/test_upload_file_tab.py TestRemoteModelField,
// tests/test_live_preview_control.py's Remote cases).
#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QSignalSpy>
#include <QTimer>

#include "base/identity.h"
#include "settings/testing/memory_store.h"
#include "ui_record/file_info_card.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/stats_display.h"
#include "ui_record/upload_file_tab.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/remote_engine_controls.h"
#include "ui_widgets/remote_link_glyph.h"
#include "ui_widgets/remote_model_notice.h"
#include "ui_widgets/speech_server_controls.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::UploadFileTab;
using fv::remote::HostModel;
using fv::remote::RemoteLink;
using fv::remote::RemoteModels;
using fv::remote::RemoteTiming;

namespace {

const QString kRemote = QStringLiteral("Remote computer");

// The paired_with fixture: this computer is paired with "jed".
Json pairedWithJed() {
    return Json{{"remote_engine_client", Json{{"host", "192.168.1.40"},
                                              {"port", 47821},
                                              {"fingerprint", "ab:cd"},
                                              {"host_name", "jed"}}}};
}

// An Upload tab over settings in memory, shown, with Remote chosen (_remote_tab).
struct RemoteTab {
    explicit RemoteTab(const Json& saved = pairedWithJed(), bool chooseRemote = true)
        : settings(saved) {
        tab = std::make_unique<UploadFileTab>(settings, fv::speechEngineCatalog());
        tab->show();
        if (chooseRemote) {
            tab->chooseBackend(kRemote);
        }
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

RemoteLink connected() {
    RemoteLink link;
    link.state = "connected";
    link.host = "jed";
    link.route = "Tailscale";
    link.latency_ms = 24.4;
    link.engine_label = "Parakeet TDT 0.6B v3";
    link.device = "cuda";
    link.address = "100.101.102.103:47821";
    return link;
}

RemoteLink offline(std::string detail) {
    RemoteLink link;
    link.state = "offline";
    link.host = "jed";
    link.detail = std::move(detail);
    return link;
}

QStringList items(const QComboBox* combo) {
    QStringList out;
    for (int index = 0; index < combo->count(); ++index) {
        out << combo->itemText(index);
    }
    return out;
}

const HostModel kParakeet{"parakeet", "parakeet-v3", "Parakeet TDT 0.6B v3"};
const HostModel kNemotron{"nemotron", "nemotron-3.5", "Nemotron 3.5 ASR 0.6B"};
const HostModel kSmall{"local_whisper", "small", "Whisper small"};
const HostModel kTurbo{"local_whisper", "turbo", "Whisper turbo"};

RemoteModels choices(std::optional<std::vector<HostModel>> models,
                     std::optional<HostModel> current = std::nullopt) {
    RemoteModels state;
    state.host = "jed";
    state.models = std::move(models);
    state.current = std::move(current);
    return state;
}

// test_live_preview_control.py's _remote_models.
RemoteModels hostRunning(std::optional<std::string> family) {
    RemoteModels state;
    state.host = "jed";
    state.models = std::vector<HostModel>{};
    if (family) {
        state.current = HostModel{*family, "model", "Model"};
    }
    return state;
}

} // namespace

// ---- the engine card ----

// from tests/test_remote_link_ui.py::test_the_card_shows_the_link_instead_of_the_dot
TEST_CASE("test_the_card_shows_the_link_instead_of_the_dot", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    CHECK(tab->statusDot()->isVisible());
    CHECK_FALSE(tab->linkGlyph()->isVisible());
    tab->setRemoteLink(connected());
    CHECK(tab->linkGlyph()->isVisible());
    CHECK_FALSE(tab->statusDot()->isVisible());
    CHECK(tab->resolvedLabel()->text() == QStringLiteral(u"Connected to jed · Tailscale · 24 ms"));
    CHECK(tab->linkGlyph()->toolTip().contains(
        QStringLiteral("Parakeet TDT 0.6B v3 on CUDA, served by jed at 100.101.102.103:47821")));
    auto near = connected();
    near.route = "local network";
    near.latency_ms = 0.4;
    tab->setRemoteLink(near);
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral(u"Connected to jed · local network · <1 ms"));
    // Another backend: back to the plain dot.
    tab->chooseBackend(QStringLiteral("Parakeet"));
    QApplication::processEvents();
    CHECK_FALSE(tab->linkGlyph()->isVisible());
    CHECK(tab->statusDot()->isVisible());
}

// from tests/test_remote_link_ui.py::test_the_link_tooltip_names_the_model_once
TEST_CASE("test_the_link_tooltip_names_the_model_once", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    auto link = connected();
    link.runtime_status = "Parakeet TDT 0.6B v3 | cuda";
    tab->setRemoteLink(link);
    const QString tip =
        QStringLiteral("Parakeet TDT 0.6B v3 on CUDA, served by jed at 100.101.102.103:47821.");
    CHECK(tab->linkGlyph()->toolTip() == tip);
    CHECK(tab->remoteRuntimeLabel()->toolTip() == tip);
    // The host's fallback note is news; its restated model and device are not.
    link.engine_label = "Whisper turbo";
    link.device = "cpu";
    link.compute_type = "int8";
    link.runtime_status = "turbo | cpu (int8) — GPU unavailable, using CPU";
    tab->setRemoteLink(link);
    CHECK(tab->linkGlyph()->toolTip() ==
          QStringLiteral("Whisper turbo on CPU (int8), served by jed at "
                         "100.101.102.103:47821.\nGPU unavailable, using CPU"));
    link = connected();
    link.runtime_status = "Warming up the GPU";
    tab->setRemoteLink(link);
    CHECK(tab->linkGlyph()->toolTip().endsWith(QStringLiteral("\nWarming up the GPU")));
}

// from tests/test_remote_link_ui.py::test_the_card_counts_down_to_the_next_try
TEST_CASE("test_the_card_counts_down_to_the_next_try", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    const std::string detail = "Couldn't reach 192.168.1.40:47821. Check that the host is on.";
    auto link = offline(detail);
    link.retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(15200);
    tab->setRemoteLink(link);
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral("Couldn't reach 192.168.1.40:47821. Trying again in 15 s."));
    CHECK(tab->linkCountdown()->isActive());
    CHECK(tab->linkCountdown()->interval() == 1000);
    CHECK(tab->linkGlyph()->toolTip().contains(QString::fromStdString(detail)));
    RemoteLink connecting;
    connecting.state = "connecting";
    connecting.host = "jed";
    tab->setRemoteLink(connecting);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Connecting to jed..."));
    CHECK_FALSE(tab->linkCountdown()->isActive());
    tab->setRemoteLink(offline("jed stopped answering."));
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral("jed stopped answering. Click to try again."));
    CHECK(tab->linkGlyph()->toolTip() ==
          QStringLiteral("jed stopped answering.\nClick the link to try again now."));
}

TEST_CASE("the countdown ticks the readout down each second", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    auto link = offline("Couldn't reach jed.");
    link.retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(2400);
    tab->setRemoteLink(link);
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral("Couldn't reach jed. Trying again in 2 s."));
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 1500) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral("Couldn't reach jed. Trying again in 1 s."));
}

TEST_CASE("an unpaired link says how to pair", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    RemoteLink link;
    tab->setRemoteLink(link);
    CHECK(tab->resolvedLabel()->text() ==
          QStringLiteral(u"Pair with a host in Settings → Remote engine."));
    RemoteLink unreached = offline("");
    tab->setRemoteLink(unreached);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Can't reach jed. Click to try again."));
}

// from tests/test_remote_link_ui.py::test_clicking_an_offline_link_asks_to_reconnect
TEST_CASE("test_clicking_an_offline_link_asks_to_reconnect", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    QSignalSpy asked(tab.tab.get(), &fv::TranscriptionTabBase::remoteRetryRequested);
    tab->setRemoteLink(connected());
    Q_EMIT tab->linkGlyph()->clicked();
    CHECK(asked.isEmpty()); // connected: nothing to retry
    tab->setRemoteLink(offline("jed stopped answering."));
    Q_EMIT tab->linkGlyph()->clicked();
    CHECK(asked.count() == 1);
}

// from tests/test_remote_link_ui.py::test_transcribing_names_the_computer_doing_it
TEST_CASE("test_transcribing_names_the_computer_doing_it", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    tab->setRemoteLink(connected());
    tab->setActivityState(fv::appcore::OverlayState::Transcribing);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Transcribing on jed..."));
    tab->setActivityState(fv::appcore::OverlayState::None);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral(u"Connected to jed · Tailscale · 24 ms"));
}

TEST_CASE("the link beats a generic loading line but not a specific one",
          "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    RemoteLink connecting;
    connecting.state = "connecting";
    connecting.host = "jed";
    tab->setEngineBusy(true);
    tab->setRemoteLink(connecting);
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Connecting to jed..."));
    tab->setEngineMessage(QStringLiteral("Switching jed to Whisper small..."));
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Switching jed to Whisper small..."));
    tab->setEngineBusy(false);
    tab->setRemoteLink(connected());
    tab->setEngineMessage(QStringLiteral("Loading speech engine..."));
    CHECK(tab->resolvedLabel()->text() == QStringLiteral(u"Connected to jed · Tailscale · 24 ms"));
}

TEST_CASE("the runtime line names the host's hardware", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    CHECK_FALSE(tab->remoteRuntimeLabel()->isVisible());
    auto link = connected();
    link.compute_type = "float16";
    link.gpu_name = "RTX 4090";
    link.gpu_memory_mib = 24576;
    tab->setRemoteLink(link);
    CHECK(tab->remoteRuntimeLabel()->isVisible());
    CHECK(tab->remoteRuntimeLabel()->text() ==
          QStringLiteral(u"Running on jed: GPU · float16 · RTX 4090 · 24 GB VRAM"));
    link.device = "";
    link.compute_type = "";
    tab->setRemoteLink(link);
    CHECK(tab->remoteRuntimeLabel()->text() ==
          QStringLiteral("Running on jed: device not reported"));
    tab->setRemoteLink(offline("jed stopped answering."));
    CHECK_FALSE(tab->remoteRuntimeLabel()->isVisible());
}

// from tests/test_remote_link_ui.py::test_the_host_slides_in_who_it_is_serving
TEST_CASE("test_the_host_slides_in_who_it_is_serving", "[test_remote_link_ui][widget]") {
    RemoteTab tab(Json::object(), false);
    CHECK_FALSE(tab->servingRow()->isVisible());
    tab->setRemoteClients({{QStringLiteral("laptop"), false}});
    CHECK(tab->servingRow()->isVisible());
    CHECK(tab->servingLabel()->text() == QStringLiteral("Sharing this engine with laptop"));
    tab->setRemoteClients({{QStringLiteral("laptop"), true}});
    CHECK(tab->servingLabel()->text() == QStringLiteral("Transcribing for laptop"));
    CHECK(tab->servingGlyph()->busy());
    tab->setRemoteClients({{QStringLiteral("laptop"), false}});
    // The finished request flies home.
    CHECK(tab->servingGlyph()->flightKinds() == QStringList{QStringLiteral("reply")});
    tab->setRemoteClients({{QStringLiteral("laptop"), false}, {QStringLiteral("desk"), false}});
    CHECK(tab->servingLabel()->text() == QStringLiteral("Sharing this engine with 2 computers"));
    CHECK(tab->servingRow()->toolTip() == QStringLiteral("laptop is connected\ndesk is connected"));
    tab->setRemoteClients({});
    QElapsedTimer waited;
    waited.start();
    while (tab->servingRow()->isVisible() && waited.elapsed() < 3000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    CHECK_FALSE(tab->servingRow()->isVisible());
}

// ---- the stats line ----

// from tests/test_remote_link_ui.py::test_remote_timing_text
TEST_CASE("test_remote_timing_text", "[test_remote_link_ui]") {
    CHECK(fv::remoteTimingText(std::nullopt).isEmpty());
    CHECK(fv::remoteTimingText(RemoteTiming{"jed", 2, 0.5, 0.42}) ==
          QStringLiteral(u"on jed · 80 ms network"));
    CHECK(fv::remoteTimingText(RemoteTiming{"jed", 1, 0.5, std::nullopt}) ==
          QStringLiteral("on jed"));
    // Nothing was sent (every window silent): no network to speak of.
    CHECK(fv::remoteTimingText(RemoteTiming{"jed", 0, 0.0, std::nullopt}) ==
          QStringLiteral("on jed"));
}

// from tests/test_remote_link_ui.py::test_the_stats_line_names_the_host
TEST_CASE("test_the_stats_line_names_the_host", "[test_remote_link_ui][widget]") {
    fv::TranscriptionStatsWidget stats;
    stats.setStats(0.42, 8.8, 1024, 0.0, RemoteTiming{"jed", 1, 0.35, 0.31});
    QLabel* detail = stats.remoteDetailLabel();
    CHECK(detail->objectName() == QStringLiteral("statsRemoteDetail"));
    CHECK(detail->text() == QStringLiteral(u"on jed · 40 ms network"));
    CHECK_FALSE(detail->isHidden());
    stats.setStats(0.42, 8.8, 1024);
    CHECK(detail->isHidden());
}

// from tests/test_remote_link_ui.py::test_the_upload_card_names_the_host
TEST_CASE("test_the_upload_card_names_the_host", "[test_remote_link_ui][widget]") {
    RemoteTab tab;
    tab->setTranscriptionStats(4.2, 73.3, 1024, 0.0, RemoteTiming{"jed", 3, 3.9, 3.72});
    const QString text = tab->fileInfoCard()->resultLabel()->text();
    CHECK(text.contains(QStringLiteral("on jed")));
    CHECK(text.contains(QStringLiteral("180 ms")));
    CHECK(text.contains(QStringLiteral("network")));
}

// ---- the Model field (tests/test_upload_file_tab.py TestRemoteModelField) ----

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_lists_the_hosts_models_by_engine_with_the_running_one_selected
TEST_CASE("TestRemoteModelField lists the host's models by engine with the running one selected",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    CHECK(tab->remoteModelField()->isVisible());
    CHECK_FALSE(tab->localEngine()->isVisible());
    if (tab->apiServer() != nullptr) {
        CHECK_FALSE(tab->apiServer()->isVisible());
    }
    // Unlike the API, a remote engine has a connection for the dot to report.
    CHECK(tab->statusDot()->isVisible());

    QComboBox* combo = tab->remoteModelCombo();
    tab->setRemoteModels(choices(std::vector{kParakeet, kNemotron, kSmall, kTurbo}, kSmall));
    // One separator between engines; the two Whisper sizes stay together.
    CHECK(items(combo) == QStringList{QStringLiteral("Parakeet TDT 0.6B v3"), QString(),
                                      QStringLiteral("Nemotron 3.5 ASR 0.6B"), QString(),
                                      QStringLiteral("Whisper small"),
                                      QStringLiteral("Whisper turbo")});
    CHECK(combo->currentText() == QStringLiteral("Whisper small"));
    CHECK(combo->isEnabled());
    CHECK(combo->toolTip().contains(QStringLiteral("jed")));
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_choosing_a_model_asks_to_switch_the_host
TEST_CASE("TestRemoteModelField choosing a model asks to switch the host",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    tab->setRemoteModels(choices(std::vector{kParakeet, kNemotron}, kParakeet));
    QSignalSpy chosen(tab.tab.get(), &fv::TranscriptionTabBase::remoteModelSelected);
    QComboBox* combo = tab->remoteModelCombo();
    Q_EMIT combo->activated(combo->findText(QStringLiteral("Parakeet TDT 0.6B v3")));
    CHECK(chosen.isEmpty()); // the model it already runs is no switch
    Q_EMIT combo->activated(combo->findText(QStringLiteral("Nemotron 3.5 ASR 0.6B")));
    REQUIRE(chosen.count() == 1);
    CHECK(chosen[0][0].toString() == QStringLiteral("nemotron"));
    CHECK(chosen[0][1].toString() == QStringLiteral("nemotron-3.5"));
    // Following what the host reports never asks for a switch.
    tab->setRemoteModels(choices(std::vector{kParakeet, kNemotron}, kNemotron));
    CHECK(chosen.count() == 1);
    CHECK(combo->currentText() == QStringLiteral("Nemotron 3.5 ASR 0.6B"));
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_locked_while_the_engine_reloads_or_a_recording_runs
TEST_CASE("TestRemoteModelField locked while the engine reloads or a recording runs",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    tab->setRemoteModels(choices(std::vector{kParakeet, kNemotron}, kParakeet));
    QComboBox* combo = tab->remoteModelCombo();
    tab->setEngineBusy(true);
    CHECK_FALSE(combo->isEnabled());
    tab->setEngineBusy(false);
    CHECK(combo->isEnabled());
    tab->setBackendEnabled(false);
    CHECK_FALSE(combo->isEnabled());
    tab->setBackendEnabled(true);
    CHECK(combo->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_a_host_serving_nothing_usable_offers_its_models
TEST_CASE("TestRemoteModelField a host serving nothing usable offers its models",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    tab->setRemoteModels(choices(std::vector{kParakeet, kSmall}));
    QComboBox* combo = tab->remoteModelCombo();
    CHECK(combo->currentText() == QStringLiteral("Choose a model"));
    CHECK(combo->isEnabled());
    QSignalSpy chosen(tab.tab.get(), &fv::TranscriptionTabBase::remoteModelSelected);
    Q_EMIT combo->activated(0);
    CHECK(chosen.isEmpty()); // the prompt is no model
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_nothing_to_choose_between_shows_why
TEST_CASE("TestRemoteModelField nothing to choose between shows why",
          "[test_upload_file_tab][widget]") {
    struct Case {
        RemoteModels state;
        QString text;
        QString tip;
    };
    const QString product = QString::fromUtf8(fv::identity().product);
    const std::vector<Case> cases{
        {choices(std::nullopt), QStringLiteral("Not connected"),
         QStringLiteral("once this computer connects")},
        {choices(std::vector<HostModel>{}), QStringLiteral("No models ready"),
         QStringLiteral("Open Manage host models")},
        {choices(std::nullopt, kParakeet), QStringLiteral("Parakeet TDT 0.6B v3"),
         QStringLiteral("Update %1 there").arg(product)},
        {choices(std::vector{kParakeet}, kParakeet), QStringLiteral("Parakeet TDT 0.6B v3"),
         QStringLiteral("only downloaded model")},
    };
    for (const Case& item : cases) {
        RemoteTab tab;
        tab->setRemoteModels(item.state);
        QComboBox* combo = tab->remoteModelCombo();
        CHECK(items(combo) == QStringList{item.text});
        CHECK_FALSE(combo->isEnabled());
        CHECK(combo->toolTip().contains(item.tip));
    }
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_before_the_first_report_it_names_the_pairing
TEST_CASE("TestRemoteModelField before the first report it names the pairing",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    QComboBox* combo = tab->remoteModelCombo();
    CHECK(items(combo) == QStringList{QStringLiteral("Not connected")});
    CHECK(combo->toolTip() ==
          QStringLiteral("jed's models show here once this computer connects."));
    tab->setEngineBusy(true);
    CHECK(items(combo) == QStringList{QStringLiteral("Connecting...")});
    tab->setEngineBusy(false);
    tab.settings.update(Json::object(), {"remote_engine_client"});
    tab->chooseBackend(QStringLiteral("API"));
    tab->chooseBackend(kRemote);
    CHECK(items(combo) == QStringList{QStringLiteral("Not paired")});

    tab->chooseBackend(QStringLiteral("API"));
    QApplication::processEvents();
    if (tab->apiServer() != nullptr) {
        CHECK(tab->apiServer()->isVisible());
    }
    CHECK_FALSE(tab->remoteModelField()->isVisible());
}

// from
// tests/test_upload_file_tab.py::TestRemoteModelField::test_card_height_is_the_same_as_for_a_local_backend
TEST_CASE("TestRemoteModelField card height is the same as for a local backend",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab(pairedWithJed(), false);
    tab->resize(605, 580);
    QApplication::processEvents();
    const int localHeight = tab->engineCard()->height();
    tab->chooseBackend(kRemote);
    auto state = choices(std::vector{kParakeet, kNemotron}, kParakeet);
    tab->setRemoteModels(state);
    QApplication::processEvents();
    CHECK(tab->remoteManageButton()->isVisible());
    CHECK(tab->engineCard()->height() == localHeight);
    // Something to install on the host is a dot on the footer's link, not a row.
    state.runtime = Json{{"dependencies", Json::array({Json{{"label", "GPU Acceleration"},
                                                            {"device", "cuda"},
                                                            {"installable", true}}})}};
    tab->setRemoteModels(state);
    QApplication::processEvents();
    CHECK(tab->remoteManageButton()->available() ==
          QStringList{QStringLiteral("GPU Acceleration")});
    CHECK(tab->engineCard()->height() == localHeight);
}

TEST_CASE("the Model field's help opens Settings > Remote engine",
          "[test_upload_file_tab][widget]") {
    CHECK(fv::kRemoteModelHelp ==
          QStringLiteral("The model the paired computer transcribes with. Choosing another "
                         "switches that computer to it, as if it were chosen there. Only models "
                         "already downloaded there are listed."));
    RemoteTab tab;
    QSignalSpy help(tab.tab.get(), &fv::TranscriptionTabBase::helpRequested);
    Q_EMIT tab->remoteManageButton()->clicked();
    REQUIRE(help.count() == 1);
    CHECK(help[0][0].toString() == QStringLiteral("remote_models"));
}

TEST_CASE("the backend lock and a dropped link lock the host's runtime fields",
          "[test_upload_file_tab][widget]") {
    RemoteTab tab;
    RemoteModels state = choices(std::vector{kParakeet}, kParakeet);
    state.engine = Json{{"family", "parakeet"}, {"device", "cuda"}};
    state.runtime = Json{{"family", "parakeet"},
                         {"model", "parakeet-v3"},
                         {"devices", Json::array({"cpu", "cuda"})},
                         {"selected", Json{{"device", "cuda"}}},
                         {"can_configure", true}};
    tab->setRemoteModels(state);
    tab->setRemoteLink(connected());
    QComboBox* device = tab->remoteEngine()->deviceCombo();
    CHECK(device->isEnabled());
    tab->setBackendEnabled(false);
    CHECK_FALSE(device->isEnabled());
    tab->setBackendEnabled(true);
    tab->setRemoteLink(offline("jed stopped answering."));
    CHECK_FALSE(device->isEnabled());
    tab->setRemoteLink(connected());
    QSignalSpy runtime(tab.tab.get(), &fv::TranscriptionTabBase::remoteRuntimeSelected);
    Q_EMIT device->activated(device->findData(QStringLiteral("cpu")));
    REQUIRE(runtime.count() == 1);
    CHECK(runtime[0][2].toMap() == QVariantMap{{QStringLiteral("device"), QStringLiteral("cpu")}});
}

// ---- Live preview on the paired computer (tests/test_live_preview_control.py) ----

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_remote_host_without_a_preview_engine_names_the_host
TEST_CASE("remote host without a preview engine names the host",
          "[test_live_preview_control][widget]") {
    fv::MemorySettingsStore settings(Json{{"streaming_enabled", true}});
    fv::QuickRecordTab tab(settings, fv::speechEngineCatalog());
    tab.setBackend(kRemote);
    tab.setRemoteModels(hostRunning("local_whisper"));
    QCheckBox* check = tab.livePreviewCheck();
    CHECK_FALSE(check->isEnabled());
    CHECK_FALSE(check->isChecked());
    // Lead decision (src/speech/preview_status.h): the copy names Parakeet only.
    CHECK(check->toolTip().startsWith(QStringLiteral("Live preview needs Parakeet on jed.")));
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_remote_host_switching_to_parakeet_turns_it_back_on
TEST_CASE("remote host switching to parakeet turns it back on",
          "[test_live_preview_control][widget]") {
    fv::MemorySettingsStore settings(Json{{"streaming_enabled", true}});
    fv::QuickRecordTab tab(settings, fv::speechEngineCatalog());
    tab.setBackend(kRemote);
    tab.setRemoteModels(hostRunning("local_whisper"));
    tab.setRemoteModels(hostRunning("parakeet"));
    CHECK(tab.livePreviewCheck()->isEnabled());
    CHECK(tab.livePreviewCheck()->isChecked());
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_remote_not_connected_yet_keeps_the_choice
TEST_CASE("remote not connected yet keeps the choice", "[test_live_preview_control][widget]") {
    // The host decides once it connects; graying out until then would flicker on every start.
    fv::MemorySettingsStore settings(Json{{"streaming_enabled", true}});
    fv::QuickRecordTab tab(settings, fv::speechEngineCatalog());
    tab.setBackend(kRemote);
    tab.setRemoteModels(hostRunning(std::nullopt));
    CHECK(tab.livePreviewCheck()->isEnabled());
    CHECK(tab.livePreviewCheck()->isChecked());
}
