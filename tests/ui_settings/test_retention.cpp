// Retention controls apply finished values only, and ask before deleting
// (tests/test_settings_retention_commit.py). The count spin box used to apply every keystroke:
// replacing 20 with 100 applied 1 on the way and pruned all but one saved recording.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include "app_pages_support.h"
#include "ui_settings/appearance_page.h"
#include "ui_settings/recording_page.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/no_wheel.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr std::int64_t kMb = 1024 * 1024;
using Limits = std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>;

// The open_settings fixture: Settings on the Recording page over a temporary recordings folder
// holding `recordings` files of `size` bytes, shown and active.
struct RetentionScene {
    RetentionScene(const Json& values, int recordings, std::int64_t size = 1024) : rig(values) {
        rig.useHistory();
        for (int index = 0; index < recordings; ++index) {
            const std::string stamp = std::format("20260901_{:02}{:02}00", index / 60, index % 60);
            std::ofstream file(rig.recordingsFolder() / ("recording_" + stamp + ".wav"),
                               std::ios::binary);
            file << std::string(static_cast<std::size_t>(size), '\0');
        }
        page = &rig.page<fv::RecordingPage>(dest::recording);
        rig.dialog().show();
        rig.dialog().activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&rig.dialog()));
        QApplication::processEvents();
    }

    int remaining() {
        return static_cast<int>(
            std::distance(std::filesystem::directory_iterator(rig.recordingsFolder()), {}));
    }
    std::vector<std::string> names() {
        std::vector<std::string> all;
        for (const auto& item : std::filesystem::directory_iterator(rig.recordingsFolder())) {
            all.push_back(item.path().filename().string());
        }
        std::ranges::sort(all);
        return all;
    }
    // The worker answered and History applied what it was told.
    bool settled(std::size_t applications) {
        return pumpUntil([&] {
            return rig.appliedNow().size() >= applications && !page->confirmingRetention();
        });
    }

    AppPagesRig rig;
    fv::RecordingPage* page = nullptr;
};

void type(QWidget* spinbox, const QString& text) {
    spinbox->setFocus();
    QApplication::processEvents();
    static_cast<QAbstractSpinBox*>(spinbox)->selectAll();
    QTest::keyClicks(spinbox, text);
}

} // namespace

// from tests/test_settings_retention_commit.py::test_typing_100_over_20_never_applies_1_or_10
TEST_CASE("test_typing_100_over_20_never_applies_1_or_10",
          "[test_settings_retention_commit][widget]") {
    const std::string commit = GENERATE("enter", "focus_out");
    INFO(commit);
    RetentionScene env({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 20}}, 20);
    auto* spinbox = env.page->maxRecordingsSpin();

    type(spinbox, QStringLiteral("100"));
    pumpUntil([] { return false; }, std::chrono::milliseconds(50));
    CHECK(env.rig.appliedNow().empty());
    CHECK(env.rig.saved("max_saved_recordings") == 20);

    if (commit == "enter") {
        QTest::keyClick(spinbox, Qt::Key_Return);
    } else {
        env.page->retentionCombo()->setFocus();
    }
    QApplication::processEvents();

    REQUIRE(env.settled(1));
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{100, std::nullopt}});
    CHECK(env.rig.saved("max_saved_recordings") == 100);
    CHECK(env.remaining() == 20);
    CHECK(env.rig.asked.empty());
}

// from tests/test_settings_retention_commit.py::test_confirmed_shrink_prunes_only_the_oldest
TEST_CASE("test_confirmed_shrink_prunes_only_the_oldest",
          "[test_settings_retention_commit][widget]") {
    RetentionScene env({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 20}}, 12);
    env.rig.answer = true;

    type(env.page->maxRecordingsSpin(), QStringLiteral("5"));
    QTest::keyClick(env.page->maxRecordingsSpin(), Qt::Key_Return);

    REQUIRE(env.settled(1));
    REQUIRE(env.rig.asked.size() == 1);
    CHECK(env.rig.asked[0].first == QStringLiteral("Delete 7 saved recordings?"));
    CHECK(env.rig.asked[0].second.contains(QStringLiteral("7 older saved recordings")));
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{5, std::nullopt}});
    CHECK(env.rig.saved("max_saved_recordings") == 5);
    std::vector<std::string> expected;
    for (int index = 7; index < 12; ++index) {
        expected.push_back(std::format("recording_20260901_00{:02}00.wav", index));
    }
    CHECK(env.names() == expected);
}

