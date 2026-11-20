// Settings > Downloads (tests/test_settings_downloads.py): the catalog, its filters, sorts and
// actions, the inspector, batch selection, the components tab and the scan worker. The page is
// built standalone over fakes here; test_downloads_window.cpp covers how Settings hosts it.
//
// OpenWhisper_CPP's catalog is the Parakeet family: Orukeet stands in for Python's "tiny" (the row
// the recommended order puts first), Parakeet TDT 0.6B v3 for "base" (the dictation model), and the
// two NVIDIA Speech runtimes for the components a Windows computer lists.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "base/json.h"
#include "downloads_page_support.h"
#include "history/format_utils.h"
#include "models/component_details.h"
#include "models/downloads_model.h"
#include "models/model_details.h"
#include "models/speech_catalog.h"
#include "ui_downloads/batch_download_dialog.h"
#include "ui_downloads/component_details_dialog.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/file_reveal.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/wrapped_label.h"

using fv::DownloadsPage;
using fv::Json;
using fv::ModelRowWidget;
using fv::models::ComponentState;
using fvtest::DownloadsRig;
using fvtest::kOrukeet;
using fvtest::kOrukeetBytes;
using fvtest::kOrukeetRepo;
using fvtest::kParakeet;
using fvtest::kParakeetBytes;
using fvtest::kParakeetRepo;
using fvtest::pick;

namespace {

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

bool visibleTo(const QWidget* widget, const QWidget* ancestor) {
    return widget->isVisibleTo(ancestor);
}

ModelRowWidget* row(DownloadsPage& page, const std::string& name) {
    ModelRowWidget* found = page.row(q(name));
    REQUIRE(found != nullptr);
    return found;
}

// The settings that make `model` the dictation model, and `meeting` nothing.
Json dictating(const std::string& model) {
    return Json{{"selected_model", "parakeet"},
                {"local_asr_models", Json::object({{"parakeet", model}})}};
}

// The badge text a row shows (DownloadRow::badge).
QString badgeOf(ModelRowWidget* each) {
    return each->badge()->text();
}

// What the theme declares for one object name: its rule body after `selector`.
QString ruleAfter(const QString& sheet, const QString& selector) {
    const qsizetype at = sheet.indexOf(selector);
    if (at < 0) {
        return {};
    }
    const QString rest = sheet.mid(at + selector.size());
    return rest.left(rest.indexOf(QLatin1Char('}')));
}

// Records what a page's link and folder buttons would open.
class OpenedUrls {
public:
    OpenedUrls() {
        fv::FileRevealHooks hooks;
        hooks.openUrl = [this](const QUrl& url) {
            urls.push_back(url.toString());
            return true;
        };
        hooks.startDetached = [](const QString&) {
            return true;
        };
        previous_ = fv::setFileRevealHooks(hooks);
    }
    ~OpenedUrls() { fv::setFileRevealHooks(previous_); }
    OpenedUrls(const OpenedUrls&) = delete;
    OpenedUrls& operator=(const OpenedUrls&) = delete;

    std::vector<QString> urls;

private:
    fv::FileRevealHooks previous_;
};

} // namespace

// ---- TestPageShell: the page fills Settings' body; only the two content columns scroll ----

// from
// tests/test_settings_downloads.py::TestPageShell::test_rows_are_not_clipped_at_the_settings_minimum_width
TEST_CASE("rows are not clipped at the settings minimum width",
          "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    page->resize(fv::SettingsDialog::kMinimumSize.width() - 258 - 56, 480);
    QCoreApplication::processEvents();

    QWidget* container = page->libraryScrollArea()->widget();
    CHECK(container->minimumSizeHint().width() <= page->libraryScrollArea()->viewport()->width());
}

// from
// tests/test_settings_downloads.py::TestPageShell::test_inspector_docks_when_the_page_is_wide_enough
TEST_CASE("inspector docks when the page is wide enough", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    page->resize(1400, 640);
    QCoreApplication::processEvents();

    CHECK(page->inspectorDocked());
    CHECK(page->inspector()->isVisible());
    CHECK_FALSE(visibleTo(page->inspectorCloseButton(), page.get()));
}

// from
// tests/test_settings_downloads.py::TestPageShell::test_narrow_page_floats_the_inspector_until_a_model_is_picked
TEST_CASE("narrow page floats the inspector until a model is picked",
          "[test_settings_downloads][widget]") {
    // The page sits in a window wider than itself, as in Settings: a window would not shrink below
    // the docked layout's minimum, and with Windows' fonts that minimum is above 640 px (Python's
    // tests ran without fonts, so their page could be resized to 640 on its own).
    QWidget window;
    window.resize(1500, 800);
    DownloadsRig rig;
    auto page = rig.page();
    page->setParent(&window);
    window.show();
    page->show();
    page->resize(640, 560);
    QCoreApplication::processEvents();

    CHECK_FALSE(page->inspectorDocked());
    CHECK_FALSE(page->inspector()->isVisible());

    page->selectModel(q(kOrukeet));
    QCoreApplication::processEvents();
    CHECK(page->inspector()->isVisible());
    CHECK(page->inspector()->property(fv::ui::prop::overlay).toBool());
    CHECK(visibleTo(page->inspectorCloseButton(), page.get()));
    CHECK(page->inspector()->geometry().right() == page->catalogColumn()->geometry().right());
    // The drop shadow (blur 28, offset (-6, 0), alpha 110).
    CHECK(page->inspector()->graphicsEffect() != nullptr);

    page->inspectorCloseButton()->click();
    CHECK_FALSE(page->inspector()->isVisible());
}

// from tests/test_settings_downloads.py::TestPageShell::test_widening_again_docks_the_inspector
TEST_CASE("widening again docks the inspector", "[test_settings_downloads][widget]") {
    QWidget window;
    window.resize(1500, 800);
    DownloadsRig rig;
    auto page = rig.page();
    page->setParent(&window);
    window.show();
    page->show();
    page->resize(640, 560);
    QCoreApplication::processEvents();
    CHECK_FALSE(page->inspectorDocked());
    page->resize(1400, 640);
    QCoreApplication::processEvents();

    CHECK(page->inspectorDocked());
    CHECK(page->inspector()->isVisible());
    CHECK(page->inspector()->property(fv::ui::prop::overlay).toBool() == false);
    CHECK(page->inspector()->property(fv::ui::prop::overlay).isValid());
    CHECK(page->inspector()->graphicsEffect() == nullptr);
}

// from tests/test_settings_downloads.py::TestPageShell::test_only_the_designated_regions_scroll
TEST_CASE("only the designated regions scroll", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    const QList<QScrollArea*> scrollers = page->findChildren<QScrollArea*>();
    const std::set<QScrollArea*> found(scrollers.begin(), scrollers.end());
    CHECK(found == std::set<QScrollArea*>{page->libraryScrollArea(), page->inspectorDetailScroll(),
                                          page->componentsScroll()});
    CHECK(page->libraryScrollArea()->widgetResizable());
    CHECK(page->inspectorDetailScroll()->widgetResizable());
    CHECK(page->componentsScroll()->widgetResizable());
}

// from
// tests/test_settings_downloads.py::TestPageShell::test_rows_share_the_right_edge_with_the_toolbar
TEST_CASE("rows share the right edge with the toolbar", "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    // OpenWhisper's thirty rows scroll at 560 px; OpenWhisper_CPP's two need a shorter list. With
    // Windows' fonts the themed toolbar's minimum is wider than the catalog column of an 806 px
    // page (Python's tests ran without fonts), so the page is as wide as Settings' default body.
    page->resize(fv::SettingsDialog::kDefaultSize.width() - 258 - 56, 300);
    QCoreApplication::processEvents();
    // The scroll bar appears during the first pass; the toolbar's new margin is laid out in the
    // next.
    QCoreApplication::processEvents();

    const auto rightEdge = [&page](QWidget* widget) {
        return widget->mapTo(page.get(), QPoint(widget->width(), 0)).x();
    };
    REQUIRE(page->libraryScrollArea()->verticalScrollBar()->isVisible());
    INFO("margin " << page->toolbarLayout()->contentsMargins().right() << " area "
                   << page->libraryScrollArea()->width() << " viewport "
                   << page->libraryScrollArea()->viewport()->width() << " row "
                   << rightEdge(row(*page, kOrukeet)) << " sort " << rightEdge(page->sortCombo())
                   << " column " << rightEdge(page->catalogColumn()) << " page " << page->width()
                   << " toolbar min " << page->toolbarLayout()->minimumSize().width() << " toolbar "
                   << page->toolbarLayout()->geometry().width());
    CHECK(std::abs(rightEdge(row(*page, kOrukeet)) - rightEdge(page->sortCombo())) <= 1);
}

