// The Personalize feature with the Dictionary and Snippets pages: what it provides and
// registers, and "<feature> connects every signal" (architecture 8.5) for the pages' classes.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QMetaMethod>

#include "pages_support.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/dictionary_library.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/snippets_page.h"
#include "ui_personalize/snippets_panel.h"
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

// Every signal `object`'s own class declares (not those of its Qt bases) is connected.
void checkEverySignalConnected(const QObject& object, const QMetaObject& own, int expected) {
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
    INFO(own.className());
    CHECK(declared == expected);
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

TEST_CASE("the feature provides the cleanup gates and registers Dictionary and Snippets",
          "[widget]") {
    PagesRig rig;
    CHECK(rig.context().find<fv::CleanupGates>() != nullptr);
    CHECK(rig.context().settingsPages().page(dest::dictionary) != nullptr);
    CHECK(rig.context().settingsPages().page(dest::snippets) != nullptr);
    // Home's Personalize card gets the Dictionary row, then the Snippets row (then the Apps &
    // styles and Command Mode rows).
    CHECK(rig.context().service<fv::HomeRows>().contributors(fv::HomeCardId::Personalize).size() ==
          4);
    QStringList titles;
    for (const auto* label :
         rig.home().findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowTitle"))) {
        if (label->text() == QStringLiteral("Dictionary") ||
            label->text() == QStringLiteral("Snippets")) {
            titles << label->text();
        }
    }
    CHECK(titles == QStringList{QStringLiteral("Dictionary"), QStringLiteral("Snippets")});
}

TEST_CASE("PersonalizeFeature connects every signal of the Dictionary and Snippets pages",
          "[widget]") {
    PagesRig rig(Json{{"dictation_dictionary", Json::array({word("a", "Ksenia")})}});
    fv::DictionaryPage& dictionary = rig.dictionary();
    fv::DictionaryLibrary& library = *dictionary.library();
    checkEverySignalConnected(library, fv::DictionaryLibrary::staticMetaObject, 3);
    fv::DictionaryRow* row = library.row(QStringLiteral("a"));
    REQUIRE(row != nullptr);
    checkEverySignalConnected(*row, fv::DictionaryRow::staticMetaObject, 3);
    checkEverySignalConnected(rig.snippets(), fv::SnippetsPanel::staticMetaObject, 1);
    for (const QMetaObject* meta :
         {&fv::DictionaryPage::staticMetaObject, &fv::SnippetsPage::staticMetaObject,
          &fv::CleanupGates::staticMetaObject}) {
        INFO(meta->className());
        CHECK(declaredSignals(*meta) == 0);
    }
}