// from
// tests/test_settings_retention_commit.py::test_declined_shrink_keeps_files_and_restores_the_count
TEST_CASE("test_declined_shrink_keeps_files_and_restores_the_count",
          "[test_settings_retention_commit][widget]") {
    RetentionScene env({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 20}}, 12);
    env.rig.answer = false;

    type(env.page->maxRecordingsSpin(), QStringLiteral("5"));
    QTest::keyClick(env.page->maxRecordingsSpin(), Qt::Key_Return);
    REQUIRE(pumpUntil([&] { return !env.rig.asked.empty() && !env.page->confirmingRetention(); }));
    // Focus leaving afterwards must not ask again or apply the typed value.
    env.page->retentionCombo()->setFocus();
    QApplication::processEvents();
    pumpUntil([] { return false; }, std::chrono::milliseconds(100));

    CHECK(env.rig.asked.size() == 1);
    CHECK(env.rig.appliedNow().empty());
    CHECK(env.page->maxRecordingsSpin()->value() == 20);
    CHECK(env.rig.saved("max_saved_recordings") == 20);
    CHECK(env.remaining() == 12);
}

// from tests/test_settings_retention_commit.py::test_chevron_burst_settles_into_one_prompt
TEST_CASE("test_chevron_burst_settles_into_one_prompt",
          "[test_settings_retention_commit][widget]") {
    RetentionScene env({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 20}}, 20);
    env.rig.answer = true;
    env.page->retentionTimer().setInterval(0);
    QToolButton* down = env.page->maxRecordingsSpin()->downButton();

    for (int i = 0; i < 3; ++i) {
        down->click();
    }
    CHECK(env.rig.asked.empty());
    CHECK(env.rig.appliedNow().empty());
    CHECK(env.page->retentionTimer().isActive());

    REQUIRE(env.settled(1));

    REQUIRE(env.rig.asked.size() == 1);
    CHECK(env.rig.asked[0].first == QStringLiteral("Delete 3 saved recordings?"));
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{17, std::nullopt}});
    CHECK(env.remaining() == 17);
}

// from tests/test_settings_retention_commit.py::test_switching_keep_all_to_count_asks_first
TEST_CASE("test_switching_keep_all_to_count_asks_first",
          "[test_settings_retention_commit][widget]") {
    RetentionScene env({{"recording_retention_mode", "keep_all"}, {"max_saved_recordings", 20}},
                       25);
    auto* combo = env.page->retentionCombo();

    env.rig.answer = false;
    combo->setCurrentIndex(combo->findData(QStringLiteral("custom")));
    REQUIRE(pumpUntil([&] { return !env.rig.asked.empty() && !env.page->confirmingRetention(); }));

    CHECK(combo->currentData().toString() == QStringLiteral("keep_all"));
    CHECK_FALSE(env.page->maxRecordingsSpin()->isEnabled());
    CHECK(env.rig.saved("recording_retention_mode") == "keep_all");
    CHECK(env.rig.appliedNow().empty());
    CHECK(env.remaining() == 25);

    env.rig.answer = true;
    combo->setCurrentIndex(combo->findData(QStringLiteral("custom")));
    REQUIRE(env.settled(1));

    CHECK(env.rig.asked.size() == 2);
    CHECK(env.rig.saved("recording_retention_mode") == "custom");
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{20, std::nullopt}});
    CHECK(env.remaining() == 20);
}

// from tests/test_settings_retention_commit.py::test_typing_a_folder_size_never_applies_its_prefix
TEST_CASE("test_typing_a_folder_size_never_applies_its_prefix",
          "[test_settings_retention_commit][widget]") {
    RetentionScene env(
        {{"recording_retention_mode", "size_limit"}, {"max_saved_recordings_mb", 1000}}, 10,
        2 * kMb);

    // 100 MB keeps all ten 2 MB files; the "10" on the way would keep five.
    type(env.page->maxRecordingsMbSpin(), QStringLiteral("100"));
    pumpUntil([] { return false; }, std::chrono::milliseconds(50));
    CHECK(env.rig.appliedNow().empty());
    QTest::keyClick(env.page->maxRecordingsMbSpin(), Qt::Key_Return);

    REQUIRE(env.settled(1));
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{std::nullopt, 100 * kMb}});
    CHECK(env.remaining() == 10);
    CHECK(env.rig.asked.empty());
}