// from
// tests/test_settings_downloads.py::TestPageShell::test_catalog_column_and_selection_bar_have_transparent_surface
TEST_CASE("catalog column and selection bar have transparent surface",
          "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();

    auto* bar = page->findChild<QWidget*>(QStringLiteral("downloadsSelectionBar"));
    auto* column = page->findChild<QWidget*>(QStringLiteral("downloadsCatalogColumn"));
    REQUIRE(bar != nullptr);
    REQUIRE(column != nullptr);
    CHECK_FALSE(bar->autoFillBackground());
    CHECK_FALSE(column->autoFillBackground());
    const QString sheet = fv::ui::ThemeManager().stylesheet();
    CHECK(sheet.contains(QStringLiteral("QWidget#downloadsCatalogColumn,\n"
                                        "QWidget#downloadsSelectionBar,")));
    CHECK(ruleAfter(sheet, QStringLiteral("QWidget#downloadsCatalogColumn"))
              .contains(QStringLiteral("background-color: transparent")));
}

// from tests/test_settings_downloads.py::TestPageShell::test_open_folder_button_fits_its_label
TEST_CASE("open folder button fits its label", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    fv::Button* openFolder = nullptr;
    for (fv::Button* button : page->findChildren<fv::Button*>()) {
        if (button->text() == QStringLiteral("Open folder")) {
            openFolder = button;
        }
    }
    REQUIRE(openFolder != nullptr);
    openFolder->ensurePolished();
    CHECK(openFolder->minimumWidth() >= openFolder->sizeHint().width());
    CHECK(openFolder->minimumWidth() == openFolder->maximumWidth());
    CHECK(openFolder->toolTip() == "Open the folder where downloaded models are stored");
}

// ---- TestModelRows: per-row status, size and action availability ----

// from tests/test_settings_downloads.py::TestModelRows::test_catalog_excludes_auto
TEST_CASE("catalog excludes auto", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    CHECK(page->row(QStringLiteral("auto")) == nullptr);
    CHECK(page->row(q(kParakeet)) != nullptr);
    QStringList names;
    for (const auto& [name, each] : page->rows()) {
        names.append(name);
    }
    CHECK(names == QStringList{q(kParakeet), q(kOrukeet)});
}

// from
// tests/test_settings_downloads.py::TestModelRows::test_uncached_row_offers_download_with_estimate
TEST_CASE("uncached row offers download with estimate", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    ModelRowWidget* each = row(*page, kOrukeet);
    CHECK(visibleTo(each->downloadButton(), page.get()));
    CHECK_FALSE(visibleTo(each->deleteButton(), page.get()));
    CHECK(badgeOf(each) == "Not downloaded");
    // The Download button already says so; the chip would only repeat it.
    CHECK_FALSE(visibleTo(each->badge(), page.get()));
    CHECK(each->sizeLabel()->text() == "~714 MB");
}

// from tests/test_settings_downloads.py::TestModelRows::test_cached_row_shows_real_size_and_delete
TEST_CASE("cached row shows real size and delete", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kOrukeetRepo, 76'000'000);
    auto page = rig.page();
    ModelRowWidget* each = row(*page, kOrukeet);
    CHECK_FALSE(visibleTo(each->downloadButton(), page.get()));
    CHECK(each->deleteButton()->isEnabled());
    CHECK(badgeOf(each) == "Downloaded");
    CHECK(visibleTo(each->badge(), page.get()));
    CHECK(each->sizeLabel()->text() == "76 MB");
}

// from tests/test_settings_downloads.py::TestModelRows::test_assignment_is_reported_but_not_offered
TEST_CASE("assignment is reported but not offered", "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kParakeet));
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    ModelRowWidget* each = row(*page, kParakeet);
    CHECK_FALSE(visibleTo(each->setActiveButton(), page.get()));
    CHECK(each->usageLabel()->text() == "Dictation");
    CHECK(visibleTo(each->usageLabel(), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kOrukeet)->usageLabel(), page.get()));
}

// from tests/test_settings_downloads.py::TestModelRows::test_usage_chip_combines_both_modes
TEST_CASE("usage chip combines both modes", "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kParakeet));
    rig.meeting = kParakeet;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    CHECK(row(*page, kParakeet)->usageLabel()->text() == "Dictation · Meetings");
}

// from tests/test_settings_downloads.py::TestModelRows::test_loaded_model_delete_is_disabled
TEST_CASE("loaded model delete is disabled", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.loaded = kParakeet;
    auto page = rig.page();
    ModelRowWidget* each = row(*page, kParakeet);
    CHECK_FALSE(each->deleteButton()->isEnabled());
    CHECK(each->deleteButton()->toolTip().contains("In use"));
}

// from
// tests/test_settings_downloads.py::TestModelRows::test_refresh_moves_delete_lock_when_loaded_model_changes
TEST_CASE("refresh moves delete lock when loaded model changes",
          "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kOrukeet));
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.cache(kOrukeetRepo, kOrukeetBytes);
    rig.loaded = kParakeet;
    auto page = rig.page();
    page->refresh();
    CHECK_FALSE(row(*page, kParakeet)->deleteButton()->isEnabled());
    CHECK(row(*page, kOrukeet)->deleteButton()->isEnabled());

    rig.loaded = kOrukeet;
    page->refresh();
    CHECK(row(*page, kParakeet)->deleteButton()->isEnabled());
    CHECK_FALSE(row(*page, kOrukeet)->deleteButton()->isEnabled());
}

// from
// tests/test_settings_downloads.py::TestModelRows::test_header_reports_count_disk_usage_and_folder
TEST_CASE("header reports count disk usage and folder", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.cache(kOrukeetRepo, kOrukeetBytes);
    auto page = rig.page();
    const QString text = page->statsLabel()->text();
    CHECK(text.startsWith(QStringLiteral("2 of %1 speech models").arg(page->rows().size())));
    CHECK(text.contains(q(fv::format_size_bytes(kParakeetBytes + kOrukeetBytes) + " used")));
    CHECK(text.endsWith(fvtest::kModelsFolder));
}

// ---- TestDownloadingState: the badge, and one download at a time ----

// from
// tests/test_settings_downloads.py::TestDownloadingState::test_downloading_row_and_other_downloads_blocked
TEST_CASE("downloading row and other downloads blocked", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->setDownloading(q(kOrukeet));

    CHECK(badgeOf(row(*page, kOrukeet)) == "Downloading…");
    CHECK_FALSE(row(*page, kOrukeet)->downloadButton()->isEnabled());
    CHECK_FALSE(row(*page, kParakeet)->downloadButton()->isEnabled());

    page->finishDownload(q(kOrukeet), true);
    CHECK(row(*page, kParakeet)->downloadButton()->isEnabled());
}

// from
// tests/test_settings_downloads.py::TestDownloadingState::test_failed_download_reports_in_message
TEST_CASE("failed download reports in message", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->setDownloading(q(kOrukeet));
    page->finishDownload(q(kOrukeet), false);
    CHECK(page->messageLabel()->text().contains("failed"));
    CHECK(page->messageLabel()->text() == "Download of \"orukeet-v0.1\" failed");
}

// from
// tests/test_settings_downloads.py::TestDownloadingState::test_download_progress_updates_row_and_footer
TEST_CASE("download progress updates row and footer", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->setDownloading(q(kOrukeet));
    page->setDownloadProgress(q(kOrukeet), 40'000'000, 80'000'000);

    ModelRowWidget* each = row(*page, kOrukeet);
    CHECK(visibleTo(each->progressBar(), page.get()));
    CHECK(each->progressBar()->value() == 50);
    CHECK(each->sizeLabel()->text().contains("of"));
    CHECK(page->messageLabel()->text().contains(q(kOrukeet)));
}

// ---- TestEnvBlocked: HF_HUB_OFFLINE disables downloads but not deletion ----

