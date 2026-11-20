// The Remote engine's link glyph, the "Manage host models" link and the paired host's runtime
// fields (ui_qt/widgets/remote_link.py, remote_model_notice.py, remote_engine_controls.py).
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QStandardItemModel>

#include "base/identity.h"
#include "ui_widgets/remote_engine_controls.h"
#include "ui_widgets/remote_link_glyph.h"
#include "ui_widgets/remote_model_notice.h"

using fv::Json;
using fv::RemoteEngineControls;
using fv::RemoteLinkGlyph;
using fv::RemoteModelNotice;

namespace {

QStringList items(const QComboBox* combo) {
    QStringList out;
    for (int index = 0; index < combo->count(); ++index) {
        out << combo->itemText(index);
    }
    return out;
}

// A host serving Parakeet that lets this computer change its runtime.
fv::remote::RemoteModels parakeetHost(bool canConfigure = true) {
    fv::remote::RemoteModels state;
    state.host = "jed";
    state.current = fv::remote::HostModel{"parakeet", "parakeet-v3", "Parakeet TDT 0.6B v3"};
    state.engine = Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cuda"}};
    state.runtime = Json{{"family", "parakeet"},
                         {"model", "parakeet-v3"},
                         {"devices", Json::array({"cpu", "cuda"})},
                         {"languages", Json::array({"en", "fr", "auto"})},
                         {"selected", Json{{"device", "cuda"}, {"language", "en"}}},
                         {"can_configure", canConfigure}};
    return state;
}

} // namespace

// ---- the glyph ----

// from tests/test_remote_link_ui.py::test_the_glyph_only_animates_on_real_events
TEST_CASE("test_the_glyph_only_animates_on_real_events", "[test_remote_link_ui][widget]") {
    RemoteLinkGlyph glyph;
    std::int64_t now = 1000;
    glyph.setClock([&now] { return now; });
    glyph.show();
    glyph.setLink(QStringLiteral("offline"));
    CHECK_FALSE(glyph.isAnimating()); // a broken wire doesn't move
    glyph.setLink(QStringLiteral("connecting"));
    CHECK(glyph.isAnimating());
    glyph.setLink(QStringLiteral("connected"), false, 0, 1);
    // The first answer draws the wire across, once.
    CHECK(glyph.isDrawingWire());
    now += RemoteLinkGlyph::kDrawMs + RemoteLinkGlyph::kLightMs;
    glyph.tick();
    CHECK_FALSE(glyph.isDrawingWire());
    CHECK_FALSE(glyph.isAnimating()); // connected and idle: still
    glyph.setLink(QStringLiteral("connected"), false, 0, 2);
    CHECK(glyph.flightKinds() == QStringList{QStringLiteral("beat")});
    now += RemoteLinkGlyph::kHeartbeatMs;
    glyph.tick();
    CHECK(glyph.flightKinds().isEmpty());
    glyph.setLink(QStringLiteral("connected"), true, 0, 2);
    CHECK(glyph.isAnimating());
    CHECK(glyph.busy());
    glyph.setLink(QStringLiteral("connected"), false, 1, 2);
    CHECK(glyph.flightKinds() == QStringList{QStringLiteral("reply")});
    glyph.hide();
    CHECK_FALSE(glyph.isAnimating());
}

// from tests/test_remote_link_ui.py::test_the_glyph_greets_a_host_only_the_first_time
TEST_CASE("test_the_glyph_greets_a_host_only_the_first_time", "[test_remote_link_ui][widget]") {
    RemoteLinkGlyph glyph;
    std::int64_t now = 0;
    glyph.setClock([&now] { return now; });
    glyph.setLink(QStringLiteral("connected"));
    CHECK(glyph.isDrawingWire());
    now += RemoteLinkGlyph::kDrawMs + RemoteLinkGlyph::kLightMs;
    glyph.tick();
    glyph.setLink(QStringLiteral("offline"));
    glyph.setLink(QStringLiteral("connected"));
    CHECK_FALSE(glyph.isDrawingWire());
}

