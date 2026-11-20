// Settings → Recording → Microphone: the preferred microphone, its backups, the device list off
// the GUI thread and keyboard focus through every rebuild (tests/test_personalize_s7_settings.py,
// tests/test_personalize_fix2_g2_mic_focus.py, tests/test_desktop_performance.py), over
// audio::FakeBackend's device list.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

#include "app_pages_support.h"
#include "appcore/mic_priority.h"
#include "ui_settings/microphone_section.h"
#include "ui_settings/recording_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

QString token(const Json& saved) {
    return qs(fv::appcore::mic::entry_token(
        {saved["name"].get<std::string>(), saved["hostapi"].get<std::string>()}));
}

// make_dialog: Settings on Recording, the first device list in.
struct MicScene {
    explicit MicScene(const Json& values = Json::object(), bool wait = true) : rig(values) {
        QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::audioDeviceChanged,
                         [this](const Json& priority) { changed.push_back(priority); });
        page = &rig.page<fv::RecordingPage>(dest::recording);
        if (wait) {
            REQUIRE(pumpUntil([this] { return section().devices().has_value(); }));
        }
    }

    fv::MicrophoneSection& section() { return page->microphones(); }
    QComboBox* combo() { return section().combo(); }
    Json priority() { return rig.saved("audio_input_priority"); }

    std::vector<QWidget*> rows() {
        std::vector<QWidget*> all;
        QVBoxLayout* layout = section().rowsLayout();
        for (int i = 0; i < layout->count(); ++i) {
            all.push_back(layout->itemAt(i)->widget());
        }
        return all;
    }

    AppPagesRig rig;
    fv::RecordingPage* page = nullptr;
    std::vector<Json> changed;
};

std::vector<QString> rowText(QWidget* row) {
    std::vector<QString> texts;
    for (QLabel* label : row->findChildren<QLabel*>()) {
        auto* wrapped = qobject_cast<fv::WrappedLabel*>(label);
        texts.push_back(wrapped != nullptr ? wrapped->text() : label->text());
    }
    return texts;
}

QPushButton* button(QWidget* row, const char* kind) {
    QPushButton* found = fv::MicrophoneSection::rowButton(row, QString::fromLatin1(kind));
    REQUIRE(found != nullptr);
    return found;
}

std::vector<std::pair<QString, QString>> comboItems(QComboBox* combo) {
    std::vector<std::pair<QString, QString>> items;
    for (int i = 0; i < combo->count(); ++i) {
        items.emplace_back(combo->itemText(i), combo->itemData(i).toString());
    }
    return items;
}

std::vector<fv::audio::InputDevice> devicesOf(AppPagesRig& rig) {
    return fv::audio::list_input_devices(rig.backend());
}

// The `shown` fixture of the focus tests: the window shown at 1000 x 800 and active.
struct ShownScene : MicScene {
    explicit ShownScene(const Json& priority) : MicScene({{"audio_input_priority", priority}}) {
        rig.dialog().resize(1000, 800);
        rig.dialog().show();
        rig.dialog().activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&rig.dialog()));
        flush();
    }
    ~ShownScene() { rig.dialog().hide(); }
};

void focus(QWidget* widget) {
    widget->setFocus(Qt::TabFocusReason);
    flush();
}

void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::keyClick(QApplication::focusWidget(), key, modifiers);
    flush();
}

QString focusedName() {
    QWidget* widget = QApplication::focusWidget();
    return widget != nullptr ? widget->accessibleName() : QString();
}

} // namespace

// --- tests/test_personalize_s7_settings.py ---

// from
// tests/test_personalize_s7_settings.py::test_preferred_combo_lists_one_row_per_microphone_and_keeps_saved_ones
TEST_CASE("test_preferred_combo_lists_one_row_per_microphone_and_keeps_saved_ones",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kDsoundUsb})}});

    CHECK(comboItems(scene.combo()) ==
          std::vector<std::pair<QString, QString>>{
              {QStringLiteral("System default"), QString()},
              {QStringLiteral("Microphone (Blue Snowball)"), token(kSnowball)},
              {QStringLiteral("Microphone (USB Audio Device)"), token(kUsb)},
              {QStringLiteral("Microphone (Webcam)"), token(kWebcam)},
              {QStringLiteral(u"Microphone (Lapel) · Not connected"), token(kLapel)},
              {QStringLiteral(u"Microphone (USB Audio Device) · DirectSound"), token(kDsoundUsb)},
          });
    CHECK(scene.combo()->currentData().toString() == token(kUsb));
}