// from tests/test_settings_downloads.py::TestEnvBlocked::test_banner_shown_and_downloads_disabled
TEST_CASE("banner shown and downloads disabled", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.blockDownloads();
    auto page = rig.page();
    CHECK(visibleTo(page->envBanner(), page.get()));
    CHECK(page->envBanner()->text() == "Downloads are disabled by the HF_HUB_OFFLINE environment "
                                       "variable set outside this application.");
    CHECK_FALSE(row(*page, kOrukeet)->downloadButton()->isEnabled());
    CHECK(row(*page, kOrukeet)->downloadButton()->toolTip() ==
          "Downloads are disabled by HF_HUB_OFFLINE");
    CHECK(row(*page, kParakeet)->deleteButton()->isEnabled());
}

// ---- TestFilter: search and status filters hide rows and show the empty state ----

// from tests/test_settings_downloads.py::TestFilter::test_filter_matches_name_and_repo
TEST_CASE("filter matches name and repo", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->filterEdit()->setText(QStringLiteral("oruk"));
    CHECK(visibleTo(row(*page, kOrukeet), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kParakeet), page.get()));
    // "nvidia" is only in Parakeet's repository id.
    page->filterEdit()->setText(QStringLiteral("nvidia"));
    CHECK(visibleTo(row(*page, kParakeet), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kOrukeet), page.get()));
}

// from tests/test_settings_downloads.py::TestFilter::test_no_match_shows_empty_state
TEST_CASE("no match shows empty state", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->filterEdit()->setText(QStringLiteral("no-such-model"));
    CHECK(visibleTo(page->emptyLabel(), page.get()));
    CHECK(page->emptyLabel()->text() == "No models match");
    page->filterEdit()->setText(QString());
    CHECK_FALSE(visibleTo(page->emptyLabel(), page.get()));
}

// from tests/test_settings_downloads.py::TestFilter::test_status_filter_shows_matching_rows
TEST_CASE("status filter shows matching rows", "[test_settings_downloads][widget]") {
    struct Case {
        const char* status;
        bool parakeetShown;
        bool orukeetShown;
    };
    for (const Case& each : {Case{"downloaded", true, false}, Case{"not_downloaded", false, true},
                             Case{"all", true, true}}) {
        DownloadsRig rig;
        rig.cache(kParakeetRepo, kParakeetBytes);
        auto page = rig.page();
        pick(page->statusFilterCombo(), each.status);
        INFO(each.status);
        CHECK(visibleTo(row(*page, kParakeet), page.get()) == each.parakeetShown);
        CHECK(visibleTo(row(*page, kOrukeet), page.get()) == each.orukeetShown);
    }
}

// from tests/test_settings_downloads.py::TestFilter::test_status_filter_combines_with_search
TEST_CASE("status filter combines with search", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.cache(kOrukeetRepo, kOrukeetBytes);
    auto page = rig.page();
    pick(page->statusFilterCombo(), "downloaded");
    page->filterEdit()->setText(QStringLiteral("oruk"));
    CHECK(visibleTo(row(*page, kOrukeet), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kParakeet), page.get()));
    pick(page->statusFilterCombo(), "not_downloaded");
    CHECK_FALSE(visibleTo(row(*page, kOrukeet), page.get()));
}

// from tests/test_settings_downloads.py::TestFilter::test_backend_filter_shows_one_family
TEST_CASE("backend filter shows one family", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    pick(page->backendFilterCombo(), "parakeet");
    CHECK(page->backendFilterCombo()->currentData().toString() == "parakeet");
    // Both catalog models are Parakeet's.
    CHECK(visibleTo(row(*page, kParakeet), page.get()));
    CHECK(visibleTo(row(*page, kOrukeet), page.get()));
    CHECK_FALSE(visibleTo(page->emptyLabel(), page.get()));
}

// from tests/test_settings_downloads.py::TestFilter::test_backend_filter_lists_every_backend_once
TEST_CASE("backend filter lists every backend once", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    fv::ElidingComboBox* combo = page->backendFilterCombo();
    QStringList ids;
    for (int i = 0; i < combo->count(); ++i) {
        ids.append(combo->itemData(i).toString());
    }
    CHECK(ids == QStringList{QStringLiteral("all"), QStringLiteral("parakeet")});
    CHECK(combo->itemText(0) == "All backends");
    CHECK(combo->itemText(1) == "Parakeet");
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == "Parakeet");
    CHECK(combo->currentData().toString() == "all");
    CHECK(combo->toolTip() == "Show one speech backend's models");
}

// from
// tests/test_settings_downloads.py::TestFilter::test_backend_filter_combines_with_status_and_search
TEST_CASE("backend filter combines with status and search", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    pick(page->backendFilterCombo(), "parakeet");
    pick(page->statusFilterCombo(), "not_downloaded");
    CHECK(visibleTo(row(*page, kOrukeet), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kParakeet), page.get()));
    page->filterEdit()->setText(QStringLiteral("parakeet"));
    CHECK_FALSE(visibleTo(row(*page, kParakeet), page.get()));
    page->filterEdit()->setText(QStringLiteral("oruk"));
    CHECK(visibleTo(row(*page, kOrukeet), page.get()));
    pick(page->statusFilterCombo(), "downloaded");
    CHECK(visibleTo(page->emptyLabel(), page.get()));
}

// from tests/test_settings_downloads.py::TestFilter::test_backend_filter_all_restores_every_row
TEST_CASE("backend filter all restores every row", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    fv::ElidingComboBox* combo = page->backendFilterCombo();
    // A backend no row has hides them all, then All shows them again.
    combo->addItem(QStringLiteral("Other"), QStringLiteral("other"));
    pick(combo, "other");
    CHECK(visibleTo(page->emptyLabel(), page.get()));
    pick(combo, "all");
    for (const auto& [name, each] : page->rows()) {
        CHECK(visibleTo(each, page.get()));
    }
}

// ---- TestSorting: built-in sort choices make common catalog scans one step ----

// from tests/test_settings_downloads.py::TestSorting::test_default_keeps_assigned_model_in_place
TEST_CASE("default keeps assigned model in place", "[test_settings_downloads][widget]") {
    // Parakeet is the dictation model; the recommended order still puts Orukeet first (equal
    // estimates, then the name).
    DownloadsRig rig(dictating(kParakeet));
    auto page = rig.page();
    CHECK(page->sortCombo()->currentData().toString() == "recommended");
    CHECK(page->rowOrder().front() == q(kOrukeet));
}

// from tests/test_settings_downloads.py::TestSorting::test_downloaded_first_groups_cached_models
TEST_CASE("downloaded first groups cached models", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    pick(page->sortCombo(), "downloaded");
    CHECK(page->rowOrder().front() == q(kParakeet));
}

// from tests/test_settings_downloads.py::TestSorting::test_smallest_first_uses_catalog_estimates
TEST_CASE("smallest first uses catalog estimates", "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kParakeet));
    auto page = rig.page();
    pick(page->sortCombo(), "size");
    // Both estimates are 714 MB: the name decides.
    CHECK(row(*page, kOrukeet)->sortSizeBytes() == 714'000'000.0);
    CHECK(row(*page, kParakeet)->sortSizeBytes() == 714'000'000.0);
    CHECK(page->rowOrder().front() == q(kOrukeet));
    // A smaller size on disk wins over an estimate.
    rig.cache(kParakeetRepo, 1'000'000);
    page->refresh();
    CHECK(page->rowOrder().front() == q(kParakeet));
}