// from tests/test_remote_link_ui.py::test_the_glyph_asks_to_reconnect_when_clicked
TEST_CASE("test_the_glyph_asks_to_reconnect_when_clicked", "[test_remote_link_ui][widget]") {
    RemoteLinkGlyph glyph;
    QSignalSpy clicked(&glyph, &RemoteLinkGlyph::clicked);
    const QPointF point(10, 8);
    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&glyph, &release);
    CHECK(clicked.count() == 1);
    // Outside the glyph: no click.
    const QPointF outside(60, 8);
    QMouseEvent away(QEvent::MouseButtonRelease, outside, outside, Qt::LeftButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&glyph, &away);
    CHECK(clicked.count() == 1);
}

TEST_CASE("the glyph is 44 by 16, and an offline link takes the pointing hand",
          "[test_remote_link_ui][widget]") {
    RemoteLinkGlyph glyph;
    CHECK(glyph.size() == QSize(44, 16));
    CHECK(glyph.objectName() == QStringLiteral("remoteLinkGlyph"));
    glyph.setLink(QStringLiteral("offline"));
    CHECK(glyph.cursor().shape() == Qt::PointingHandCursor);
    glyph.setLink(QStringLiteral("connecting"));
    CHECK(glyph.cursor().shape() == Qt::ArrowCursor);
    // Busy only counts while connected.
    glyph.setLink(QStringLiteral("connecting"), true);
    CHECK_FALSE(glyph.busy());
    RemoteLinkGlyph serving(nullptr, true);
    CHECK(serving.mirrored());
}

// ---- Manage host models ----

TEST_CASE("manage host models names what the host could install", "[remote_model_notice][widget]") {
    RemoteModelNotice notice;
    CHECK(notice.text() == QStringLiteral("Manage host models"));
    CHECK(notice.objectName() == QStringLiteral("remoteModelNotice"));
    CHECK(notice.isFlat());
    CHECK(notice.focusPolicy() == Qt::TabFocus);
    CHECK(notice.toolTip() ==
          QStringLiteral(
              "Browse models, manage downloads, and set up CPU or GPU on the paired host."));
    notice.setAvailable({QStringLiteral("GPU Acceleration"), QStringLiteral("Parakeet runtime")});
    CHECK(notice.available().size() == 2);
    CHECK(notice.toolTip() ==
          QStringLiteral("Available to install on the paired host: GPU Acceleration, Parakeet "
                         "runtime. Browse models, manage downloads, and set up CPU or GPU on the "
                         "paired host."));
    CHECK(notice.accessibleDescription() == notice.toolTip());
    notice.setAvailable({});
    CHECK(notice.available().isEmpty());
    // It paints without an icon of its own: the glyph is drawn in the theme's left padding.
    CHECK(notice.icon().isNull());
    notice.resize(140, 18);
    CHECK_FALSE(notice.grab().isNull());
}

// ---- the host's runtime fields ----

TEST_CASE("the runtime fields show only what the paired host reports",
          "[remote_engine_controls][widget]") {
    RemoteEngineControls controls;
    controls.show();
    QApplication::processEvents();
    // Not connected yet.
    CHECK(items(controls.deviceCombo()) == QStringList{QStringLiteral("Not connected")});
    CHECK_FALSE(controls.deviceCombo()->isEnabled());
    CHECK(controls.deviceCombo()->toolTip() ==
          QStringLiteral("Connect to the host to see its runtime choices."));

    controls.setState(parakeetHost());
    CHECK(items(controls.deviceCombo()) ==
          QStringList{QStringLiteral("CPU"), QStringLiteral("GPU")});
    CHECK(controls.deviceCombo()->currentText() == QStringLiteral("GPU"));
    CHECK(items(controls.languageCombo()) ==
          QStringList{QStringLiteral("English"), QStringLiteral("French"), QStringLiteral("Auto")});
    CHECK(controls.languageCombo()->currentData().toString() == QStringLiteral("en"));
    // Whisper's precision means nothing to Parakeet; its language does.
    CHECK_FALSE(controls.field(QStringLiteral("compute_type"))->isVisible());
    CHECK(controls.field(QStringLiteral("language"))->isVisible());
    CHECK(controls.deviceCombo()->isEnabled());
    CHECK(controls.deviceCombo()->toolTip() ==
          QStringLiteral("Runtime on jed. Changes apply to the host and connected clients."));

    // A host that reports its engine but takes no runtime changes from here.
    controls.setState(parakeetHost(false));
    CHECK_FALSE(controls.deviceCombo()->isEnabled());
    CHECK(controls.deviceCombo()->toolTip() ==
          QStringLiteral("Update %1 on jed to change its runtime from here.")
              .arg(QString::fromUtf8(fv::identity().product)));

    // An engine that reports no choices.
    auto bare = parakeetHost();
    bare.runtime = Json{{"family", "parakeet"}, {"model", "parakeet-v3"}};
    bare.engine = Json{{"family", "parakeet"}};
    controls.setState(bare);
    CHECK(items(controls.deviceCombo()) == QStringList{QStringLiteral("Not reported")});
}

