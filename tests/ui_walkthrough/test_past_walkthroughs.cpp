// The side panel's Walkthroughs page (ui_qt/widgets/past_walkthroughs.py): one card per capture
// folder, newest first. Ported from tests/test_past_walkthroughs.py (seven of its nine tests; the
// side panel registration and the library are the walkthrough integration's), and the golden
// vectors of tools/parity/cases/ui_qt.widgets.past_walkthroughs.py.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "testkit/testkit.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "walkthrough/document.h"

namespace fvtest {
namespace {

using fv::Json;
namespace past = fv::past_walkthroughs;

constexpr std::chrono::year_month_day kToday{std::chrono::year{2026}, std::chrono::October,
                                             std::chrono::day{25}};

void writeBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string wall(std::chrono::local_seconds when) {
    return std::format("{:%Y-%m-%dT%H:%M:%S}", when);
}

// "2026-10-25_22-46-05" as an isoformat start.
std::string isoStart(const std::string& name) {
    return name.substr(0, 10) + "T" + name.substr(11, 2) + ":" + name.substr(14, 2) + ":" +
           name.substr(17, 2);
}

// _walkthrough: a capture folder named `name` with `sections` of (text, with a 400 x 300
// screenshot), lasting 106 s, and a write-up when `writeUp` is set.
std::filesystem::path walkthroughFolder(const std::filesystem::path& root, const std::string& name,
                                        const std::vector<std::pair<std::string, bool>>& sections,
                                        const std::string& writeUp = {}) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    Json listed = Json::array();
    int number = 1;
    for (const auto& [text, withShot] : sections) {
        Json shot = nullptr;
        if (withShot) {
            const std::string file = "shot-" + std::to_string(number) + ".png";
            QImage image(400, 300, QImage::Format_RGB32);
            image.fill(QColor(QStringLiteral("#3a6ea5")));
            image.save(QString::fromStdString(fv::path_to_utf8(folder / file)));
            shot = file;
        }
        listed.push_back(Json{{"text", text}, {"shot", shot}, {"kept_at_s", double(number)}});
        ++number;
    }
    writeBytes(folder / "walkthrough.json", Json{{"version", 1},
                                                 {"started_at", isoStart(name)},
                                                 {"duration_s", 106.0},
                                                 {"sections", listed}}
                                                .dump());
    if (!writeUp.empty()) {
        writeBytes(folder / "write-up.md", writeUp);
        writeBytes(folder / "write-up.pdf", "%PDF-1.4");
    }
    return folder;
}

std::filesystem::path draftFolder(const std::filesystem::path& root, const std::string& name) {
    const auto folder = root / name;
    std::filesystem::create_directories(folder);
    writeBytes(folder / "draft.json", Json{{"started_at", isoStart(name)}}.dump());
    return folder;
}

struct PanelRig {
    fv::testkit::TempDataRoot tmp;
    std::filesystem::path root = tmp.path() / "captures";
    fv::ManualExecutor io;
    fv::PastWalkthroughsPanel panel{root, [] {
                                        return QDate(2026, 10, 25);
                                    }};

    PanelRig() { panel.setExecutor(&io); }
    ~PanelRig() { panel.close(); }

    // _loaded: refresh, then until the read has come back.
    void loaded() {
        panel.refresh();
        io.run_all();
        REQUIRE(QTest::qWaitFor([this] { return panel.total().has_value(); }, 5000));
    }
};

} // namespace

// from tests/test_past_walkthroughs.py::test_a_written_up_walkthrough_is_titled_by_its_heading
TEST_CASE("a written up walkthrough is titled by its heading", "[test_past_walkthroughs][widget]") {
    fv::testkit::TempDataRoot tmp;
    const auto folder = walkthroughFolder(
        tmp.path() / "captures", "2026-10-25_22-46-05",
        {{"I think we don't need this whole recent area", true}, {"and this", true}},
        "---\nfont: Georgia\n---\n\n# Rethinking the Recent area\n\nBody.");

    const past::Summary summary = past::summarize(folder, kToday);

    CHECK(summary.title == "Rethinking the Recent area");
    CHECK(summary.meta == "2 screenshots · 1:46");
    CHECK(summary.pill == "AI write-up");
    CHECK(summary.words.find("recent area") != std::string::npos);
    REQUIRE_FALSE(summary.thumb.isNull());
    CHECK(summary.thumb.size() == past::THUMB_SIZE);
}