// from tests/test_settings_downloads.py::TestSorting::test_name_sort_is_alphabetical
TEST_CASE("name sort is alphabetical", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    pick(page->sortCombo(), "name");
    QStringList order = page->rowOrder();
    QStringList sorted = order;
    std::ranges::sort(sorted, [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    CHECK(order == sorted);
    CHECK(order.front() == q(kOrukeet));
}

// from
// tests/test_settings_downloads.py::TestSorting::test_backend_sort_keeps_each_family_contiguous
TEST_CASE("backend sort keeps each family contiguous", "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kParakeet));
    auto page = rig.page();
    pick(page->sortCombo(), "backend");
    std::vector<std::string> firstSeen;
    for (const QString& name : page->rowOrder()) {
        const std::string backend(fv::models::backend_of(name.toStdString()));
        if (std::ranges::find(firstSeen, backend) == firstSeen.end()) {
            firstSeen.push_back(backend);
        }
    }
    CHECK(firstSeen == std::vector<std::string>{"parakeet"});
    // Inside a family the recommended order still applies.
    CHECK(page->rowOrder().front() == q(kOrukeet));
}

// ---- TestActions: row actions route through the page's signals ----

// from tests/test_settings_downloads.py::TestActions::test_download_click_invokes_callback
TEST_CASE("download click invokes callback", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    QSignalSpy requested(page.get(), &DownloadsPage::downloadRequested);
    row(*page, kOrukeet)->downloadButton()->click();
    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == q(kOrukeet));
}

// from tests/test_settings_downloads.py::TestActions::test_delete_confirm_default_no_does_nothing
TEST_CASE("delete confirm default no does nothing", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    QSignalSpy requested(page.get(), &DownloadsPage::deleteRequested);
    QString title;
    QString text;
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton;
    page->runMessageBox = [&](QMessageBox& box) {
        title = box.windowTitle();
        text = box.text();
        defaultButton = box.standardButton(box.defaultButton());
        return static_cast<int>(QMessageBox::No);
    };
    row(*page, kParakeet)->deleteButton()->click();
    CHECK(requested.count() == 0);
    CHECK(title == "Delete Model");
    CHECK(text == "Delete the downloaded files for \"parakeet-v3\"?\n\n"
                  "You can download the model again later.");
    CHECK(defaultButton == QMessageBox::No);
}

// from tests/test_settings_downloads.py::TestActions::test_delete_confirm_yes_invokes_callback
TEST_CASE("delete confirm yes invokes callback", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    QSignalSpy requested(page.get(), &DownloadsPage::deleteRequested);
    page->runMessageBox = [](QMessageBox&) {
        return static_cast<int>(QMessageBox::Yes);
    };
    row(*page, kParakeet)->deleteButton()->click();
    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == q(kParakeet));
}

// from
// tests/test_settings_downloads.py::TestActions::test_delete_result_is_reported_in_the_message_line
TEST_CASE("delete result is reported in the message line", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->showDeleteResult(q(kParakeet), false, QStringLiteral("locked"));
    CHECK(page->messageLabel()->text().contains("locked"));
    page->showDeleteResult(q(kParakeet), true, QString());
    CHECK(page->messageLabel()->text() == "Deleted \"parakeet-v3\"");
}

// ---- TestInspector: the side inspector replaced the separate Model Details dialog ----

// from
// tests/test_settings_downloads.py::TestInspector::test_inspector_is_empty_until_a_model_is_selected
TEST_CASE("inspector is empty until a model is selected", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    CHECK(visibleTo(page->inspectorEmpty(), page.get()));
    CHECK_FALSE(visibleTo(page->inspectorName(), page.get()));
    CHECK_FALSE(visibleTo(page->inspectorRepoButton(), page.get()));
    CHECK(page->inspectorEmpty()->text().contains("Select a model"));
}

// from
// tests/test_settings_downloads.py::TestInspector::test_selecting_a_row_shows_its_bundled_profile
TEST_CASE("selecting a row shows its bundled profile", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->selectModel(q(kParakeet));

    CHECK(page->inspectorName()->text() == "Parakeet TDT 0.6B v3");
    CHECK(page->inspectorName()->toolTip() == q(kParakeet));
    CHECK(visibleTo(page->inspectorRepoButton(), page.get()));
    CHECK_FALSE(page->inspectorDescription()->text().isEmpty());
    REQUIRE(page->factLabel(QStringLiteral("Repository")) != nullptr);
    CHECK(page->factLabel(QStringLiteral("Repository"))->text() == q(kParakeetRepo));
    CHECK_FALSE(page->factLabel(QStringLiteral("Local format"))->text().isEmpty());
    CHECK(page->inspectorTradeoffs()->text().startsWith(QStringLiteral("•")));
    QStringList captions;
    for (const char* caption :
         {"Origin", "Repository", "Maintainer", "Family", "Languages", "Tasks", "Parameters",
          "Published speed", "Memory guidance", "Download size", "Local format", "License",
          "Download host", "Version", "Download checks"}) {
        CHECK(page->factLabel(QString::fromUtf8(caption)) != nullptr);
    }
    CHECK(page->factLabel(QStringLiteral("Download host"))->text() == "huggingface.co");
}

// from
// tests/test_settings_downloads.py::TestInspector::test_tag_pill_shows_its_full_text_under_the_theme
TEST_CASE("tag pill shows its full text under the theme", "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    page->selectModel(q(kParakeet));
    QCoreApplication::processEvents();

    fv::ElidingLabel* tags = page->inspectorTags();
    CHECK_FALSE(tags->text().isEmpty());
    CHECK(tags->QLabel::text() == tags->text());
    CHECK(tags->contentsRect().width() >= tags->fontMetrics().horizontalAdvance(tags->text()));
}

// from
// tests/test_settings_downloads.py::TestInspector::test_source_links_never_paint_over_each_other
TEST_CASE("source links never paint over each other", "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    for (const std::string& name : {kParakeet, kOrukeet}) {
        page->selectModel(q(name));
        QCoreApplication::processEvents();
        const std::vector<fv::Button*> buttons{page->inspectorRepoButton(),
                                               page->inspectorOriginButton(),
                                               page->inspectorLicenseButton()};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            for (std::size_t j = i + 1; j < buttons.size(); ++j) {
                INFO(name << " " << buttons[i]->text().toStdString() << " / "
                          << buttons[j]->text().toStdString());
                CHECK_FALSE(buttons[i]->geometry().intersects(buttons[j]->geometry()));
            }
        }
        for (fv::Button* button : buttons) {
            INFO(name << " " << button->text().toStdString());
            CHECK(button->width() >= button->sizeHint().width());
            CHECK(visibleTo(button, page.get()));
        }
    }
}

// from
// tests/test_settings_downloads.py::TestInspector::test_profile_text_fits_the_scroll_area_for_every_model
TEST_CASE("profile text fits the scroll area for every model",
          "[test_settings_downloads][widget]") {
    fvtest::AppTheme theme;
    DownloadsRig rig;
    auto page = rig.page();
    page->resize(1000, 700);
    page->show();
    QScrollArea* scroll = page->inspectorDetailScroll();
    // A model folder, like a commit hash, has nowhere to wrap.
    const QString folder = QStringLiteral(
        "D:\\models\\a-very-long-unbroken-model-folder-name-without-dashes_or_spaces");
    for (const auto& [name, each] : page->rows()) {
        page->selectModel(name);
        page->factLabel(QStringLiteral("Origin"))->setText(folder);
        QCoreApplication::processEvents();
        QWidget* content = scroll->widget();
        INFO(name.toStdString());
        CHECK(content->width() == scroll->viewport()->width());
        for (QLabel* label : content->findChildren<QLabel*>()) {
            if (label->isVisibleTo(content) && !label->text().isEmpty()) {
                INFO(label->text().toStdString());
                // Its own width, not the widest word's, is what the text wraps at.
                CHECK(label->minimumSizeHint().width() <= label->width());
                CHECK(label->geometry().right() <= content->width());
            }
        }
    }
}

// from tests/test_settings_downloads.py::TestInspector::test_row_click_signal_selects_that_model
TEST_CASE("row click signal selects that model", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    Q_EMIT row(*page, kOrukeet)->detailsRequested(q(kOrukeet));

    CHECK(page->inspectorName()->text() == "Orukeet TDT 0.6B");
    CHECK(row(*page, kOrukeet)->property(fv::ui::prop::selected).toBool());
    CHECK_FALSE(row(*page, kParakeet)->property(fv::ui::prop::selected).toBool());
}

// from tests/test_settings_downloads.py::TestInspector::test_auto_has_no_profile_to_show
TEST_CASE("auto has no profile to show", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->selectModel(QStringLiteral("auto"));
    CHECK(visibleTo(page->inspectorEmpty(), page.get()));
    CHECK_FALSE(visibleTo(page->inspectorName(), page.get()));
    CHECK_FALSE(page->selectedModel().has_value());
}

// from
// tests/test_settings_downloads.py::TestInspector::test_inspector_reports_the_selected_model_usage
TEST_CASE("inspector reports the selected model usage", "[test_settings_downloads][widget]") {
    DownloadsRig rig(dictating(kParakeet));
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    page->selectModel(q(kParakeet));
    page->refresh();
    CHECK(page->inspectorUsage()->text() == "Dictation");
    CHECK(visibleTo(page->inspectorUsage(), page.get()));
}

