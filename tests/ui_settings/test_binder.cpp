// SettingsBinder and the Settings controls it loads and saves (tests/test_settings_binder.py).
// The dialog-level cases bind the same keys through stand-in General and Meeting Mode pages: the
// real pages and their resolvers arrive with their own packages.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFocusEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include "settings_support.h"
#include "ui_settings/settings_binder.h"
#include "ui_widgets/settings_switch.h"

using fvtest::SettingsRig;

namespace {

// _Recorder: what persist was asked to save, unless loading.
struct Recorder {
    bool loading = false;
    std::vector<std::pair<std::string, fv::Json>> saved;

    fv::SettingsBinder::Persist persist() {
        return [this](std::string_view key, const fv::Json& value) {
            if (loading) {
                return false;
            }
            saved.emplace_back(std::string(key), value);
            return true;
        };
    }
};

using Saved = std::vector<std::pair<std::string, fv::Json>>;

fv::SettingsBinder::Resolve valueOr(std::string key, fv::Json fallback) {
    return [key = std::move(key), fallback = std::move(fallback)](const fv::Json& settings) {
        return settings.is_object() && settings.contains(key) ? settings[key] : fallback;
    };
}

void leave(QWidget* edit) {
    QFocusEvent event(QEvent::FocusOut);
    QApplication::sendEvent(edit, &event);
}

} // namespace

// from tests/test_settings_binder.py::test_checkbox_saves_on_toggle_and_loads_resolved_value
TEST_CASE("test_checkbox_saves_on_toggle_and_loads_resolved_value", "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QCheckBox check;
    binder.checkbox(&check, "flag", valueOr("flag", true));

    check.setChecked(true);
    check.setChecked(false);
    CHECK(persist.saved == Saved{{"flag", true}, {"flag", false}});

    persist.loading = true;
    binder.load(fv::Json::object());
    CHECK(check.isChecked());
    binder.load(fv::Json{{"flag", false}});
    CHECK_FALSE(check.isChecked());
    CHECK(persist.saved == Saved{{"flag", true}, {"flag", false}});
}

// from tests/test_settings_binder.py::test_combo_saves_item_data_and_falls_back_to_first_item
TEST_CASE("test_combo_saves_item_data_and_falls_back_to_first_item", "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QComboBox combo;
    combo.addItem(QStringLiteral("Normal"), QStringLiteral("normal"));
    combo.addItem(QStringLiteral("Thorough"), QStringLiteral("thorough"));
    binder.combo(&combo, "mode", valueOr("mode", nullptr));

    combo.setCurrentIndex(1);
    CHECK(persist.saved == Saved{{"mode", "thorough"}});

    persist.loading = true;
    binder.load(fv::Json{{"mode", "unknown"}});
    CHECK(combo.currentIndex() == 0);
    binder.load(fv::Json{{"mode", "thorough"}});
    CHECK(combo.currentIndex() == 1);
}

// from tests/test_settings_binder.py::test_spin_saves_int_values
TEST_CASE("test_spin_saves_int_values", "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QSpinBox spin;
    spin.setMaximum(100);
    binder.spin(&spin, "port", valueOr("port", 0));

    spin.setValue(42);
    CHECK(persist.saved == Saved{{"port", 42}});
    persist.loading = true;
    binder.load(fv::Json{{"port", 7}});
    CHECK(spin.value() == 7);
}

// from tests/test_settings_binder.py::test_load_follows_binding_order
TEST_CASE("test_load_follows_binding_order", "[test_settings_binder]") {
    std::vector<std::string> order;
    fv::SettingsBinder binder([](std::string_view, const fv::Json&) { return true; });
    std::vector<std::unique_ptr<QCheckBox>> checks;
    for (const char* name : {"first", "second", "third"}) {
        checks.push_back(std::make_unique<QCheckBox>());
        binder.checkbox(checks.back().get(), name, [&order, name](const fv::Json&) {
            order.emplace_back(name);
            return fv::Json(true);
        });
    }
    binder.load(fv::Json::object());
    CHECK(order == std::vector<std::string>{"first", "second", "third"});
    CHECK(binder.bindingCount() == 3);
    order.clear();
    binder.load(fv::Json::object(), 2);
    CHECK(order == std::vector<std::string>{"third"});
}

// from
// tests/test_settings_binder.py::test_plain_text_saves_on_focus_out_and_keeps_the_default_unsaved
TEST_CASE("test_plain_text_saves_on_focus_out_and_keeps_the_default_unsaved",
          "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QPlainTextEdit edit;
    edit.setPlaceholderText(QStringLiteral("The default."));
    binder.plainText(&edit, "notes", valueOr("notes", ""));

    const auto type = [&edit](const QString& text) {
        edit.setPlainText(text);
        edit.document()->setModified(true);
    };

    type(QStringLiteral("  Mine  "));
    CHECK(persist.saved.empty()); // not while typing
    leave(&edit);
    leave(&edit); // nothing new to save
    type(QStringLiteral("The default."));
    leave(&edit);
    CHECK(persist.saved == Saved{{"notes", "Mine"}, {"notes", ""}});

    persist.loading = true;
    binder.load(fv::Json{{"notes", "Saved"}});
    CHECK(edit.toPlainText() == QStringLiteral("Saved"));
}

