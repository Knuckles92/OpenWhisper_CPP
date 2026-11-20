// Apps & styles, Profiles and Commands in the Personalize feature: what it registers, "<feature>
// connects every signal" (architecture 8.5) for their classes, and the Command Mode readiness
// check that runs off the GUI thread.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <QApplication>
#include <QMetaMethod>
#include <QPushButton>

#include "base/testing/manual_executor.h"
#include "styles_commands_support.h"
#include "ui_personalize/fit_row.h"
#include "ui_personalize/removable_rows.h"
#include "ui_settings/home_rows.h"

using fv::Json;
using namespace fvtest;

namespace {

struct SignalProbe : QObject {
    using QObject::isSignalConnected;
};

bool connected(const QObject& object, const QMetaMethod& signal) {
    return (object.*(&SignalProbe::isSignalConnected))(signal);
}

int checkEverySignalConnected(const QObject& object, const QMetaObject& own) {
    int declared = 0;
    for (int index = own.methodOffset(); index < own.methodCount(); ++index) {
        const QMetaMethod method = own.method(index);
        if (method.methodType() != QMetaMethod::Signal) {
            continue;
        }
        ++declared;
        INFO(own.className() << "::" << method.name().toStdString());
        CHECK(connected(object, method));
    }
    return declared;
}

int declaredSignals(const QMetaObject& meta) {
    int count = 0;
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        if (meta.method(index).methodType() == QMetaMethod::Signal) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("the feature registers Apps & styles, Profiles and Commands with their rail values",
          "[widget]") {
    PersonalizeRig rig({{"transcript_cleanup_enabled", true}});
    for (const auto key : {dest::styles, dest::cleanup_profiles, dest::commands}) {
        CHECK(rig.context().settingsPages().page(key) != nullptr);
    }
    rig.window();
    CHECK(rig.railValue(dest::styles) == QStringLiteral("On · All formal"));
    CHECK(rig.railValue(dest::cleanup_profiles) == QStringLiteral("2 profiles"));
    CHECK(rig.railValue(dest::commands) == QStringLiteral("No shortcut · 4 transforms"));
    CHECK(rig.window().pageTitle(dest::cleanup_profiles) == QStringLiteral("Profiles"));
    CHECK(rig.window().pageSubtitle(dest::commands) ==
          QStringLiteral("Rewrite selected text by voice, and keep the rewrites you use often."));
    QStringList titles;
    for (const auto* label :
         rig.home().findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowTitle"))) {
        titles << label->text();
    }
    const auto at = [&titles](const char* title) {
        return titles.indexOf(QString::fromUtf8(title));
    };
    CHECK(at("Snippets") >= 0);
    CHECK(at("Snippets") < at("Match tone to each app"));
    CHECK(at("Match tone to each app") < at("Command Mode shortcut"));
}

TEST_CASE("PersonalizeFeature connects every signal of Apps & styles, Profiles and Commands",
          "[widget]") {
    PersonalizeRig rig;
    fv::CommandsPage& commands = rig.commands();
    CHECK(checkEverySignalConnected(*commands.transformsPanel(),
                                    fv::TransformsPanel::staticMetaObject) == 2);
    CHECK(checkEverySignalConnected(rig.profilesPanel(),
                                    fv::CleanupProfilesPanel::staticMetaObject) == 3);
    for (const QMetaObject* meta :
         {&fv::StylesPage::staticMetaObject, &fv::CommandsPage::staticMetaObject,
          &fv::ProfilesPage::staticMetaObject, &fv::CommandShortcutField::staticMetaObject,
          &fv::FitRow::staticMetaObject, &fv::RemovableRows::staticMetaObject}) {
        INFO(meta->className());
        CHECK(declaredSignals(*meta) == 0);
    }
}

TEST_CASE("readiness answers unknown without a check, and the latest ask wins", "[widget]") {
    QObject owner;
    fv::CommandReadiness::Receiver receiver(&owner);
    std::vector<std::optional<bool>> answers;
    const auto record = [&answers](std::optional<bool> answer) {
        answers.push_back(answer);
    };

    fv::CommandReadiness none;
    none.ask(Json::object(), receiver, record);
    CHECK(answers.empty());
    QApplication::processEvents();
    CHECK(answers == std::vector<std::optional<bool>>{std::nullopt});

    answers.clear();
    fv::ManualExecutor worker;
    std::vector<Json> asked;
    fv::CommandReadiness check(
        [&asked](const Json& settings) -> std::optional<bool> {
            asked.push_back(settings);
            return settings.value("ready", false);
        },
        &worker);
    check.ask(Json{{"ready", false}}, receiver, record);
    check.ask(Json{{"ready", true}}, receiver, record);
    // Nothing runs on the GUI thread until the worker does.
    CHECK(asked.empty());
    worker.run_all();
    CHECK(asked.size() == 2);
    QApplication::processEvents();
    CHECK(answers == std::vector<std::optional<bool>>{true});
}

TEST_CASE("a readiness answer for a receiver that is gone is dropped", "[widget]") {
    fv::ManualExecutor worker;
    fv::CommandReadiness check([](const Json&) -> std::optional<bool> { return false; }, &worker);
    int answered = 0;
    {
        QObject owner;
        fv::CommandReadiness::Receiver receiver(&owner);
        check.ask(Json::object(), receiver, [&answered](std::optional<bool>) { ++answered; });
    }
    worker.run_all();
    QApplication::processEvents();
    CHECK(answered == 0);
}

TEST_CASE("a check that throws answers unknown", "[widget]") {
    fv::ManualExecutor worker;
    fv::CommandReadiness check(
        [](const Json&) -> std::optional<bool> { throw std::runtime_error("store locked"); },
        &worker);
    QObject owner;
    fv::CommandReadiness::Receiver receiver(&owner);
    std::vector<std::optional<bool>> answers;
    check.ask(Json::object(), receiver,
              [&answers](std::optional<bool> answer) { answers.push_back(answer); });
    worker.run_all();
    QApplication::processEvents();
    CHECK(answers == std::vector<std::optional<bool>>{std::nullopt});
}

TEST_CASE("the add row stacks below what its controls need side by side", "[widget]") {
    QWidget holder;
    auto* first = new QPushButton(QStringLiteral("Picker"));
    auto* second = new QPushButton(QStringLiteral("Add"));
    auto* row = new fv::FitRow({first, second}, &holder);
    CHECK(row->objectName() == QStringLiteral("appStylesAddRow"));
    const int needed = std::max(first->minimumSizeHint().width(), fv::FitRow::kFirstMinWidth) +
                       row->line()->spacing() + second->minimumSizeHint().width();
    row->resize(needed, 80);
    holder.show();
    QApplication::processEvents();
    CHECK_FALSE(row->isStacked());
    row->resize(needed - 1, 80);
    QApplication::processEvents();
    CHECK(row->isStacked());
    CHECK(row->minimumSizeHint().width() ==
          std::max(first->minimumSizeHint().width(), second->minimumSizeHint().width()));
    holder.hide();
}