// from
// tests/test_settings_downloads.py::TestInspector::test_representative_profiles_render_expected_facts
TEST_CASE("representative profiles render expected facts", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    struct Expected {
        std::string model;
        const char* family;
        const char* parameters;
        const char* languages;
        const char* license;
    };
    for (const Expected& expected :
         {Expected{kParakeet, "parakeet", "600 million", "25 European languages", "CC-BY-4.0"},
          Expected{kOrukeet, "parakeet", "627 million",
                   "25 European languages, including Russian and English", "CC-BY-SA-4.0"}}) {
        page->selectModel(q(expected.model));
        INFO(expected.model);
        CHECK(page->factLabel(QStringLiteral("Family"))->text() == expected.family);
        CHECK(page->factLabel(QStringLiteral("Parameters"))->text() == expected.parameters);
        CHECK(page->factLabel(QStringLiteral("Languages"))->text() == expected.languages);
        CHECK(page->factLabel(QStringLiteral("Local format"))->text().contains("GGUF"));
        CHECK(page->factLabel(QStringLiteral("License"))->text() == expected.license);
        CHECK(page->factLabel(QStringLiteral("Download size"))->text() == "~714 MB");
    }
}

// from
// tests/test_settings_downloads.py::TestInspector::test_profile_needs_no_network_under_the_hf_override
TEST_CASE("profile needs no network under the hf override", "[test_settings_downloads][widget]") {
    // The page has no network client at all: the profile is bundled.
    DownloadsRig rig;
    rig.blockDownloads();
    auto page = rig.page();
    page->selectModel(q(kOrukeet));
    CHECK(page->factLabel(QStringLiteral("Origin"))->text() ==
          "Orukeet, based on NVIDIA Parakeet TDT v3");
}

// from tests/test_settings_downloads.py::TestInspector::test_repository_links_open_the_bundled_urls
TEST_CASE("repository links open the bundled urls", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->selectModel(q(kParakeet));
    std::vector<QString> opened;
    page->openUrl = [&opened](const QUrl& url) {
        opened.push_back(url.toString());
    };
    page->inspectorRepoButton()->click();
    page->inspectorOriginButton()->click();
    page->inspectorLicenseButton()->click();
    REQUIRE(opened.size() == 3);
    CHECK(opened[0].endsWith(q(kParakeetRepo)));
    CHECK(opened[1].startsWith("http"));
    CHECK(opened[2] == "https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3#license");
    CHECK(page->inspectorRepoButton()->text() == "Hugging Face ↗");
}

TEST_CASE("open folder opens the speech models folder", "[widget]") {
    // A folder that exists: openFolderInFileManager reports a missing one instead of opening it.
    QTemporaryDir models;
    REQUIRE(models.isValid());
    DownloadsRig rig;
    rig.folder = models.path();
    auto page = rig.page();
    OpenedUrls opened;
    page->openFolderButton()->click();
    // Every catalog model lives in the one folder.
    page->selectModel(q(kParakeet));
    page->openFolderButton()->click();
    const QString expected = QUrl::fromLocalFile(models.path()).toString();
    CHECK(opened.urls == std::vector<QString>{expected, expected});
}

// ---- TestRowActivation: selecting a row stays separate from its actions ----

// from tests/test_settings_downloads.py::TestRowActivation::test_row_body_click_selects_the_model
TEST_CASE("row body click selects the model", "[test_settings_downloads][widget]") {
    ModelRowWidget each(q(kParakeet));
    each.resize(720, 64);
    each.show();
    QSignalSpy requested(&each, &ModelRowWidget::detailsRequested);

    QTest::mouseClick(&each, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));

    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == q(kParakeet));
    each.close();
}

// from tests/test_settings_downloads.py::TestRowActivation::test_enter_and_space_select_the_model
TEST_CASE("enter and space select the model", "[test_settings_downloads][widget]") {
    ModelRowWidget each(q(kOrukeet));
    QSignalSpy requested(&each, &ModelRowWidget::detailsRequested);

    QTest::keyClick(&each, Qt::Key_Return);
    QTest::keyClick(&each, Qt::Key_Space);

    REQUIRE(requested.count() == 2);
    CHECK(requested.at(0).at(0).toString() == q(kOrukeet));
    CHECK(requested.at(1).at(0).toString() == q(kOrukeet));
}

// from
// tests/test_settings_downloads.py::TestRowActivation::test_action_buttons_do_not_select_the_model
TEST_CASE("action buttons do not select the model", "[test_settings_downloads][widget]") {
    ModelRowWidget each(q(kParakeet));
    QSignalSpy selected(&each, &ModelRowWidget::detailsRequested);
    QSignalSpy downloads(&each, &ModelRowWidget::downloadClicked);
    QSignalSpy deletes(&each, &ModelRowWidget::deleteClicked);

    each.downloadButton()->click();
    each.deleteButton()->click();

    CHECK(downloads.count() == 1);
    CHECK(deletes.count() == 1);
    CHECK(selected.count() == 0);
}