// from
// tests/test_settings_binder.py::test_plain_text_passing_focus_through_keeps_a_change_made_elsewhere
TEST_CASE("test_plain_text_passing_focus_through_keeps_a_change_made_elsewhere",
          "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QPlainTextEdit edit;
    binder.plainText(&edit, "notes", valueOr("notes", ""));
    binder.load(fv::Json{{"notes", "Loaded before the tab changed it"}});

    leave(&edit);

    CHECK(persist.saved.empty());
}

TEST_CASE("line edits save trimmed text when editing finishes", "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    QLineEdit edit;
    binder.lineEdit(&edit, "title", valueOr("title", "Walkthrough"));

    edit.setText(QStringLiteral("  Half"));
    CHECK(persist.saved.empty());
    Q_EMIT edit.editingFinished();
    CHECK(persist.saved == Saved{{"title", "Half"}});

    persist.loading = true;
    binder.load(fv::Json::object());
    CHECK(edit.text() == QStringLiteral("Walkthrough"));
}

TEST_CASE("a destroyed control neither saves nor loads", "[test_settings_binder]") {
    Recorder persist;
    fv::SettingsBinder binder(persist.persist());
    auto check = std::make_unique<QCheckBox>();
    binder.checkbox(check.get(), "flag", valueOr("flag", true));
    check.reset();
    binder.load(fv::Json::object());
    CHECK(persist.saved.empty());
}

namespace {

// The checks a stand-in General and the Meeting Mode pages bind, as _BOUND_CHECKS lists them:
// (control, key, stored value, checked after load).
struct BoundCheck {
    const char* control;
    const char* key;
    fv::Json stored;
    bool checked;
    bool meeting;
};

const std::vector<BoundCheck>& boundChecks() {
    static const std::vector<BoundCheck> checks{
        {"auto_paste_check", "auto_paste", false, false, false},
        {"restore_clipboard_check", "restore_clipboard", false, false, false},
        {"copy_clipboard_check", "copy_clipboard", false, false, false},
        {"update_notify_check", "update_notify_enabled", false, false, false},
        {"update_beta_check", "update_beta_track_enabled", true, true, false},
        {"meeting_past_recall_check", "meeting_past_recall_enabled", true, true, true},
        {"meeting_context_folder_check", "meeting_context_folder_enabled", true, true, true},
        // Not a real true/false: the page shows the default (on).
        {"typesafe_topic_shift_check", "typesafe_topic_shift_enabled", "yes", true, true},
        {"meeting_end_redecode_check", "meeting_end_redecode", true, true, true},
        {"meeting_end_polish_check", "meeting_end_polish", true, true, true},
    };
    return checks;
}

// A stand-in page whose switches, review combo and port spin are bound through the dialog's
// binders, as General's and Meeting Mode's are.
class BoundPage : public QWidget, public fv::SettingsPageModule {
public:
    BoundPage(fv::SettingsDialog& dialog, bool meeting, QWidget* parent) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        fv::SettingsBinder& binder = meeting ? dialog.meetingBindings() : dialog.bindings();
        for (const BoundCheck& bound : boundChecks()) {
            if (bound.meeting != meeting) {
                continue;
            }
            auto* tile = new fv::SettingTile(QString::fromUtf8(bound.key), QString());
            layout->addWidget(tile);
            controls_[bound.control] = tile->checkbox();
            const bool fallback = bound.stored.is_string() ? true : !bound.checked;
            binder.checkbox(tile->checkbox(), bound.key,
                            [key = std::string(bound.key), fallback](const fv::Json& settings) {
                                if (!settings.contains(key)) {
                                    return fv::Json(fallback);
                                }
                                // The resolvers read anything but a real bool as the default.
                                return settings[key].is_boolean() ? settings[key]
                                                                  : fv::Json(fallback);
                            });
        }
        if (meeting) {
            auto* combo = new QComboBox;
            combo->addItem(QStringLiteral("Normal"), QStringLiteral("normal"));
            combo->addItem(QStringLiteral("Thorough"), QStringLiteral("thorough"));
            layout->addWidget(combo);
            controls_["meeting_review_sensitivity"] = combo;
            binder.combo(combo, "meeting_insight_review_sensitivity",
                         valueOr("meeting_insight_review_sensitivity", "normal"));
            auto* spin = new QSpinBox;
            spin->setMaximum(65535);
            layout->addWidget(spin);
            controls_["meeting_port_spinbox"] = spin;
            binder.spin(spin, "meeting_server_port", valueOr("meeting_server_port", 0));
        }
    }