// from
// tests/test_personalize_s7_settings.py::test_backups_list_in_order_with_a_greyed_disconnected_row_and_system_default_last
TEST_CASE("test_backups_list_in_order_with_a_greyed_disconnected_row_and_system_default_last",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kSnowball})}});

    const auto rows = scene.rows();
    REQUIRE(rows.size() == 3);
    CHECK(rowText(rows[0]) == std::vector<QString>{QStringLiteral("Microphone (Lapel)"),
                                                   QStringLiteral("Not connected")});
    CHECK(rowText(rows[1]) == std::vector<QString>{QStringLiteral("Microphone (Blue Snowball)")});
    CHECK(rowText(rows[2]) ==
          std::vector<QString>{QStringLiteral("System default"), QStringLiteral("Always last")});
    CHECK(rows[0]->property("state").toString() == QStringLiteral("missing"));
    CHECK(rows[1]->property("state").toString() == QString());
    CHECK(rows[2]->property("state").toString() == QStringLiteral("fixed"));
    CHECK_FALSE(button(rows[0], "up")->isEnabled());
    CHECK(button(rows[0], "down")->isEnabled());
    CHECK(button(rows[1], "up")->isEnabled());
    CHECK_FALSE(button(rows[1], "down")->isEnabled());
    CHECK(rows[2]->findChildren<QPushButton*>().isEmpty());
    CHECK(scene.rig.dialog().rail()->value(qs(dest::recording)) ==
          QStringLiteral(u"USB Audio Device → Lapel +1"));
}

// from tests/test_personalize_s7_settings.py::test_up_down_and_remove_save_the_new_order
TEST_CASE("test_up_down_and_remove_save_the_new_order", "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kSnowball})}});

    button(scene.rows()[1], "up")->click();
    CHECK(scene.priority() == Json::array({kUsb, kSnowball, kLapel}));
    button(scene.rows()[0], "down")->click();
    CHECK(scene.priority() == Json::array({kUsb, kLapel, kSnowball}));
    button(scene.rows()[0], "remove")->click();
    CHECK(scene.priority() == Json::array({kUsb, kSnowball}));

    CHECK(scene.changed == std::vector<Json>{Json::array({kUsb, kSnowball, kLapel}),
                                             Json::array({kUsb, kLapel, kSnowball}),
                                             Json::array({kUsb, kSnowball})});
    CHECK(scene.rig.dialog().rail()->value(qs(dest::recording)) ==
          QStringLiteral(u"USB Audio Device → Blue Snowball"));
}

// from
// tests/test_personalize_s7_settings.py::test_add_offers_connected_microphones_not_already_ranked
TEST_CASE("test_add_offers_connected_microphones_not_already_ranked",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb})}});
    QComboBox* add = scene.section().addCombo();

    std::vector<QString> texts;
    for (const auto& [text, data] : comboItems(add)) {
        texts.push_back(text);
    }
    CHECK(texts == std::vector<QString>{QStringLiteral(u"Add a microphone…"),
                                        QStringLiteral("Microphone (Blue Snowball)"),
                                        QStringLiteral("Microphone (Webcam)")});
    add->setCurrentIndex(add->findData(token(kWebcam)));
    Q_EMIT add->activated(add->currentIndex());

    CHECK(scene.priority() == Json::array({kUsb, kWebcam}));
    CHECK(add->currentIndex() == 0);
    texts.clear();
    for (const auto& [text, data] : comboItems(add)) {
        texts.push_back(text);
    }
    CHECK(texts == std::vector<QString>{QStringLiteral(u"Add a microphone…"),
                                        QStringLiteral("Microphone (Blue Snowball)")});
}

