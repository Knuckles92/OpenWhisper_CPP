// Checklist line WT-77 of docs/port/map/a32-user-facing-features.md: Saved Walkthroughs and the
// Walkthrough tab stay in step. A save in the library reloads the tab (WalkthroughFeature's
// library changed -> reloadWalkthrough), and a save in the tab refreshes an open, idle library
// (ui_controller._refresh_walkthrough_library).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QPushButton>
#include <QTextEdit>

#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/tabbed_content.h"
#include "walkthrough/finish.h"
#include "walkthrough/session.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace wt = fv::walkthrough;
using fv::TabbedContentWidget;

namespace {

fvtest::RigOptions syncRig() {
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    return options;
}

std::filesystem::path finishedWalkthrough(const std::filesystem::path& root) {
    using namespace std::chrono;
    wt::WalkthroughSession::Options options;
    options.started_at = local_seconds(local_days{year{2026} / 10 / 25} + hours{9});
    options.root = root;
    wt::WalkthroughSession session(std::move(options));
    session.add_shot(std::make_shared<wt::FakeShotImage>(120, 80, 0xf84f30), 1.0);
    const auto shots = session.saved_shots();
    const std::vector<std::string> texts{"as said", ""};
    const auto result =
        wt::build_documents(session, shots, texts, texts, 3.0, {}, fv::Json::object());
    session.end();
    return result.folder;
}

QString firstWords(fv::WalkthroughReview* review) {
    const auto rows = review->rows();
    return rows.empty() ? QString() : rows.front()->editor()->toPlainText();
}

} // namespace

TEST_CASE("a save in the tab or the library shows in the other", "[widget]") {
    fvtest::ControllerRig rig(syncRig());
    REQUIRE(rig.startEngine());
    const auto folder = finishedWalkthrough(wt::captures_root(rig.paths()));

    rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    REQUIRE(fvtest::waitUntil([tab, &folder] { return tab->review()->folder() == folder; }));
    auto* sidebar = rig.context().service<fv::HistoryPanel>().sidebar();
    REQUIRE(sidebar);
    auto* past = sidebar->findChild<fv::PastWalkthroughsPanel*>();
    REQUIRE(past);
    past->libraryButton()->click();
    auto* library = rig.window().findChild<fv::WalkthroughLibraryDialog*>();
    REQUIRE(library);
    REQUIRE(library->isVisible());
    REQUIRE(library->review()->folder() == folder);
    CHECK(firstWords(library->review()) == QStringLiteral("as said"));

    // Saved in the tab: the open library shows the new words.
    tab->review()->rows().front()->editor()->setPlainText(QStringLiteral("fixed in the tab"));
    tab->review()->saveButton()->click();
    CHECK(fvtest::waitUntil([library] {
        return !library->review()->isBusy() &&
               firstWords(library->review()) == QStringLiteral("fixed in the tab");
    }));

    // Saved in the library: the tab shows them.
    REQUIRE(fvtest::waitUntil([tab] { return !tab->review()->isBusy(); }));
    library->review()->rows().front()->editor()->setPlainText(
        QStringLiteral("fixed in the library"));
    library->review()->saveButton()->click();
    CHECK(fvtest::waitUntil([tab] {
        return !tab->review()->isBusy() &&
               firstWords(tab->review()) == QStringLiteral("fixed in the library");
    }));
    library->close();
}