// from tests/test_past_walkthroughs.py::test_an_as_spoken_walkthrough_is_titled_by_its_first_words
TEST_CASE("an as spoken walkthrough is titled by its first words",
          "[test_past_walkthroughs][widget]") {
    fv::testkit::TempDataRoot tmp;
    std::string longWords;
    for (int i = 0; i < 40; ++i) {
        longWords += "word ";
    }
    const auto folder =
        walkthroughFolder(tmp.path() / "captures", "2026-10-25_09-00-00", {{longWords, false}});

    const past::Summary summary = past::summarize(folder, kToday);

    CHECK(summary.title.ends_with("…"));
    CHECK(QString::fromStdString(summary.title).toUcs4().size() <= past::TITLE_CHARS + 1);
    CHECK(summary.pill.empty());
    CHECK(summary.thumb.isNull());
}

// from tests/test_past_walkthroughs.py::test_an_interrupted_draft_says_so
TEST_CASE("an interrupted draft says so", "[test_past_walkthroughs][widget]") {
    fv::testkit::TempDataRoot tmp;
    const past::Summary summary =
        past::summarize(draftFolder(tmp.path() / "captures", "2026-10-24_09-00-00"), kToday);

    CHECK(summary.title == "Not finished");
    CHECK(summary.pill == "Interrupted");
    CHECK(summary.pill_tone == "warning");
}

// Not in Python's file: the cards and their signal, which test_cards_list_newest_first_... (the
// walkthrough integration's) checks with the library.
TEST_CASE("cards list newest first and say when", "[past_walkthroughs][widget]") {
    PanelRig rig;
    const auto older = walkthroughFolder(rig.root, "2026-10-24_09-00-00", {{"older one", true}});
    const auto newer = walkthroughFolder(rig.root, "2026-10-25_10-30-00", {{"newer one", false}});
    QSignalSpy spy(&rig.panel, &fv::PastWalkthroughsPanel::walkthroughSelected);

    rig.loaded();

    REQUIRE(rig.panel.cards().size() == 2);
    CHECK(rig.panel.cards()[0]->summary().folder == newer);
    CHECK(rig.panel.cards()[1]->summary().folder == older);
    CHECK(rig.panel.cards()[0]->whenLabel()->text() == QStringLiteral("Today · 10:30 AM"));
    CHECK(rig.panel.countHeader()->text() == QStringLiteral("2 walkthroughs"));
    QTest::mouseClick(rig.panel.cards()[1], Qt::LeftButton);
    QTest::keyClick(rig.panel.cards()[0], Qt::Key_Return);
    rig.panel.libraryButton()->click();
    REQUIRE(spy.count() == 3);
    CHECK(spy[0][0].toString() == QString::fromStdString(fv::path_to_utf8(older)));
    CHECK(spy[1][0].toString() == QString::fromStdString(fv::path_to_utf8(newer)));
    CHECK(spy[2][0].toString().isEmpty());
}

// from tests/test_past_walkthroughs.py::test_search_keeps_the_cards_holding_every_word
TEST_CASE("search keeps the cards holding every word", "[test_past_walkthroughs][widget]") {
    PanelRig rig;
    walkthroughFolder(rig.root, "2026-10-24_09-00-00", {{"the settings dialog is slow", true}});
    walkthroughFolder(rig.root, "2026-10-25_10-30-00", {{"the history panel", false}});
    rig.loaded();

    rig.panel.searchInput()->setText(QStringLiteral("History  PANEL"));
    REQUIRE(rig.panel.cards().size() == 2);
    CHECK(rig.panel.cards()[0]->isVisibleTo(&rig.panel));
    CHECK_FALSE(rig.panel.cards()[1]->isVisibleTo(&rig.panel));
    CHECK(rig.panel.countHeader()->text() == QStringLiteral("1 walkthrough"));

    rig.panel.searchInput()->setText(QStringLiteral("nothing like this"));
    CHECK(rig.panel.emptyLabel()->text() == QStringLiteral("No walkthrough says that"));
}