// from
// tests/test_personalize_s7_settings.py::test_choosing_a_backup_as_preferred_moves_it_to_the_front
TEST_CASE("test_choosing_a_backup_as_preferred_moves_it_to_the_front",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kSnowball})}});

    scene.combo()->setCurrentIndex(scene.combo()->findData(token(kSnowball)));

    CHECK(scene.priority() == Json::array({kSnowball, kUsb, kLapel}));
    CHECK(scene.rig.saved("audio_input_device") == 1);
    std::vector<QString> first;
    for (QWidget* row : scene.rows()) {
        first.push_back(rowText(row)[0]);
    }
    CHECK(first == std::vector<QString>{QStringLiteral("Microphone (USB Audio Device)"),
                                        QStringLiteral("Microphone (Lapel)"),
                                        QStringLiteral("System default")});
}

// from
// tests/test_personalize_s7_settings.py::test_system_default_clears_the_order_and_hides_backups
TEST_CASE("test_system_default_clears_the_order_and_hides_backups",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene(
        {{"audio_input_priority", Json::array({kUsb, kSnowball})}, {"audio_input_device", 2}});

    scene.combo()->setCurrentIndex(scene.combo()->findData(QString()));

    CHECK(scene.priority() == Json::array());
    CHECK_FALSE(scene.rig.has("audio_input_device"));
    CHECK(scene.section().backupTile()->isHidden());
    CHECK(scene.rig.dialog().rail()->value(qs(dest::recording)) ==
          QStringLiteral("System default"));
}

// from
// tests/test_personalize_s7_settings.py::test_a_preferred_microphone_on_another_host_api_drops_the_legacy_index
TEST_CASE("test_a_preferred_microphone_on_another_host_api_drops_the_legacy_index",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene(
        {{"audio_input_device", 2}, {"audio_input_priority", Json::array({kUsb, kDsoundUsb})}});

    scene.combo()->setCurrentIndex(scene.combo()->findData(token(kDsoundUsb)));

    CHECK(scene.priority() == Json::array({kDsoundUsb, kUsb}));
    CHECK_FALSE(scene.rig.has("audio_input_device"));
}

// from
// tests/test_personalize_s7_settings.py::test_nothing_is_called_disconnected_before_the_device_list_arrives
TEST_CASE("test_nothing_is_called_disconnected_before_the_device_list_arrives",
          "[test_personalize_s7_settings][widget]") {
    AppPagesRig rig({{"audio_input_priority", Json::array({kUsb, kLapel})}});
    auto release = std::make_shared<Latch>();
    rig.listGate = release;
    auto& page = rig.page<fv::RecordingPage>(dest::recording);
    auto& section = page.microphones();
    REQUIRE(rig.listEntered.wait(std::chrono::seconds(2)));

    QWidget* first = section.rowsLayout()->itemAt(0)->widget();
    CHECK(rowText(first) == std::vector<QString>{QStringLiteral("Microphone (Lapel)")});
    CHECK(section.addCombo()->itemText(0) == QStringLiteral(u"Looking for microphones…"));
    CHECK_FALSE(section.addCombo()->isEnabled());

    release->release();
    REQUIRE(pumpUntil([&] { return section.devices().has_value(); }));
    flush();
    CHECK(rowText(section.rowsLayout()->itemAt(0)->widget()) ==
          std::vector<QString>{QStringLiteral("Microphone (Lapel)"),
                               QStringLiteral("Not connected")});
}

// from
// tests/test_personalize_s7_settings.py::test_a_window_closed_while_listing_microphones_is_not_held_by_the_worker
TEST_CASE("test_a_window_closed_while_listing_microphones_is_not_held_by_the_worker",
          "[test_personalize_s7_settings][widget]") {
    // The worker used to capture the microphone section, and through it the window: it kept a
    // closed window alive, then could drop the last reference (deleting a QWidget) off the Qt
    // thread.
    AppPagesRig rig({{"audio_input_priority", Json::array({kUsb})}});
    auto gate = std::make_shared<Latch>();
    rig.listGate = gate;
    rig.page<fv::RecordingPage>(dest::recording);
    REQUIRE(rig.listEntered.wait(std::chrono::seconds(2)));
    QPointer<fv::SettingsDialog> window = &rig.dialog();

    rig.dropDialog();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    CHECK(window == nullptr);

    gate->release();
    // The list arrives for a window that is gone, and is dropped.
    rig.pool().shutdown(std::chrono::seconds(5));
    QApplication::processEvents();
    CHECK(rig.listings.load() == 1);
}

