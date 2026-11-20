#pragma once

// The Dictionary and Snippets pages inside a real Settings window over fakes (OpenWhisper's
// tests/fakes/personalize.settings_dialogs): an in-memory settings file, Home, the Personalize
// feature provided and installed as the app does it, and a stand-in for every other destination.
// `changes` records what the window said changed (the on_settings_changed mock).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QStringList>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include "personalize_support.h"
#include "text/dictionary.h"
#include "text/snippets.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/dictionary_library.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/personalize_feature.h"
#include "ui_personalize/snippets_page.h"
#include "ui_personalize/snippets_panel.h"
#include "ui_settings/home_page.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/wrapped_label.h"

namespace fvtest {

// Deferred deletes are what clear a destroyed button's focus; plain processEvents() never runs
// them outside an event loop.
inline void flushDeletes() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// A dictionary word as the tests save it.
inline fv::Json word(const std::string& id, const std::string& term, bool starred = false,
                     std::vector<std::string> heard = {}, bool learned = false,
                     bool fresh = false) {
    return fv::Json{{"id", id},       {"term", term},       {"starred", starred},
                    {"heard", heard}, {"learned", learned}, {"new", fresh}};
}

class PagesRig {
public:
    explicit PagesRig(const fv::Json& saved = fv::Json::object()) : rig_(saved) {
        fv::registerHomePage(rig_.context());
        feature_.provide(rig_.context());
        feature_.install(rig_.context());
        for (const Heading& heading : headings()) {
            if (rig_.registry().page(heading.key) == nullptr) {
                rig_.registry().addPage(standInSpec(heading));
            }
        }
    }
    ~PagesRig() {
        dialogs_.clear();
        fv::DictionaryPage::setBackendProvider({});
        flushDeletes();
    }
    PagesRig(const PagesRig&) = delete;
    PagesRig& operator=(const PagesRig&) = delete;

    fv::SettingsDialog& dialog() {
        if (dialogs_.empty()) {
            auto made = rig_.dialog();
            QObject::connect(made.get(), &fv::SettingsDialog::settingsChanged,
                             [this](const QString& kind) { changes << kind; });
            dialogs_.push_back(std::move(made));
        }
        return *dialogs_.back();
    }
    // The window shown, active and at the size OpenWhisper's Enter-key tests give it.
    fv::SettingsDialog& shownDialog() {
        fv::SettingsDialog& made = dialog();
        made.resize(1100, 820);
        made.show();
        made.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&made));
        return made;
    }

    template <class Page> Page& page(std::string_view key) {
        dialog().selectDestination(qs(key));
        auto* content = dynamic_cast<Page*>(dialog().pageContent(key));
        REQUIRE(content != nullptr);
        return *content;
    }
    fv::DictionaryPage& dictionary() { return page<fv::DictionaryPage>(dest::dictionary); }
    // The panel never puts a real message box up: until a test answers, every question is
    // recorded in `questions` and answered Cancel.
    fv::SnippetsPage& snippetsPage() {
        fv::SnippetsPage& made = page<fv::SnippetsPage>(dest::snippets);
        if (guarded_ != made.panel()) {
            guarded_ = made.panel();
            guarded_->question = [this](QWidget*, const QString& title, const QString& text,
                                        QMessageBox::StandardButtons, QMessageBox::StandardButton) {
                questions << title + QStringLiteral(": ") + text;
                return QMessageBox::Cancel;
            };
        }
        return made;
    }
    fv::SnippetsPanel& snippets() { return *snippetsPage().panel(); }
    QStringList questions;
    fv::HomePage& home() {
        auto* content = dynamic_cast<fv::HomePage*>(dialog().pageContent(dest::overview));
        REQUIRE(content != nullptr);
        return *content;
    }
    QString railValue(std::string_view key) { return dialog().rail()->value(qs(key)); }

    fv::CleanupGates& gates() { return rig_.context().service<fv::CleanupGates>(); }
    fv::MemorySettingsStore& settings() { return rig_.settings(); }
    fv::AppContext& context() { return rig_.context(); }
    fv::Json saved(std::string_view key) {
        const auto snapshot = rig_.settings().snapshot();
        const auto found = snapshot->find(key);
        return found == snapshot->end() ? fv::Json() : *found;
    }
    std::vector<fv::text::DictionaryTerm> words() {
        return fv::text::load_dictionary(*rig_.settings().snapshot());
    }
    std::vector<std::string> terms() {
        std::vector<std::string> found;
        for (const auto& term : words()) {
            found.push_back(term.term);
        }
        return found;
    }
    std::vector<fv::text::Snippet> savedSnippets() {
        return fv::text::load_snippets(*rig_.settings().snapshot());
    }
    // The texts of Home's row descriptions.
    QStringList homeDetails() {
        QStringList texts;
        for (const auto* label :
             home().findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowDescription"))) {
            texts << label->text();
        }
        return texts;
    }

    QStringList changes;

private:
    SettingsRig rig_;
    fv::PersonalizeFeature feature_;
    std::vector<std::unique_ptr<fv::SettingsDialog>> dialogs_;
    fv::SnippetsPanel* guarded_ = nullptr;
};

} // namespace fvtest