// ---- TestBatchSelection: checkboxes, running total, confirmation ----

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_uncached_rows_offer_selection_cached_do_not
TEST_CASE("uncached rows offer selection cached do not", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    CHECK(visibleTo(row(*page, kOrukeet)->selectCheckbox(), page.get()));
    CHECK_FALSE(visibleTo(row(*page, kParakeet)->selectCheckbox(), page.get()));
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_toggling_updates_summary_and_button
TEST_CASE("toggling updates summary and button", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
    CHECK(page->selectionSummary()->text().contains("1 selected"));
    CHECK(page->selectionSummary()->text().contains("~714 MB"));
    CHECK(page->downloadSelectedButton()->text() == "Download 1 model…");

    row(*page, kParakeet)->selectCheckbox()->setChecked(true);
    CHECK(page->selectionSummary()->text().contains("2 selected"));
    CHECK(
        page->selectionSummary()->text().contains(q("~" + fv::format_size_bytes(2 * 714'000'000))));
    CHECK(page->downloadSelectedButton()->text() == "Download 2 models…");
}

// from tests/test_settings_downloads.py::TestBatchSelection::test_clear_empties_the_selection
TEST_CASE("clear empties the selection", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
    page->clearSelection();
    CHECK_FALSE(row(*page, kOrukeet)->selectCheckbox()->isChecked());
    CHECK(page->selectionSummary()->text() == "Tick models to download several at once");
    CHECK_FALSE(page->downloadSelectedButton()->isEnabled());
    CHECK(page->downloadSelectedButton()->toolTip() == "Tick the models to download first");

    row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
    CHECK(page->downloadSelectedButton()->isEnabled());
    CHECK(page->downloadSelectedButton()->toolTip().isEmpty());
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_disabled_download_selected_looks_disabled
TEST_CASE("disabled download selected looks disabled", "[test_settings_downloads][widget]") {
    QColor enabled;
    QColor disabled;
    {
        fvtest::AppTheme theme;
        DownloadsRig rig;
        auto page = rig.page();
        fv::Button* button = page->downloadSelectedButton();
        page->resize(1100, 700);
        page->show();
        row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
        QCoreApplication::processEvents();
        enabled = button->grab().toImage().pixelColor(button->width() / 2, 4);

        page->clearSelection();
        QCoreApplication::processEvents();
        CHECK_FALSE(button->isEnabled());
        disabled = button->grab().toImage().pixelColor(button->width() / 2, 4);
        page->close();
    }
    // #primaryButton's accent fill outranked QPushButton:disabled.
    CHECK(enabled.blue() > enabled.red() + 60);
    CHECK(std::abs(disabled.blue() - disabled.red()) < 40);
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_select_all_checks_only_missing_models
TEST_CASE("select all checks only missing models", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    page->selectAllMissing();
    CHECK(row(*page, kOrukeet)->selectCheckbox()->isChecked());
    CHECK_FALSE(row(*page, kParakeet)->selectCheckbox()->isChecked());
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_download_selected_confirms_and_requests_in_catalog_order
TEST_CASE("download selected confirms and requests in catalog order",
          "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    QSignalSpy requested(page.get(), &DownloadsPage::batchDownloadRequested);
    // Ticked in the opposite order to the catalog's.
    row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
    row(*page, kParakeet)->selectCheckbox()->setChecked(true);
    QString listed;
    page->runDialog = [&listed](QDialog& dialog) {
        if (auto* batch = qobject_cast<fv::BatchDownloadDialog*>(&dialog)) {
            listed = batch->modelListLabel()->text();
        }
        return static_cast<int>(QDialog::Accepted);
    };

    page->downloadSelectedButton()->click();

    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toStringList() == QStringList{q(kParakeet), q(kOrukeet)});
    CHECK(listed.indexOf(q(kParakeet)) < listed.indexOf(q(kOrukeet)));
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_canceled_confirmation_requests_nothing
TEST_CASE("canceled confirmation requests nothing", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    QSignalSpy requested(page.get(), &DownloadsPage::batchDownloadRequested);
    row(*page, kOrukeet)->selectCheckbox()->setChecked(true);
    page->runDialog = [](QDialog&) {
        return static_cast<int>(QDialog::Rejected);
    };

    page->downloadSelectedButton()->click();

    CHECK(requested.count() == 0);
}

// from
// tests/test_settings_downloads.py::TestBatchSelection::test_confirmation_lists_sizes_total_and_destination
TEST_CASE("confirmation lists sizes total and destination", "[test_settings_downloads][widget]") {
    // The page hands its models folder to the dialog it opens.
    DownloadsRig rig;
    auto page = rig.page();
    QString destination;
    QString list;
    QString total;
    page->runDialog = [&](QDialog& dialog) {
        if (auto* batch = qobject_cast<fv::BatchDownloadDialog*>(&dialog)) {
            destination = batch->destinationPathLabel()->text();
            list = batch->modelListLabel()->text();
            total = batch->totalLabel()->text();
        }
        return static_cast<int>(QDialog::Rejected);
    };
    page->downloadAllButton()->click();
    CHECK(list.contains(q(kOrukeet + "  ·  ~714 MB")));
    CHECK(list.contains(q(kParakeet + "  ·  ~714 MB")));
    CHECK(total == q("Estimated total: ~" + fv::format_size_bytes(2 * 714'000'000)));
    CHECK(destination == fvtest::kModelsFolder);
}

// ---- TestBatchLifecycle: queue bookkeeping between beginBatch and finishBatch ----

// from
// tests/test_settings_downloads.py::TestBatchLifecycle::test_begin_batch_locks_selection_and_downloads
TEST_CASE("begin batch locks selection and downloads", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->beginBatch({q(kOrukeet)});

    CHECK(visibleTo(page->stopBatchButton(), page.get()));
    CHECK(page->stopBatchButton()->toolTip() ==
          "Finish the model now downloading, then stop the queue");
    CHECK_FALSE(row(*page, kParakeet)->selectCheckbox()->isEnabled());
    CHECK_FALSE(row(*page, kParakeet)->downloadButton()->isEnabled());
    CHECK_FALSE(page->selectAllButton()->isEnabled());
    CHECK_FALSE(page->downloadAllButton()->isEnabled());
    CHECK(page->messageLabel()->text() == "Downloading 1 models…");

    QSignalSpy stop(page.get(), &DownloadsPage::batchCancelRequested);
    page->stopBatchButton()->click();
    CHECK(stop.count() == 1);
}

// from tests/test_settings_downloads.py::TestBatchLifecycle::test_progress_counts_each_model
TEST_CASE("progress counts each model", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->beginBatch({q(kOrukeet), q(kParakeet)});

    page->setDownloading(q(kOrukeet));
    CHECK(page->messageLabel()->text().contains("Downloading model 1 of 2: orukeet-v0.1"));
    CHECK(badgeOf(row(*page, kParakeet)) == "Queued");

    page->finishDownload(q(kOrukeet), true);
    page->setDownloading(q(kParakeet));
    CHECK(page->messageLabel()->text().contains("Downloading model 2 of 2: parakeet-v3"));
}

// from
// tests/test_settings_downloads.py::TestBatchLifecycle::test_finish_batch_summarizes_failures_and_unlocks
TEST_CASE("finish batch summarizes failures and unlocks", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->beginBatch({q(kOrukeet), q(kParakeet)});
    page->setDownloading(q(kOrukeet));
    page->finishDownload(q(kOrukeet), true);
    page->setDownloading(q(kParakeet));
    page->finishDownload(q(kParakeet), false);

    page->finishBatch(1, 2);

    CHECK(page->messageLabel()->text() == "Downloaded 1 of 2 models — 1 failed");
    CHECK_FALSE(visibleTo(page->stopBatchButton(), page.get()));
    CHECK(row(*page, kParakeet)->downloadButton()->isEnabled());
    CHECK(page->selectAllButton()->isEnabled());
}

// from tests/test_settings_downloads.py::TestBatchLifecycle::test_stopped_queue_is_reported
TEST_CASE("stopped queue is reported", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->beginBatch({q(kOrukeet), q(kParakeet)});
    page->setDownloading(q(kOrukeet));
    page->finishDownload(q(kOrukeet), true);

    page->finishBatch(1, 2);

    CHECK(page->messageLabel()->text() == "Stopped — downloaded 1 of 2 models");
    CHECK_FALSE(visibleTo(page->stopBatchButton(), page.get()));
}

// from
// tests/test_settings_downloads.py::TestBatchLifecycle::test_single_download_message_is_unchanged
TEST_CASE("single download message is unchanged", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->setDownloading(q(kOrukeet));
    CHECK(page->messageLabel()->text().contains("Downloading \"orukeet-v0.1\""));
    CHECK_FALSE(visibleTo(page->stopBatchButton(), page.get()));
}

// from
// tests/test_settings_downloads.py::TestBatchLifecycle::test_env_blocked_disables_selection_actions
TEST_CASE("env blocked disables selection actions", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.blockDownloads();
    auto page = rig.page();
    CHECK_FALSE(row(*page, kOrukeet)->selectCheckbox()->isEnabled());
    CHECK_FALSE(page->selectAllButton()->isEnabled());
    CHECK_FALSE(page->downloadAllButton()->isEnabled());
    CHECK_FALSE(page->downloadSelectedButton()->isEnabled());
}

// ---- TestComponents: optional components install from the Components tab ----

namespace {

fv::models::ComponentInfo cudaInfo(ComponentState state, std::int64_t downloadBytes = 0,
                                   const std::string& reason = {}) {
    fv::models::ComponentInfo info;
    info.component_id = fvtest::kCudaRuntime;
    info.display_name = "NVIDIA Speech GPU";
    info.summary = "CUDA runtime";
    info.state = state;
    info.available_version = Json("test");
    info.download_bytes = downloadBytes;
    info.reason = reason;
    return info;
}

} // namespace

// from
// tests/test_settings_downloads.py::TestComponents::test_missing_component_has_enabled_install_button
TEST_CASE("missing component has enabled install button", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    each.updateState(cudaInfo(ComponentState::NotInstalled, 1'000'000), false);

    CHECK_FALSE(each.installButton()->isHidden());
    CHECK(each.installButton()->isEnabled());
    CHECK(each.installButton()->text() == "Install");
}

// from
// tests/test_settings_downloads.py::TestComponents::test_remove_waits_a_moment_where_install_just_was
TEST_CASE("remove waits a moment where install just was", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    QSignalSpy removes(&each, &fv::ComponentRowWidget::removeClicked);
    each.updateState(cudaInfo(ComponentState::NotInstalled, 1'000'000), true);
    each.updateState(cudaInfo(ComponentState::Installed), false);

    CHECK_FALSE(each.removeButton()->isHidden());
    CHECK_FALSE(each.removeButton()->isEnabled());
    each.removeButton()->click();
    // A refresh during the pause does not cut it short.
    each.updateState(cudaInfo(ComponentState::Installed), false);
    CHECK_FALSE(each.removeButton()->isEnabled());
    CHECK(removes.count() == 0);

    // OpenWhisper's test shortens the guard to 40 ms; here it runs its 600 ms.
    for (int i = 0; i < 200 && !each.removeButton()->isEnabled(); ++i) {
        QTest::qWait(10);
    }
    CHECK(each.removeButton()->isEnabled());
    each.removeButton()->click();
    REQUIRE(removes.count() == 1);
    CHECK(removes.at(0).at(0).toString() == q(fvtest::kCudaRuntime));
}

// from
// tests/test_settings_downloads.py::TestComponents::test_remove_is_ready_at_once_for_a_component_already_installed
TEST_CASE("remove is ready at once for a component already installed",
          "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    each.updateState(cudaInfo(ComponentState::Installed), false);

    CHECK(each.removeButton()->isEnabled());
    CHECK(fv::ComponentRowWidget::kRemoveGuardMs == 600);
}

// from
// tests/test_settings_downloads.py::TestComponents::test_existing_cuda_setup_is_not_reported_missing
TEST_CASE("existing cuda setup is not reported missing", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    each.updateState(cudaInfo(ComponentState::External, 0, "CUDA libraries are already available."),
                     false);

    CHECK(each.badge()->text() == "Available");
    CHECK(each.sizeLabel()->text() == "Existing setup");
    CHECK(each.installButton()->isHidden());
    CHECK(each.removeButton()->isHidden());
}

// from
// tests/test_settings_downloads.py::TestComponents::test_component_progress_and_result_reach_the_row_and_message
TEST_CASE("component progress and result reach the row and message",
          "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.installing.insert(fvtest::kCpuRuntime);
    auto page = rig.page();
    REQUIRE_FALSE(page->componentRows().empty());
    const QString id = page->componentRows().front().first;
    fv::ComponentRowWidget* each = page->componentRow(id);

    page->setComponentProgress(id, QStringLiteral("downloading"), 1, 2);
    CHECK(each->progressBar()->value() == 50);
    CHECK(each->badge()->text() == "Downloading…");
    rig.installing.clear();
    page->finishComponentInstall(id, true, QStringLiteral("Installed"));

    CHECK(page->messageLabel()->text() == "Installed");
}

// from
// tests/test_settings_downloads.py::TestComponents::test_component_removal_asks_before_deleting
TEST_CASE("component removal asks before deleting", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    REQUIRE_FALSE(page->componentRows().empty());
    const QString id = page->componentRows().front().first;
    QSignalSpy requested(page.get(), &DownloadsPage::componentRemoveRequested);
    QString text;
    page->runMessageBox = [&text](QMessageBox& box) {
        text = box.text();
        return static_cast<int>(QMessageBox::No);
    };
    page->confirmComponentRemoval(id);
    CHECK(requested.count() == 0);
    const fv::models::ComponentInfo info = fvtest::runtimeInfo(id.toStdString());
    CHECK(text ==
          q("Remove " + info.display_name + " (" + fv::format_size_bytes(info.install_bytes) +
            ")?\n\nYou can install it again later."));

    page->runMessageBox = [](QMessageBox&) {
        return static_cast<int>(QMessageBox::Yes);
    };
    page->confirmComponentRemoval(id);
    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == id);
}

TEST_CASE("a component that cannot be described is neither asked about nor removed", "[widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    REQUIRE_FALSE(page->componentRows().empty());
    const QString id = page->componentRows().front().first;
    QSignalSpy requested(page.get(), &DownloadsPage::componentRemoveRequested);
    int asked = 0;
    page->runMessageBox = [&asked](QMessageBox&) {
        ++asked;
        return static_cast<int>(QMessageBox::Yes);
    };
    rig.describeFails = true;
    // The row's Remove click is a slot: the coordinator's error is logged, not thrown at Qt.
    CHECK_NOTHROW(page->confirmComponentRemoval(id));
    CHECK(asked == 0);
    CHECK(requested.count() == 0);
}

// from tests/test_settings_downloads.py::TestComponents::test_row_click_opens_the_component_profile
TEST_CASE("row click opens the component profile", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    REQUIRE_FALSE(page->componentRows().empty());
    const QString id = page->componentRows().front().first;
    std::vector<QString> opened;
    bool selectedWhileOpen = false;
    page->runDialog = [&](QDialog& dialog) {
        if (auto* details = qobject_cast<fv::ComponentDetailsDialog*>(&dialog)) {
            opened.push_back(QString::fromStdString(details->componentId().toStdString()));
            selectedWhileOpen = page->componentRow(id)->property(fv::ui::prop::selected).toBool();
        }
        return 0;
    };

    Q_EMIT page->componentRow(id)->detailsRequested(id);

    CHECK(opened == std::vector<QString>{id});
    CHECK(selectedWhileOpen);
    CHECK_FALSE(page->componentRow(id)->property(fv::ui::prop::selected).toBool());
}

// from tests/test_settings_downloads.py::TestComponents::test_component_body_click_requests_details
TEST_CASE("component body click requests details", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    each.resize(720, 72);
    each.show();
    QSignalSpy requested(&each, &fv::ComponentRowWidget::detailsRequested);

    QTest::mouseClick(&each, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));

    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == q(fvtest::kCudaRuntime));
    each.close();
}

// from
// tests/test_settings_downloads.py::TestComponents::test_enter_and_space_open_component_details
TEST_CASE("enter and space open component details", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCpuRuntime));
    QSignalSpy requested(&each, &fv::ComponentRowWidget::detailsRequested);

    QTest::keyClick(&each, Qt::Key_Return);
    QTest::keyClick(&each, Qt::Key_Space);

    REQUIRE(requested.count() == 2);
    CHECK(requested.at(0).at(0).toString() == q(fvtest::kCpuRuntime));
    CHECK(requested.at(1).at(0).toString() == q(fvtest::kCpuRuntime));
}