// from
// tests/test_personalize_s7_settings.py::test_refresh_rereads_devices_only_while_nothing_records
TEST_CASE("test_refresh_rereads_devices_only_while_nothing_records",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb})}});
    auto& section = scene.section();
    int stream = 0;
    fv::audio::register_stream(&stream);
    section.refreshButton()->click();
    CHECK(section.status()->text().contains(QStringLiteral("in use")));
    CHECK(scene.rig.refreshes.load() == 0);
    fv::audio::unregister_stream(&stream);

    scene.rig.meetingActive = true;
    section.refreshButton()->click();
    CHECK(scene.rig.refreshes.load() == 0);

    scene.rig.meetingActive = false;
    const int before = scene.rig.listings.load();
    section.refreshButton()->click();
    CHECK_FALSE(section.refreshButton()->isEnabled());
    REQUIRE(pumpUntil([&] { return section.refreshButton()->isEnabled(); }));
    CHECK(scene.rig.refreshes.load() == 1);
    CHECK(section.status()->isHidden());
    CHECK(scene.rig.listings.load() == before + 1);
}

// from tests/test_personalize_s7_settings.py::test_rail_value_reads_short_names
TEST_CASE("test_rail_value_reads_short_names", "[test_personalize_s7_settings]") {
    CHECK(fv::appcore::mic::rail_value(Json::object()) == "System default");
    CHECK(fv::appcore::mic::rail_value({{"audio_input_priority", Json::array({kSnowball})}}) ==
          "Blue Snowball");
}

// from
// tests/test_personalize_s7_settings.py::test_recording_page_fits_narrow_windows_and_large_fonts
TEST_CASE("test_recording_page_fits_narrow_windows_and_large_fonts",
          "[test_personalize_s7_settings][widget]") {
    // The classic window at its 940 px floor; Omarchy's 560 px variant is Linux's.
    AppTheme theme(QStringLiteral("dark"), 130);
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kSnowball})}});
    fv::SettingsDialog& dialog = scene.rig.dialog();
    dialog.show();
    dialog.fitToScreen();
    dialog.resize(940, 600);
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents();
    }
    QWidget* page = dialog.page(dest::recording);
    fv::InfoTile* tile = scene.section().backupTile();
    REQUIRE(tile->isVisible());
    QList<QWidget*> controls;
    for (QAbstractButton* button : tile->findChildren<QAbstractButton*>()) {
        controls.append(button);
    }
    for (QComboBox* combo : scene.section().preferredTile()->findChildren<QComboBox*>()) {
        controls.append(combo);
    }
    for (QComboBox* combo : tile->findChildren<QComboBox*>()) {
        controls.append(combo);
    }
    controls.append(scene.section().refreshButton());
    for (QWidget* control : controls) {
        if (!control->isVisible()) {
            continue;
        }
        INFO(control->objectName().toStdString() << " " << control->accessibleName().toStdString());
        CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
        CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
    }
    for (QWidget* row : scene.rows()) {
        for (QLabel* label : row->findChildren<QLabel*>()) {
            CHECK(label->width() > 0);
            CHECK(label->height() >= label->sizeHint().height() - 1);
        }
    }
    for (QLabel* label : page->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    dialog.close();
}

// from
// tests/test_personalize_s7_settings.py::test_system_default_suggests_choosing_a_microphone_for_backups
TEST_CASE("test_system_default_suggests_choosing_a_microphone_for_backups",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene;
    fv::FieldTile* tile = scene.section().preferredTile();
    CHECK(tile->descriptionLabel()->text().contains(QStringLiteral("backups")));

    scene.combo()->setCurrentIndex(scene.combo()->findData(token(kSnowball)));

    CHECK(tile->descriptionLabel()->text() == QStringLiteral("Used for dictation and meetings."));
    CHECK_FALSE(scene.section().backupTile()->isHidden());
}