// from tests/test_past_walkthroughs.py::test_without_walkthroughs_the_page_says_so
TEST_CASE("without walkthroughs the page says so", "[test_past_walkthroughs][widget]") {
    PanelRig rig;
    rig.loaded();

    CHECK(rig.panel.cards().empty());
    CHECK(rig.panel.emptyLabel()->isVisibleTo(&rig.panel));
    CHECK(rig.panel.emptyLabel()->text().startsWith(QStringLiteral("Nothing yet")));
}

// from tests/test_past_walkthroughs.py::test_only_the_newest_are_built
TEST_CASE("only the newest are built", "[test_past_walkthroughs][widget]") {
    PanelRig rig;
    rig.panel.setMaxShown(2);
    for (int hour : {7, 8, 9}) {
        walkthroughFolder(rig.root, "2026-10-25_0" + std::to_string(hour) + "-00-00",
                          {{"at " + std::to_string(hour), false}});
    }

    rig.loaded();

    CHECK(rig.panel.cards().size() == 2);
    CHECK(rig.panel.countHeader()->text() == QStringLiteral("2+ walkthroughs"));
    CHECK(rig.panel.emptyLabel()->text().contains(QStringLiteral("all 3")));
}

// from tests/test_past_walkthroughs.py::test_a_stale_read_is_dropped
// (Python emits a stale token on the relay; here an older read comes back after a newer one was
// asked for)
TEST_CASE("a stale read is dropped", "[test_past_walkthroughs][widget]") {
    PanelRig rig;
    walkthroughFolder(rig.root, "2026-10-25_10-30-00", {{"one", false}});
    rig.loaded();
    REQUIRE(rig.panel.cards().size() == 1);
    std::filesystem::remove_all(rig.root / "2026-10-25_10-30-00");

    rig.panel.refresh(); // the read that goes stale
    rig.panel.refresh(); // a newer read is on its way
    rig.io.run_one();
    QApplication::processEvents();
    QApplication::processEvents();

    CHECK(rig.panel.cards().size() == 1);
}

// summarize turns only OSError, ValueError, KeyError and TypeError into a "Can't be opened" card.
// A section that is not an object fails load_document with AttributeError, which leaves summarize:
// the reader logs it and builds no card for that folder.
TEST_CASE("a folder summarize cannot read for another reason gets no card",
          "[past_walkthroughs][widget]") {
    PanelRig rig;
    walkthroughFolder(rig.root, "2026-10-24_09-00-00", {{"kept", false}});
    const auto odd = rig.root / "2026-10-25_09-00-00";
    std::filesystem::create_directories(odd);
    writeBytes(odd / "walkthrough.json",
               R"({"version": 1, "started_at": "2026-10-25T09:00:00", "duration_s": 1.0,)"
               R"( "sections": [1]})");
    const auto broken = rig.root / "2026-10-25_10-00-00";
    std::filesystem::create_directories(broken);
    writeBytes(broken / "walkthrough.json", "{broken");

    CHECK_THROWS_AS(past::summarize(odd, kToday), fv::py::PyError);
    CHECK(past::summarize(broken, kToday).title == "Can't be opened");

    rig.loaded();
    REQUIRE(rig.panel.cards().size() == 2);
    CHECK(rig.panel.cards()[0]->summary().folder == broken);
    CHECK(rig.panel.cards()[1]->summary().title == "kept");
    CHECK(rig.panel.total() == 3);
}

// The thumbnail is decoded at the strip's size (cover-scaled, never at full size), cropped from
// the top and centred across.
TEST_CASE("a thumbnail covers its strip from the top, centred across", "[past_walkthroughs]") {
    fv::testkit::TempDataRoot tmp;
    QImage image(1000, 1000, QImage::Format_RGB32);
    image.fill(QColor(Qt::white));
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 1000; ++x) {
            image.setPixelColor(x, y, QColor(Qt::red));
        }
    }
    const auto path = tmp.path() / "tall.png";
    REQUIRE(image.save(QString::fromStdString(fv::path_to_utf8(path))));

    const QImage thumb = past::thumb(path, 1.0);

    REQUIRE(thumb.size() == past::THUMB_SIZE);
    // 1000 px scaled to 296 wide: the red band, 100 px, is 29 px tall at the top.
    CHECK(thumb.pixelColor(148, 10) == QColor(Qt::red));
    CHECK(thumb.pixelColor(148, 60) == QColor(Qt::white));
    CHECK(thumb.pixelColor(0, 0).alpha() < 255); // a rounded corner
    CHECK(past::thumb(tmp.path() / "missing.png", 1.0).isNull());
}