TEST_CASE("choosing a runtime setting asks the host and locks until it answers",
          "[remote_engine_controls][widget]") {
    RemoteEngineControls controls;
    controls.show();
    controls.setState(parakeetHost());
    QSignalSpy chosen(&controls, &RemoteEngineControls::runtimeSelected);
    // The value already selected is no change.
    Q_EMIT controls.deviceCombo()->activated(
        controls.deviceCombo()->findData(QStringLiteral("cuda")));
    CHECK(chosen.isEmpty());
    Q_EMIT controls.deviceCombo()->activated(
        controls.deviceCombo()->findData(QStringLiteral("cpu")));
    REQUIRE(chosen.count() == 1);
    CHECK(chosen[0][0].toString() == QStringLiteral("parakeet"));
    CHECK(chosen[0][1].toString() == QStringLiteral("parakeet-v3"));
    CHECK(chosen[0][2].toMap() == QVariantMap{{QStringLiteral("device"), QStringLiteral("cpu")}});
    CHECK_FALSE(controls.deviceCombo()->isEnabled());
    // The host's next report unlocks it.
    controls.setState(parakeetHost());
    CHECK(controls.deviceCombo()->isEnabled());
    controls.setLocked(true);
    CHECK_FALSE(controls.deviceCombo()->isEnabled());
    CHECK_FALSE(controls.languageCombo()->isEnabled());
    controls.setLocked(false);
    CHECK(controls.languageCombo()->isEnabled());
}

TEST_CASE("a device the host could set up opens its model management",
          "[remote_engine_controls][widget]") {
    RemoteEngineControls controls;
    controls.show();
    auto state = parakeetHost();
    (*state.runtime)["devices"] = Json::array({"cpu"});
    (*state.runtime)["selected"]["device"] = "cpu";
    (*state.runtime)["dependencies"] = Json::array(
        {Json{{"label", "GPU Acceleration"}, {"device", "cuda"}, {"installable", true}}});
    controls.setState(state);
    CHECK(items(controls.deviceCombo()) ==
          QStringList{QStringLiteral("CPU"), QStringLiteral(u"GPU · Set up on host")});
    QSignalSpy help(&controls, &RemoteEngineControls::helpRequested);
    QSignalSpy chosen(&controls, &RemoteEngineControls::runtimeSelected);
    Q_EMIT controls.deviceCombo()->activated(1);
    CHECK(chosen.isEmpty());
    REQUIRE(help.count() == 1);
    CHECK(help[0][0].toString() == QStringLiteral("remote_models"));
    // The field goes back to what the host runs.
    CHECK(controls.deviceCombo()->currentText() == QStringLiteral("CPU"));
}

TEST_CASE("a saved choice the host no longer offers stays shown and greyed",
          "[remote_engine_controls][widget]") {
    RemoteEngineControls controls;
    auto state = parakeetHost();
    (*state.runtime)["languages"] = Json::array({"en", "auto"});
    (*state.runtime)["selected"]["language"] = "fr";
    controls.setState(state);
    QComboBox* combo = controls.languageCombo();
    CHECK(combo->currentData().toString() == QStringLiteral("fr"));
    CHECK(combo->toolTip().endsWith(
        QStringLiteral(" The saved choice is currently unavailable on the host.")));
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    REQUIRE(model != nullptr);
    CHECK_FALSE(model->item(combo->findData(QStringLiteral("fr")))->isEnabled());
}