// from tests/test_personalize_s7_settings.py::test_refresh_moves_under_the_combo_in_a_narrow_column
TEST_CASE("test_refresh_moves_under_the_combo_in_a_narrow_column",
          "[test_personalize_s7_settings][widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb})}});
    auto* row = qobject_cast<fv::PreferredRow*>(scene.combo()->parentWidget());
    REQUIRE(row != nullptr);
    fv::SettingsDialog& dialog = scene.rig.dialog();
    dialog.show();
    // Omarchy's floor. No explicit minimum would hand the window the layout's, which counts the
    // rail until the narrow window folds it away.
    dialog.setMinimumSize(360, 240);
    // 420 folds the rail away, so the page itself is that narrow.
    const std::pair<int, QBoxLayout::Direction> steps[] = {
        {1100, QBoxLayout::LeftToRight},
        {420, QBoxLayout::TopToBottom},
        {1100, QBoxLayout::LeftToRight},
    };
    for (const auto& [width, direction] : steps) {
        dialog.resize(width, 700);
        for (int i = 0; i < 8; ++i) {
            QApplication::processEvents();
        }
        INFO(width);
        CHECK(row->boxLayout()->direction() == direction);
    }
    dialog.close();
}

// --- tests/test_personalize_fix2_g2_mic_focus.py ---

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_moved_microphone_keeps_focus_and_a_second_space_moves_it_back
TEST_CASE("test_moved_microphone_keeps_focus_and_a_second_space_moves_it_back",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel}));
    focus(button(scene.rows()[0], "down"));
    std::vector<bool> inList;
    QWidget* rows = scene.section().rows();
    const auto connection = QObject::connect(
        qApp, &QApplication::focusChanged, [&inList, rows](QWidget*, QWidget* now) {
            inList.push_back(now != nullptr && rows->isAncestorOf(now));
        });
    key(Qt::Key_Space);
    QObject::disconnect(connection);

    CHECK(scene.priority() == Json::array({kUsb, kLapel, kSnowball}));
    // Down is gone at the bottom, so the same microphone's Up takes focus.
    CHECK(focusedName() == QStringLiteral("Move up: Microphone (Blue Snowball)"));
    CHECK(inList == std::vector<bool>{true});

    key(Qt::Key_Space);

    CHECK(scene.priority() == Json::array({kUsb, kSnowball, kLapel}));
    CHECK(focusedName() == QStringLiteral("Move down: Microphone (Blue Snowball)"));
    CHECK(scene.rig.dialog().rail()->currentKey() == qs(dest::recording));
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_moved_microphone_keeps_the_same_button_away_from_the_edges
TEST_CASE("test_moved_microphone_keeps_the_same_button_away_from_the_edges",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel, kWebcam}));
    focus(button(scene.rows()[0], "down"));

    key(Qt::Key_Space);

    CHECK(scene.priority() == Json::array({kUsb, kLapel, kSnowball, kWebcam}));
    CHECK(focusedName() == QStringLiteral("Move down: Microphone (Blue Snowball)"));
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_removing_a_microphone_focuses_the_row_that_takes_its_place
TEST_CASE("test_removing_a_microphone_focuses_the_row_that_takes_its_place",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    struct Case {
        int row;
        Json left;
        const char* focused;
    };
    const Case c =
        GENERATE(Case{0, Json::array({kUsb, kLapel, kWebcam}), "Remove: Microphone (Lapel)"},
                 Case{1, Json::array({kUsb, kSnowball, kWebcam}), "Remove: Microphone (Webcam)"},
                 Case{2, Json::array({kUsb, kSnowball, kLapel}), "Remove: Microphone (Lapel)"});
    INFO(c.row);
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel, kWebcam}));
    focus(button(scene.rows()[static_cast<std::size_t>(c.row)], "remove"));

    key(Qt::Key_Space);

    CHECK(scene.priority() == c.left);
    CHECK(focusedName() == QString::fromUtf8(c.focused));
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_removing_the_only_backup_focuses_the_add_picker
TEST_CASE("test_removing_the_only_backup_focuses_the_add_picker",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball}));
    focus(button(scene.rows()[0], "remove"));

    key(Qt::Key_Space);

    CHECK(scene.priority() == Json::array({kUsb}));
    CHECK(QApplication::focusWidget() == scene.section().addCombo());
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_adding_the_last_free_microphone_focuses_its_row
TEST_CASE("test_adding_the_last_free_microphone_focuses_its_row",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    // Only the MME devices (Python keeps indexes 1 to 3; the Sound Mapper at 0 is never
    // listed), narrowed before the window lists them.
    AppPagesRig narrowed({{"audio_input_priority", Json::array({kUsb, kSnowball})}});
    narrowed.backend().keep_devices(4);
    auto& page = narrowed.page<fv::RecordingPage>(dest::recording);
    auto& section = page.microphones();
    REQUIRE(pumpUntil([&] { return section.devices().has_value(); }));
    narrowed.dialog().resize(1000, 800);
    narrowed.dialog().show();
    narrowed.dialog().activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&narrowed.dialog()));
    flush();
    QComboBox* add = section.addCombo();
    focus(add);

    add->setCurrentIndex(add->findData(token(kWebcam)));
    Q_EMIT add->activated(add->currentIndex());
    flush();

    CHECK(narrowed.saved("audio_input_priority") == Json::array({kUsb, kSnowball, kWebcam}));
    CHECK_FALSE(add->isEnabled());
    CHECK(focusedName() == QStringLiteral("Remove: Microphone (Webcam)"));
    narrowed.dialog().hide();
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_a_device_refresh_keeps_focus_on_the_same_button
TEST_CASE("test_a_device_refresh_keeps_focus_on_the_same_button",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel}));
    focus(button(scene.rows()[1], "up"));
    QWidget* old = QApplication::focusWidget();

    scene.section().applyDevices(devicesOf(scene.rig), QString());
    flush();

    CHECK(QApplication::focusWidget() != old);
    CHECK(focusedName() == QStringLiteral("Move up: Microphone (Lapel)"));
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_a_rebuild_leaves_focus_outside_the_list_alone
TEST_CASE("test_a_rebuild_leaves_focus_outside_the_list_alone",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel}));
    focus(scene.combo());

    auto devices = devicesOf(scene.rig);
    fv::audio::InputDevice dock;
    dock.index = 9;
    dock.name = "Microphone (Dock)";
    dock.hostapi = kMme;
    dock.default_api = true;
    devices.push_back(dock);
    scene.section().applyDevices(devices, QString());
    flush();

    CHECK(QApplication::focusWidget() == scene.combo());
}