// from
// tests/test_settings_retention_commit.py::test_live_applied_spinboxes_commit_typed_values_once
TEST_CASE("test_live_applied_spinboxes_commit_typed_values_once",
          "[test_settings_retention_commit][widget]") {
    AppPagesRig rig;
    auto& recording = rig.page<fv::RecordingPage>(dest::recording);
    auto& appearance = rig.page<fv::AppearancePage>(dest::appearance);
    // The Dashboard port spin box is Meeting Mode's (M7).
    for (QAbstractSpinBox* spinbox :
         {static_cast<QAbstractSpinBox*>(recording.maxRecordingsSpin()),
          static_cast<QAbstractSpinBox*>(recording.maxRecordingsMbSpin()),
          static_cast<QAbstractSpinBox*>(appearance.fontSizeSpin())}) {
        CHECK_FALSE(spinbox->keyboardTracking());
    }
}

// from tests/test_settings_retention_commit.py::test_suite_keeps_recordings_out_of_the_checkout
TEST_CASE("test_suite_keeps_recordings_out_of_the_checkout",
          "[test_settings_retention_commit][widget]") {
    AppPagesRig rig;
    rig.useHistory();
    const auto checkout = std::filesystem::current_path() / "recordings";
    CHECK(std::filesystem::absolute(rig.recordingsFolder()) != std::filesystem::absolute(checkout));
    const auto fallback = fv::HistoryManager::default_recordings_folder(rig.context().paths());
    CHECK(fallback.parent_path() == rig.context().paths().env_file().parent_path());
}

// A11-19: closing Settings commits a burst of steps still settling (closeEvent's
// _commit_pending_retention), so the last count is neither lost nor applied after the window
// has gone.
TEST_CASE("closing Settings commits a retention burst still settling", "[widget]") {
    RetentionScene env({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 20}}, 20);
    env.rig.answer = true;
    env.page->retentionTimer().setInterval(60000);
    QToolButton* down = env.page->maxRecordingsSpin()->downButton();
    for (int i = 0; i < 2; ++i) {
        down->click();
    }
    REQUIRE(env.page->retentionTimer().isActive());
    CHECK(env.rig.asked.empty());

    env.rig.dialog().close();
    REQUIRE(env.settled(1));
    CHECK_FALSE(env.page->retentionTimer().isActive());
    REQUIRE(env.rig.asked.size() == 1);
    CHECK(env.rig.asked[0].first == QStringLiteral("Delete 2 saved recordings?"));
    CHECK(env.rig.appliedNow() == std::vector<Limits>{{18, std::nullopt}});
    CHECK(env.rig.saved("max_saved_recordings") == 18);
}

// A11-44: the retention controls as _build_recording_page makes them: three modes by data,
// a count of 1 to 1000, a folder size of 10 MB to 1 TB in steps of 100 MB, one scaled width
// for both, and only the active mode's spin box enabled.
TEST_CASE("the retention controls offer OpenWhisper's modes and ranges", "[widget]") {
    AppPagesRig rig({{"recording_retention_mode", "size_limit"}});
    auto& page = rig.page<fv::RecordingPage>(dest::recording);
    QComboBox* combo = page.retentionCombo();
    REQUIRE(combo->count() == 3);
    const std::vector<std::pair<QString, QString>> modes{
        {QStringLiteral("Keep all"), QStringLiteral("keep_all")},
        {QStringLiteral("By count"), QStringLiteral("custom")},
        {QStringLiteral("By folder size"), QStringLiteral("size_limit")}};
    for (int i = 0; i < combo->count(); ++i) {
        CHECK(combo->itemText(i) == modes[static_cast<std::size_t>(i)].first);
        CHECK(combo->itemData(i).toString() == modes[static_cast<std::size_t>(i)].second);
    }
    QSpinBox* count = page.maxRecordingsSpin();
    QSpinBox* size = page.maxRecordingsMbSpin();
    CHECK(count->minimum() == 1);
    CHECK(count->maximum() == 1000);
    CHECK(size->minimum() == 10);
    CHECK(size->maximum() == 1048576);
    CHECK(size->singleStep() == 100);
    CHECK(size->suffix() == QStringLiteral(" MB"));
    CHECK(count->minimumWidth() == fv::ui::scaled(156));
    CHECK(count->maximumWidth() == fv::ui::scaled(156));
    CHECK(size->maximumWidth() == fv::ui::scaled(156));
    CHECK(combo->currentData().toString() == QStringLiteral("size_limit"));
    CHECK(size->isEnabled());
    CHECK_FALSE(count->isEnabled());
    combo->setCurrentIndex(combo->findData(QStringLiteral("keep_all")));
    CHECK_FALSE(size->isEnabled());
    CHECK_FALSE(count->isEnabled());
}