// ---- golden vectors ----

TEST_CASE("past walkthroughs: card titles match OpenWhisper", "[golden][past_walkthroughs]") {
    fv::golden::for_each(
        "ui_qt.widgets.past_walkthroughs", "_title", [](const fv::golden::Case& c) {
            fv::testkit::TempDataRoot tmp;
            const Json markdown = c.arg<Json>(0);
            if (markdown.is_string()) {
                writeBytes(tmp.path() / "write-up.md", markdown.get<std::string>());
            } else if (markdown.is_object()) {
                const std::string hex = markdown.at("hex").get<std::string>();
                std::string bytes;
                for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                    bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
                }
                writeBytes(tmp.path() / "write-up.md", bytes);
            }
            fv::walkthrough::Walkthrough document;
            for (const std::string& text : c.arg<std::vector<std::string>>(1)) {
                fv::walkthrough::Section section;
                section.text = text;
                document.sections.push_back(section);
            }
            CHECK(past::title(tmp.path(), document) == c.result<std::string>());
        });
}

TEST_CASE("past walkthroughs: summaries match OpenWhisper", "[golden][past_walkthroughs]") {
    fv::golden::for_each(
        "ui_qt.widgets.past_walkthroughs", "summarize", [](const fv::golden::Case& c) {
            const Json spec = c.arg<Json>(0);
            fv::testkit::TempDataRoot tmp;
            const auto folder = tmp.path() / spec.at("name").get<std::string>();
            std::filesystem::create_directories(folder);
            const Json manifest = spec.value("manifest", Json());
            if (manifest.is_object()) {
                writeBytes(folder / "walkthrough.json", manifest.dump());
            } else if (manifest.is_string()) {
                writeBytes(folder / "walkthrough.json", manifest.get<std::string>());
            }
            if (spec.contains("draft") && spec["draft"].is_string()) {
                writeBytes(folder / "draft.json", spec["draft"].get<std::string>());
            }
            if (spec.contains("write_up") && spec["write_up"].is_string()) {
                writeBytes(folder / "write-up.md", spec["write_up"].get<std::string>());
            }
            if (spec.value("write_up_pdf", false)) {
                writeBytes(folder / "write-up.pdf", "%PDF-1.4");
            }
            int number = 1;
            for (const Json& size : spec.value("shots", Json::array())) {
                QImage image(size[0].get<int>(), size[1].get<int>(), QImage::Format_RGB32);
                image.fill(QColor(QStringLiteral("#3a6ea5")));
                image.save(QString::fromStdString(
                    fv::path_to_utf8(folder / ("shot-" + std::to_string(number++) + ".png"))));
            }
            const past::Summary summary = past::summarize(folder, kToday);
            const Json got{
                {"title", summary.title},
                {"meta", summary.meta},
                {"pill", summary.pill},
                {"pill_tone", summary.pill_tone},
                {"words", summary.words},
                {"when", summary.when ? Json(wall(*summary.when)) : Json()},
                {"thumb", summary.thumb.isNull()
                              ? Json()
                              : Json::array({summary.thumb.width(), summary.thumb.height()})},
            };
            CHECK(fv::golden::equal(c.result<Json>(), got));
        });
}

TEST_CASE("past walkthroughs: the search filter matches OpenWhisper",
          "[golden][past_walkthroughs]") {
    fv::golden::for_each(
        "ui_qt.widgets.past_walkthroughs", "_filter", [](const fv::golden::Case& c) {
            const auto words = c.arg<std::vector<std::string>>(0);
            const auto search = c.arg<std::string>(1);
            const auto total = c.arg<std::optional<int>>(2);
            const past::FilterView view = past::filter(words, search, total);
            // Before the first read the lines keep what the page says while it loads.
            const Json got{
                {"visible", view.visible},
                {"count", view.counted ? view.count : std::string()},
                {"count_shown", view.counted && view.count_shown},
                {"message", view.counted ? view.message : std::string("Loading walkthroughs…")},
                {"message_shown", !view.counted || view.message_shown},
            };
            CHECK(fv::golden::equal(c.result<Json>(), got));
        });
}

} // namespace fvtest