    QWidget* namedControl(std::string_view name) override {
        const auto found = controls_.find(std::string(name));
        return found == controls_.end() ? nullptr : found->second;
    }

private:
    std::map<std::string, QWidget*> controls_;
};

fv::Json storedSettings() {
    fv::Json stored = fv::Json::object();
    for (const BoundCheck& bound : boundChecks()) {
        stored[bound.key] = bound.stored;
    }
    stored["meeting_insight_review_sensitivity"] = "thorough";
    stored["meeting_server_port"] = 8123;
    return stored;
}

// isolated_dialog: Settings over a store holding storedSettings(), with General and the
// meeting dashboard bound.
struct IsolatedDialog {
    IsolatedDialog() : rig(storedSettings()) {
        rig.registerStandIns([](fv::SettingsPageSpec& spec) {
            const bool meeting = spec.destination == fv::destinations::meeting_dashboard;
            if (spec.destination == fv::destinations::general || meeting) {
                spec.create = [meeting](QWidget* parent) -> QWidget* {
                    auto* page = qobject_cast<fv::SettingsPage*>(parent);
                    return new BoundPage(*page->dialog(), meeting, parent);
                };
                for (const BoundCheck& bound : boundChecks()) {
                    if (bound.meeting == meeting) {
                        spec.controls.emplace_back(bound.control);
                    }
                }
                if (meeting) {
                    spec.controls.emplace_back("meeting_review_sensitivity");
                    spec.controls.emplace_back("meeting_port_spinbox");
                }
            }
        });
        dialog = rig.dialog();
    }

    QAbstractButton* check(const char* name) {
        return qobject_cast<QAbstractButton*>(dialog->control(name));
    }

    SettingsRig rig;
    std::unique_ptr<fv::SettingsDialog> dialog;
};

} // namespace

// from tests/test_settings_binder.py::test_bound_controls_load_without_saving
TEST_CASE("test_bound_controls_load_without_saving", "[test_settings_binder][widget]") {
    IsolatedDialog isolated;
    const std::size_t writes = isolated.rig.settings().file().writes();
    for (const BoundCheck& bound : boundChecks()) {
        INFO(bound.control);
        QAbstractButton* check = isolated.check(bound.control);
        REQUIRE(check != nullptr);
        CHECK(check->isChecked() == bound.checked);
    }
    auto* combo = qobject_cast<QComboBox*>(isolated.dialog->control("meeting_review_sensitivity"));
    REQUIRE(combo != nullptr);
    CHECK(combo->currentData().toString() == QStringLiteral("thorough"));
    auto* spin = qobject_cast<QSpinBox*>(isolated.dialog->control("meeting_port_spinbox"));
    REQUIRE(spin != nullptr);
    CHECK(spin->value() == 8123);
    CHECK(isolated.rig.settings().contents() == storedSettings());
    CHECK(isolated.rig.settings().file().writes() == writes);
}

// from tests/test_settings_binder.py::test_bound_checkbox_saves_its_key_immediately
TEST_CASE("test_bound_checkbox_saves_its_key_immediately", "[test_settings_binder][widget]") {
    IsolatedDialog isolated;
    for (const BoundCheck& bound : boundChecks()) {
        INFO(bound.key);
        // Reset both persistence and the control before every case so a preceding toggle cannot
        // satisfy the next assertion.
        isolated.rig.settings().set_contents(storedSettings());
        QAbstractButton* check = isolated.check(bound.control);
        REQUIRE(check != nullptr);
        {
            const QSignalBlocker blocker(check);
            check->setChecked(bound.checked);
        }
        check->setChecked(!bound.checked);
        CHECK(check->isChecked() != bound.checked);
        fv::Json expected = storedSettings();
        expected[bound.key] = !bound.checked;
        CHECK(isolated.rig.settings().contents() == expected);
    }
}

// from tests/test_settings_binder.py::test_bound_combo_and_spin_save_their_keys_immediately
TEST_CASE("test_bound_combo_and_spin_save_their_keys_immediately",
          "[test_settings_binder][widget]") {
    IsolatedDialog isolated;
    auto* combo = qobject_cast<QComboBox*>(isolated.dialog->control("meeting_review_sensitivity"));
    auto* spin = qobject_cast<QSpinBox*>(isolated.dialog->control("meeting_port_spinbox"));
    REQUIRE(combo != nullptr);
    REQUIRE(spin != nullptr);
    combo->setCurrentIndex(combo->findData(QStringLiteral("normal")));
    spin->setValue(9000);
    const fv::Json saved = isolated.rig.settings().contents();
    CHECK(saved["meeting_insight_review_sensitivity"] == "normal");
    CHECK(saved["meeting_server_port"] == 9000);
}