// from
// tests/test_settings_downloads.py::TestComponents::test_action_buttons_do_not_open_component_details
TEST_CASE("action buttons do not open component details", "[test_settings_downloads][widget]") {
    fv::ComponentRowWidget each(q(fvtest::kCudaRuntime));
    QSignalSpy selected(&each, &fv::ComponentRowWidget::detailsRequested);
    QSignalSpy installs(&each, &fv::ComponentRowWidget::installClicked);
    QSignalSpy removes(&each, &fv::ComponentRowWidget::removeClicked);
    each.updateState(cudaInfo(ComponentState::Broken, 1'000'000), false);

    each.installButton()->click();
    each.removeButton()->click();

    CHECK(installs.count() == 1);
    CHECK(removes.count() == 1);
    CHECK(selected.count() == 0);
}

// from
// tests/test_settings_downloads.py::TestComponents::test_component_profile_renders_bundled_facts_and_links
TEST_CASE("component profile renders bundled facts and links",
          "[test_settings_downloads][widget]") {
    const fv::models::ComponentDetails& details =
        fv::models::get_component_details(fvtest::kCpuRuntime);
    fv::ComponentDetailsDialog popup(fvtest::kCpuRuntime, fvtest::runtimeInfo(fvtest::kCpuRuntime));
    std::vector<QString> opened;
    popup.setUrlOpener([&opened](const QUrl& url) { opened.push_back(url.toString()); });
    popup.sourceButton()->click();

    CHECK(popup.nameLabel()->text() == q(details.display_name));
    CHECK(popup.descriptionLabel()->text() == q(details.description));
    REQUIRE(popup.factLabel(QStringLiteral("Origin")) != nullptr);
    CHECK(popup.factLabel(QStringLiteral("Origin"))->text() == q(details.origin_name));
    REQUIRE(popup.factLabel(QStringLiteral("Payload")) != nullptr);
    CHECK_FALSE(popup.factLabel(QStringLiteral("Payload"))->text().isEmpty());
    CHECK(popup.tradeoffsLabel()->text().startsWith(QStringLiteral("•")));
    CHECK(popup.sourceButton()->toolTip() == q(details.source_url));
    CHECK(opened == std::vector<QString>{q(details.source_url)});
    popup.close();
}

// from
// tests/test_settings_downloads.py::TestComponents::test_component_profile_hides_duplicate_link_button
TEST_CASE("component profile hides duplicate link button", "[test_settings_downloads][widget]") {
    // The speech runtimes publish one upstream project as both origin and source.
    const fv::models::ComponentDetails& details =
        fv::models::get_component_details(fvtest::kCudaRuntime);
    REQUIRE(details.origin_url == details.source_url);
    fv::ComponentDetailsDialog popup(fvtest::kCudaRuntime,
                                     fvtest::runtimeInfo(fvtest::kCudaRuntime));

    CHECK_FALSE(popup.sourceButton()->isHidden());
    CHECK(popup.originButton()->isHidden());
    CHECK(popup.sourceButton()->toolTip() == q(details.source_url));
    popup.close();
}

// ---- TestSectionTabs: models and components are separate tabs ----

// from tests/test_settings_downloads.py::TestSectionTabs::test_components_are_not_in_the_model_list
TEST_CASE("components are not in the model list", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    REQUIRE_FALSE(page->componentRows().empty());
    for (const auto& [id, each] : page->componentRows()) {
        CHECK(page->componentsSection()->isAncestorOf(each));
        CHECK_FALSE(page->libraryScrollArea()->isAncestorOf(each));
    }
}

// from
// tests/test_settings_downloads.py::TestSectionTabs::test_opens_on_models_and_the_tabs_switch_sections
TEST_CASE("opens on models and the tabs switch sections", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    CHECK(page->tabs()->currentIndex() == 0);
    CHECK(page->modelsSection()->isVisible());
    CHECK_FALSE(page->componentsSection()->isVisible());
    CHECK(page->tabs()->accessibleName() == "Downloads section");
    page->tabs()->buttons()[1]->click();
    CHECK(page->componentsSection()->isVisible());
    CHECK_FALSE(page->modelsSection()->isVisible());
    // Every component is on screen at once, not behind a scroll.
    CHECK(visibleTo(page->componentRows().front().second, page.get()));
}