// from
// tests/test_personalize_fix2_g2_mic_focus.py::test_tab_reaches_the_backups_between_refresh_and_add
TEST_CASE("test_tab_reaches_the_backups_between_refresh_and_add",
          "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel}));
    auto& section = scene.section();
    focus(section.refreshButton());

    std::vector<QWidget*> walk;
    for (int i = 0; i < 6; ++i) {
        key(Qt::Key_Tab);
        walk.push_back(QApplication::focusWidget());
    }

    std::vector<QString> names;
    for (int i = 0; i < 4; ++i) {
        names.push_back(walk[static_cast<std::size_t>(i)]->accessibleName());
    }
    CHECK(names == std::vector<QString>{QStringLiteral("Move down: Microphone (Blue Snowball)"),
                                        QStringLiteral("Remove: Microphone (Blue Snowball)"),
                                        QStringLiteral("Move up: Microphone (Lapel)"),
                                        QStringLiteral("Remove: Microphone (Lapel)")});
    CHECK(walk[4] == section.addCombo());
    CHECK_FALSE(section.rows()->isAncestorOf(walk[5]));

    focus(button(scene.rows()[0], "down"));
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(QApplication::focusWidget() == section.refreshButton());
}

// from tests/test_personalize_fix2_g2_mic_focus.py::test_tab_order_holds_after_a_move
TEST_CASE("test_tab_order_holds_after_a_move", "[test_personalize_fix2_g2_mic_focus][widget]") {
    ShownScene scene(Json::array({kUsb, kSnowball, kLapel}));
    auto& section = scene.section();
    focus(button(scene.rows()[0], "down"));
    key(Qt::Key_Space);

    key(Qt::Key_Tab);
    CHECK(focusedName() == QStringLiteral("Remove: Microphone (Blue Snowball)"));
    key(Qt::Key_Tab);
    CHECK(QApplication::focusWidget() == section.addCombo());
}