// from
// tests/test_settings_downloads.py::TestSectionTabs::test_tab_titles_count_what_is_downloaded_and_installed
TEST_CASE("tab titles count what is downloaded and installed",
          "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.components = {fvtest::runtimeInfo(fvtest::kCpuRuntime, ComponentState::Installed),
                      fvtest::runtimeInfo(fvtest::kCudaRuntime)};
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    const QString models = page->tabs()->buttons()[0]->titleLabel()->text();
    const QString components = page->tabs()->buttons()[1]->titleLabel()->text();
    CHECK(models == "Speech models · 1 of 2 downloaded");
    CHECK(components == "Components · 1 of 2 installed");
}

// from
// tests/test_settings_downloads.py::TestSectionTabs::test_an_available_update_is_visible_from_the_tab
TEST_CASE("an available update is visible from the tab", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    const auto describeWith = [&rig](const std::set<std::string>& updating) {
        rig.components.clear();
        for (const std::string& id : {fvtest::kCpuRuntime, fvtest::kCudaRuntime}) {
            fv::models::ComponentInfo info =
                fvtest::runtimeInfo(id, updating.contains(id) ? ComponentState::UpdateAvailable
                                                              : ComponentState::Installed);
            info.installed_version = Json("1");
            info.available_version = Json("2");
            rig.components.push_back(info);
        }
    };
    const auto title = [&page] {
        return page->tabs()->buttons()[1]->titleLabel()->text();
    };

    describeWith({});
    page->refreshComponents();
    CHECK_FALSE(title().contains("update"));
    describeWith({fvtest::kCpuRuntime});
    page->refreshComponents();
    CHECK(title().endsWith("· 1 update"));
    describeWith({fvtest::kCpuRuntime, fvtest::kCudaRuntime});
    page->refreshComponents();
    CHECK(title().endsWith("· 2 updates"));
}

// from
// tests/test_settings_downloads.py::TestSectionTabs::test_deep_links_switch_to_the_tab_that_has_what_they_want
TEST_CASE("deep links switch to the tab that has what they want",
          "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    const QString id = page->componentRows().front().first;
    page->focusComponent(id);
    CHECK(page->tabs()->currentIndex() == 1);
    CHECK(page->componentsSection()->isVisible());
    CHECK(page->componentRow(id)->property(fv::ui::prop::selected).toBool());
    page->showBackend(QStringLiteral("parakeet"));
    CHECK(page->tabs()->currentIndex() == 0);
    CHECK(page->modelsSection()->isVisible());
    CHECK(page->backendFilterCombo()->currentData().toString() == "parakeet");
    page->focusComponent(id);
    page->revealModel(q(kParakeet));
    CHECK(page->tabs()->currentIndex() == 0);
    CHECK(page->selectedModel() == q(kParakeet));
}

// from
// tests/test_settings_downloads.py::TestSectionTabs::test_a_platform_with_no_components_shows_no_tabs
TEST_CASE("a platform with no components shows no tabs", "[test_settings_downloads][widget]") {
    DownloadsRig rig;
    rig.components.clear();
    auto page = rig.page();
    page->show();
    QCoreApplication::processEvents();
    CHECK(page->tabs()->isHidden());
    CHECK(page->modelsSection()->isVisible());
}

// ---- The scan worker ----

// from tests/test_settings_downloads.py::test_a_page_closed_mid_scan_is_not_held_by_the_worker
TEST_CASE("a page closed mid scan is not held by the worker", "[test_settings_downloads][widget]") {
    for (const bool parented : {false, true}) {
        INFO("parented " << parented);
        DownloadsRig rig;
        rig.background = true;
        auto parent = parented ? std::make_unique<QWidget>() : nullptr;
        auto* page = new DownloadsPage(rig.settings, rig.io, rig.options(), true, parent.get());
        QPointer<DownloadsPage> watched(page);
        page->refresh();
        // The scan waits on the io executor: the worker holds no reference to the page.
        CHECK(rig.scans == 0);
        if (parented) {
            parent->deleteLater();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            parent.release();
        } else {
            delete page;
        }
        CHECK(watched == nullptr);
        rig.io.run_all();
        CHECK(rig.scans == 1);
        QCoreApplication::processEvents();
    }
}

TEST_CASE("the first scan says it is checking until the worker answers", "[widget]") {
    DownloadsRig rig;
    rig.background = true;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    CHECK(page->isChecking());
    CHECK(page->railValue() == "Checking downloaded models…");
    CHECK(page->statsLabel()->text() ==
          QStringLiteral("Checking downloaded models… · ") + fvtest::kModelsFolder);
    CHECK(page->tabs()->buttons()[0]->titleLabel()->text() == "Speech models");
    CHECK_FALSE(page->selectAllButton()->isEnabled());
    CHECK_FALSE(page->downloadAllButton()->isEnabled());
    CHECK_FALSE(row(*page, kOrukeet)->downloadButton()->isEnabled());
    CHECK_FALSE(row(*page, kOrukeet)->selectCheckbox()->isEnabled());

    rig.io.run_all();
    QCoreApplication::processEvents();
    CHECK_FALSE(page->isChecking());
    CHECK(page->railValue() == q("1 of 2 · " + fv::format_size_bytes(kParakeetBytes)));
    CHECK(page->selectAllButton()->isEnabled());

    // A later scan shows the last one at once and is not "checking".
    page->refresh();
    CHECK_FALSE(page->isChecking());
}

TEST_CASE("a stale scan never overwrites a newer one", "[widget]") {
    DownloadsRig rig;
    rig.background = true;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page();
    // The first scan finds Parakeet; its answer is on its way to the GUI thread.
    REQUIRE(rig.io.run_one());
    CHECK(rig.scans == 1);
    // Parakeet is deleted, and a newer refresh shows the shared inventory at once.
    rig.inventory.clear();
    page->refresh();
    CHECK(page->storageSummary().downloaded == 0);
    // The first scan's answer arrives after the newer refresh: it is dropped.
    QCoreApplication::processEvents();
    CHECK(page->storageSummary().downloaded == 0);
    CHECK(page->railValue().startsWith(QStringLiteral("0 of 2")));
    // The newer scan's answer is applied.
    rig.cache(kParakeetRepo, kParakeetBytes);
    rig.io.run_all();
    QCoreApplication::processEvents();
    CHECK(rig.scans == 2);
    CHECK(page->storageSummary().downloaded == 1);
}

TEST_CASE("a deferred page builds nothing but keeps its sizes and rail", "[widget]") {
    DownloadsRig rig;
    rig.cache(kParakeetRepo, kParakeetBytes);
    auto page = rig.page(true);
    CHECK_FALSE(page->isUiBuilt());
    CHECK(page->findChildren<QWidget*>().isEmpty());
    CHECK(page->storageSummary().downloaded == 1);
    CHECK(page->storageSummary().bytes == kParakeetBytes);
    CHECK(page->railValue() == q("1 of 2 · " + fv::format_size_bytes(kParakeetBytes)));

    page->setDownloading(q(kOrukeet));
    page->setDownloadProgress(q(kOrukeet), 25, 100);
    CHECK_FALSE(page->isUiBuilt());
    CHECK(page->railValue() == "Downloading Orukeet TDT 0.6B · 25%");

    // Any widget asked for builds it, with what it kept.
    CHECK(page->messageLabel()->text() == "Downloading \"orukeet-v0.1\"… 25 B of 100 B");
    CHECK(page->isUiBuilt());
    CHECK(badgeOf(row(*page, kOrukeet)) == "Downloading…");
}

TEST_CASE("progress changes the rail once per percent", "[widget]") {
    DownloadsRig rig;
    auto page = rig.page();
    QSignalSpy progress(page.get(), &DownloadsPage::progressChanged);
    page->setDownloading(q(kOrukeet));
    page->setDownloadProgress(q(kOrukeet), 100, 1000);
    page->setDownloadProgress(q(kOrukeet), 101, 1000);
    page->setDownloadProgress(q(kOrukeet), 200, 1000);
    CHECK(progress.count() == 2);
    CHECK(page->railValue() == "Downloading Orukeet TDT 0.6B · 20%");
}