// --- tests/test_desktop_performance.py ---

// from
// tests/test_desktop_performance.py::test_recording_driver_result_preserves_selection_and_gui_progress
TEST_CASE("test_recording_driver_result_preserves_selection_and_gui_progress",
          "[test_desktop_performance][widget]") {
    const Json desk = entry("Desk microphone");
    const Json lapel = entry("Lapel microphone");
    AppPagesRig rig({{"audio_input_priority", Json::array({desk, lapel})}});
    rig.backend().set_layout({fv::audio::testing::FakeHostApi{kMme, 0, -1}},
                             {fv::audio::testing::FakeDevice{"USB microphone", 0, 1, 0, 44100.0}},
                             0, 0);
    auto release = std::make_shared<Latch>();
    rig.listGate = release;
    auto& page = rig.page<fv::RecordingPage>(dest::recording);
    REQUIRE(rig.listEntered.wait(std::chrono::seconds(1)));
    QComboBox* combo = page.microphones().combo();
    combo->setCurrentIndex(combo->findData(token(lapel)));
    std::vector<bool> ticks;
    QTimer::singleShot(0, [&ticks] { ticks.push_back(true); });
    QApplication::processEvents();
    CHECK(ticks == std::vector<bool>{true});
    CHECK_FALSE(release->isOpen());
    release->release();
    const QString usb = qs(fv::appcore::mic::entry_token({"USB microphone", kMme}));
    REQUIRE(pumpUntil([&] { return combo->findData(usb) >= 0; }));
    CHECK(combo->currentData().toString() == token(lapel));
}

// A11-63, A11-100: each backup row's Up, Down and Remove are 28 px flat buttons with their
// tooltips; the first row's Up and the last row's Down are hidden but keep their space, and the
// backup tile says the list is followed even mid-dictation.
TEST_CASE("backup rows carry flat 28 px buttons and keep the space of a hidden one", "[widget]") {
    MicScene scene({{"audio_input_priority", Json::array({kUsb, kLapel, kSnowball})}});
    CHECK(scene.section().backupTile()->titleLabel()->text() ==
          QStringLiteral("If it's unplugged, use"));
    CHECK(
        scene.section().backupTile()->descriptionLabel()->text() ==
        QStringLiteral("OpenWhisper_CPP moves down this list, even in the middle of a dictation."));
    const auto rows = scene.rows();
    REQUIRE(rows.size() == 3);
    const std::vector<std::pair<const char*, QString>> tips{{"up", QStringLiteral("Move up")},
                                                            {"down", QStringLiteral("Move down")},
                                                            {"remove", QStringLiteral("Remove")}};
    for (const auto& [kind, tip] : tips) {
        QPushButton* made = button(rows[0], kind);
        REQUIRE(made != nullptr);
        CHECK(made->size() == QSize(28, 28));
        CHECK(made->isFlat());
        CHECK(made->toolTip() == tip);
    }
    CHECK(button(rows[0], "up")->isHidden());
    CHECK(button(rows[0], "up")->sizePolicy().retainSizeWhenHidden());
    CHECK_FALSE(button(rows[0], "down")->isHidden());
    CHECK(button(rows[1], "down")->isHidden());
    CHECK(button(rows[1], "down")->sizePolicy().retainSizeWhenHidden());
    CHECK_FALSE(button(rows[1], "up")->isHidden());
}
