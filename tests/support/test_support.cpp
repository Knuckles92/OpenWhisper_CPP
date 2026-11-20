// Self-tests of the test support libraries: fv_testkit, fv_golden and (with Qt) fv_screenshot.
#include "golden/golden.h"
#include "testkit/testkit.h"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef FV_SUPPORT_SCREENSHOT
#include "screenshot/screenshot.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>
#endif

using fv::Json;
namespace golden = fv::golden;
namespace testkit = fv::testkit;

namespace {

// services/format_utils.py format_audio_duration, with the rounding of int(round(value)) as a
// parameter: the a33 replay's two ports.
std::string format_audio_duration(const Json& seconds,
                                  const std::function<long long(double)>& round_to_int) {
    std::optional<double> value;
    if (seconds.is_number()) {
        value = seconds.get<double>();
    } else if (seconds.is_string()) {
        // float("12.5"); anything float() rejects returns "0.0s".
        const auto& text = seconds.get_ref<const std::string&>();
        double parsed = 0.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error == std::errc() && end == text.data() + text.size()) {
            value = parsed;
        }
    }
    if (!value) {
        return "0.0s";
    }
    const double v = *value < 0 ? 0.0 : *value;
    if (v < 60) {
        return std::format("{:.1f}s", v);
    }
    const long long total = round_to_int(v);
    const long long hours = total / 3600;
    const long long minutes = total % 3600 / 60;
    const long long secs = total % 60;
    if (hours > 0) {
        return minutes != 0 ? std::format("{}h {}m", hours, minutes) : std::format("{}h", hours);
    }
    return std::format("{}m {}s", minutes, secs);
}

// Python's round(): ties to even, which nearbyint does in the default rounding mode.
long long half_to_even(double v) {
    return static_cast<long long>(std::nearbyint(v));
}

// What a literal translation reaches for, and what the vectors exist to catch.
long long half_away_from_zero(double v) {
    return static_cast<long long>(v < 0 ? std::ceil(v - 0.5) : std::floor(v + 0.5));
}

const Json kSynthetic = Json::parse(R"({
 "module": "synthetic",
 "float_tolerance": 1e-06,
 "functions": {
  "f": [
   {"args": [1, 2.5, "x", null, true, [1, 2]], "result": {"$float": "nan"}},
   {"args": [{"$float": "-inf"}], "kwargs": {"k": 3}, "raises": "ValueError", "traced": true}
  ]
 }
})");

} // namespace

TEST_CASE("golden loader replays services.format_utils through a half-to-even port", "[golden]") {
    golden::for_each("services.format_utils", "format_audio_duration", [](const golden::Case& c) {
        REQUIRE_FALSE(c.raises());
        CHECK(format_audio_duration(c.arg<Json>(0), half_to_even) == c.result<std::string>());
    });
}

TEST_CASE("golden vectors catch a port that rounds half away from zero", "[golden]") {
    const Json& cases = golden::load("services.format_utils")["functions"]["format_audio_duration"];
    std::vector<std::string> mismatches;
    for (const auto& c : cases) {
        if (format_audio_duration(c["args"][0], half_away_from_zero) !=
            c["result"].get<std::string>()) {
            mismatches.push_back(c["args"].dump());
        }
    }
    // round(62.5) is 62 in Python; half away from zero gives 63.
    CHECK(mismatches == std::vector<std::string>{"[62.5]"});
}

TEST_CASE("golden vector files carry their module, commit and pins", "[golden]") {
    for (const char* module : {"services.format_utils", "ui_qt.utils.fuzzy_match"}) {
        const Json& document = golden::load(module);
        CHECK(document["module"] == module);
        CHECK(document["source_commit"] == "768b559edd932d40806c96eed05ef5a20e57e415");
        CHECK(document["pins"]["python"] == "3.12.10");
        CHECK(document["traced_from"].size() == 1);
    }
    const Json& fuzzy = golden::load("ui_qt.utils.fuzzy_match")["functions"]["fuzzy_score"];
    bool folded = false;
    for (const auto& c : fuzzy) {
        if (c["args"] == Json::array({"straße", "STRASSE"})) {
            folded = c["result"] == 10000;
        }
    }
    CHECK(folded);
}

TEST_CASE("golden cases decode arguments, tags, kwargs and raises", "[golden]") {
    std::vector<std::string> labels;
    golden::for_each_in(kSynthetic, "f", [&](const golden::Case& c) {
        labels.push_back(c.label());
        CHECK(c.float_tolerance() == 1e-06);
        if (!c.raises()) {
            CHECK(c.arg_count() == 6);
            CHECK(c.arg<int>(0) == 1);
            CHECK(c.arg<double>(0) == 1.0);
            CHECK(c.arg<double>(1) == 2.5);
            CHECK(c.arg<std::string>(2) == "x");
            CHECK(c.arg<std::optional<int>>(3) == std::nullopt);
            CHECK(c.arg<bool>(4));
            CHECK(c.arg<std::vector<int>>(5) == std::vector<int>{1, 2});
            CHECK(std::isnan(c.result<double>()));
            CHECK_FALSE(c.traced());
            CHECK(c.kwargs().empty());
            CHECK_THROWS_AS(c.arg<int>(1), golden::DecodeError);
            CHECK_THROWS_AS(c.arg<std::string>(0), golden::DecodeError);
            CHECK_THROWS_AS(c.arg<int>(4), golden::DecodeError);
            CHECK_THROWS_AS(c.arg<int>(6), golden::DecodeError);
        } else {
            CHECK(c.raises_type() == "ValueError");
            CHECK(c.arg<double>(0) == -INFINITY);
            CHECK(c.kwargs()["k"] == 3);
            CHECK(c.traced());
            CHECK_THROWS_AS(c.result<double>(), golden::DecodeError);
        }
    });
    // Catch2 runs one section per pass, so each pass records the case it entered.
    CHECK(labels.size() == 1);
}

TEST_CASE("golden for_each fails on a function without vectors", "[golden][!shouldfail]") {
    golden::for_each_in(kSynthetic, "missing", [](const golden::Case&) {});
}

TEST_CASE("golden comparators follow Python equality and tolerances", "[golden]") {
    CHECK(golden::equal(Json(1), Json(1.0)));
    CHECK_FALSE(golden::equal(Json(true), Json(1)));
    CHECK(golden::equal(Json::parse(R"({"$float": "nan"})"),
                        Json(std::numeric_limits<double>::quiet_NaN())));
    CHECK(golden::equal(Json::parse(R"([1, {"a": "b"}])"), Json::parse(R"([1.0, {"a": "b"}])")));
    CHECK_FALSE(golden::equal(Json::parse(R"({"a": 1})"), Json::parse(R"({"a": 1, "b": 2})")));
    CHECK_FALSE(golden::equal(Json(0.1), Json(0.1000001)));
    CHECK(golden::matches(Json(0.1), Json(0.1000001), 1e-6));
    CHECK_FALSE(golden::matches(Json(0.1), Json(0.10001), 1e-6));
    CHECK(golden::near(INFINITY, INFINITY, 0.0));
    CHECK_FALSE(golden::near(INFINITY, -INFINITY, 1e9));
    CHECK(golden::equal_rebranded("OpenWhisper is ready", "FioriVoce is ready"));
}

TEST_CASE("rebrand maps every OpenWhisper name to FioriVoce's", "[golden]") {
    CHECK(golden::rebrand("OpenWhisper") == "FioriVoce");
    CHECK(golden::rebrand("Restart OpenWhisper to finish.") == "Restart FioriVoce to finish.");
    CHECK(golden::rebrand("openwhisper_settings.json") == "fiorivoce_settings.json");
    CHECK(golden::rebrand("data/openwhisper.db") == "data/fiorivoce.db");
    CHECK(golden::rebrand("openwhisper.log") == "fiorivoce.log");
    CHECK(golden::rebrand(".openwhisper-data.lock") == ".fiorivoce-data.lock");
    CHECK(golden::rebrand(".openwhisper-restore") == ".fiorivoce-restore");
    CHECK(golden::rebrand("openwhisper-backup") == "fiorivoce-backup");
    CHECK(golden::rebrand("x.owbackup") == "x.fvbackup");
    CHECK(golden::rebrand("openwhisper-ui-1") == "fiorivoce-ui-1");
    CHECK(golden::rebrand("OPENWHISPER_DATA_DIR") == "FIORIVOCE_DATA_DIR");
    CHECK(golden::rebrand("Whisper large-v3 é") == "Whisper large-v3 é");
}

TEST_CASE("TempDataRoot sets FIORIVOCE_DATA_DIR and restores it", "[support]") {
    const auto before = testkit::get_env("FIORIVOCE_DATA_DIR");
    std::filesystem::path folder;
    {
        testkit::TempDataRoot root;
        folder = root.path();
        CHECK(std::filesystem::is_directory(folder));
        CHECK(std::filesystem::is_empty(folder));
        const auto utf8 = folder.u8string();
        CHECK(testkit::get_env("FIORIVOCE_DATA_DIR") == std::string(utf8.begin(), utf8.end()));
        testkit::TempDataRoot other;
        CHECK(other.path() != folder);
    }
    CHECK_FALSE(std::filesystem::exists(folder));
    CHECK(testkit::get_env("FIORIVOCE_DATA_DIR") == before);
}

TEST_CASE("testkit paths point into the checkout", "[support]") {
    CHECK(std::filesystem::is_regular_file(testkit::golden_path("services.format_utils")));
    CHECK(std::filesystem::is_directory(testkit::reference_path() / "app_stylesheet"));
    CHECK(std::filesystem::is_regular_file(testkit::parity_reference_file()));
    CHECK(std::filesystem::is_directory(testkit::artifact_path()));
}

TEST_CASE("RealtimeGuard counts heap allocations in its scope", "[support]") {
    if (!testkit::RealtimeGuard::enforced()) {
        SKIP("RealtimeGuard is enforced only in builds without NDEBUG");
    }
    // Read inside the scope, checked outside it: a Catch2 assertion may allocate.
    std::size_t at_start = 0;
    std::size_t after_two = 0;
    {
        testkit::RealtimeGuard guard(false);
        at_start = guard.allocations();
        auto value = std::make_unique<int>(7);
        auto values = std::make_unique<int[]>(4);
        after_two = guard.allocations();
        values[0] = *value;
    }
    CHECK(at_start == 0);
    CHECK(after_two == 2);
}

#ifndef NDEBUG
TEST_CASE("RealtimeGuard fails a test that allocates", "[support][!shouldfail]") {
    testkit::RealtimeGuard guard;
    auto value = std::make_unique<long long>(1);
    CHECK(*value == 1);
}
#endif

#ifdef FV_SUPPORT_SCREENSHOT
namespace screenshot = fv::screenshot;

TEST_CASE("tolerance files need a pixel count and a reason", "[widget]") {
    const auto tolerance =
        screenshot::parse_tolerance(R"({"max_differing_pixels": 12, "reason": "kerning"})");
    CHECK(tolerance.max_differing_pixels == 12);
    CHECK(tolerance.max_channel_delta == 255);
    CHECK(tolerance.reason == "kerning");
    CHECK(screenshot::parse_tolerance(
              R"({"max_differing_pixels": 0, "max_channel_delta": 3, "reason": "x"})")
              .max_channel_delta == 3);
    CHECK_THROWS(screenshot::parse_tolerance(R"({"max_differing_pixels": 3})"));
    CHECK_THROWS(screenshot::parse_tolerance(R"({"max_differing_pixels": 3, "reason": " "})"));
    CHECK_THROWS(screenshot::parse_tolerance(R"({"max_differing_pixels": -1, "reason": "x"})"));
    CHECK_THROWS(screenshot::parse_tolerance(
        R"({"max_differing_pixels": 1, "max_channel_delta": 300, "reason": "x"})"));
    CHECK_THROWS(screenshot::parse_tolerance("[]"));
    const auto none = screenshot::load_tolerance("empty_state_harness", "dark-windows11-dpr1-s100");
    CHECK(none.max_differing_pixels == 0);
}

TEST_CASE("Qt kit families come from the version", "[widget]") {
    CHECK(screenshot::qt_family("6.10.3") == "6.10");
    CHECK(screenshot::qt_family("6.11") == "6.11");
    CHECK(screenshot::qt_family("7") == "7");
}

TEST_CASE("image comparison counts differing pixels and paints them magenta", "[widget]") {
    QImage reference(4, 3, QImage::Format_ARGB32);
    reference.fill(qRgb(10, 20, 30));
    QImage actual = reference;
    actual.setPixel(2, 1, qRgb(10, 26, 30));
    const auto diff = screenshot::compare_images(reference, actual);
    CHECK(diff.same_size);
    CHECK(diff.differing == 1);
    CHECK(diff.max_channel_delta == 6);
    CHECK(diff.diff.pixel(2, 1) == qRgb(255, 0, 255));
    CHECK(diff.diff.pixel(0, 0) != qRgb(255, 0, 255));
    CHECK_FALSE(
        screenshot::compare_images(reference, QImage(5, 3, QImage::Format_RGB32)).same_size);
}

TEST_CASE("dump_tree writes the Python dump's schema for a C++ widget", "[widget]") {
    QFrame frame;
    frame.setObjectName(QStringLiteral("emptyState"));
    frame.setProperty("tone", QStringLiteral("neutral"));
    frame.setProperty("selected", true);
    auto* layout = new QVBoxLayout(&frame);
    auto* label = new QLabel(QStringLiteral("No snippets yet"));
    label->setObjectName(QStringLiteral("emptyStateTitle"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* button = new QPushButton(QStringLiteral("Add snippet"));
    layout->addWidget(button);
    frame.setFixedSize(200, 100);

    const Json tree = screenshot::dump_tree(frame);
    CHECK(tree["class"] == "QFrame");
    CHECK(tree["qtClass"] == "QFrame");
    CHECK(tree["objectName"] == "emptyState");
    CHECK(tree["properties"] == Json::parse(R"({"tone": "neutral", "selected": "True"})"));
    CHECK(tree["visible"] == false);
    CHECK_FALSE(tree.contains("text"));
    REQUIRE(tree["children"].size() == 2);
    const Json& title = tree["children"][0];
    CHECK(title["text"] == "No snippets yet");
    CHECK(title["alignment"] == 132);
    CHECK(title["wordWrap"] == true);
    CHECK_FALSE(title.contains("isChecked"));
    const Json& add = tree["children"][1];
    CHECK(add["isChecked"] == false);
    CHECK(add["isDefault"] == false);
    CHECK_FALSE(add.contains("wordWrap"));
    std::vector<std::string> keys;
    for (const auto& item : add.items()) {
        keys.push_back(item.key());
    }
    CHECK(keys == std::vector<std::string>{"class", "qtClass", "objectName", "geometry", "visible",
                                           "hasFocus", "properties", "text", "isChecked",
                                           "isDefault", "font", "children"});

    Json moved = tree;
    moved["children"][0]["geometry"][1] = 99;
    moved["children"][1]["objectName"] = "addButton";
    const auto differences = screenshot::compare_trees(moved, tree);
    REQUIRE(differences.size() == 2);
    CHECK(differences[0].starts_with("emptyState/emptyStateTitle: geometry expected"));
    CHECK(differences[1].starts_with("emptyState/addButton: objectName expected \"addButton\""));
    CHECK(screenshot::compare_trees(tree, tree).empty());
}

TEST_CASE("fv_screenshot rebuilds the EmptyState harness reference at 0 differing pixels",
          "[screenshot]") {
    const Json pins = screenshot::load_reference("empty_state_harness", "dark-windows11-dpr1-s100");
    CHECK(pins["qt"] == "6.11.0");
    CHECK(pins["base_style"] == "QWindows11Style");
    screenshot::ScopedReferenceTheme theme(pins);
    auto rebuilt = screenshot::rebuild_tree(pins.at("tree"));
    REQUIRE(rebuilt.focus != nullptr);
    // The rebuilt widgets are plain Qt classes, so only "class" (EmptyState, WrappedLabel) is
    // left out of the tree comparison.
    screenshot::expect_matches(*rebuilt.root, "empty_state_harness", "dark-windows11-dpr1-s100",
                               {.focus = rebuilt.focus, .ignore_keys = {"class"}});
}

TEST_CASE("the reference theme installs every palette group and restores the previous one",
          "[widget]") {
    const QPalette before = QApplication::palette();
    const QString sheet_before = qApp->styleSheet();
    {
        const Json pins =
            screenshot::load_reference("empty_state_harness", "dark-windows11-dpr1-s100");
        screenshot::ScopedReferenceTheme theme(pins);
        const QPalette palette = QApplication::palette();
        // ThemeManager's build_qpalette: text-muted for disabled text, text elsewhere.
        CHECK(palette.color(QPalette::Active, QPalette::WindowText) == QColor(0xf5, 0xf5, 0xf7));
        CHECK(palette.color(QPalette::Inactive, QPalette::WindowText) == QColor(0xf5, 0xf5, 0xf7));
        CHECK(palette.color(QPalette::Disabled, QPalette::WindowText) == QColor(0x63, 0x63, 0x66));
        CHECK(palette.color(QPalette::Disabled, QPalette::ButtonText) == QColor(0x63, 0x63, 0x66));
        CHECK(palette.color(QPalette::Disabled, QPalette::Button) ==
              palette.color(QPalette::Active, QPalette::Button));
        CHECK(qApp->styleSheet().size() > 100000);
    }
    CHECK(QApplication::palette().color(QPalette::Disabled, QPalette::WindowText) ==
          before.color(QPalette::Disabled, QPalette::WindowText));
    CHECK(qApp->styleSheet() == sheet_before);
}

TEST_CASE("expect_matches fails when the application stylesheet is not the reference's",
          "[screenshot][!shouldfail]") {
    Json pins = screenshot::load_reference("empty_state_harness", "dark-windows11-dpr1-s100");
    // The 130 % sheet for a reference rendered at 100 %.
    pins["font_scale_percent"] = 130;
    screenshot::ScopedReferenceTheme theme(pins);
    auto rebuilt = screenshot::rebuild_tree(pins.at("tree"));
    screenshot::expect_matches(*rebuilt.root, "empty_state_harness", "dark-windows11-dpr1-s100",
                               {.focus = rebuilt.focus, .ignore_keys = {"class", "font"}});
}

TEST_CASE("the same tree without the application stylesheet does not match the reference",
          "[screenshot]") {
    std::string why_not;
    if (!screenshot::pixel_comparison_enabled() ||
        !screenshot::qt_kit_matches_reference(&why_not)) {
        SKIP("pixels are not compared in this build " << why_not);
    }
    const Json pins = screenshot::load_reference("empty_state_harness", "dark-windows11-dpr1-s100");
    auto rebuilt = screenshot::rebuild_tree(pins.at("tree"));
    rebuilt.root->show();
    rebuilt.focus->setFocus();
    QCoreApplication::processEvents();
    const QImage reference(QString::fromStdWString(
        (fv::testkit::reference_path() / "empty_state_harness" / "dark-windows11-dpr1-s100.png")
            .wstring()));
    const auto diff = screenshot::compare_images(reference, rebuilt.root->grab().toImage());
    CHECK(diff.same_size);
    CHECK(diff.differing > 1000);
}
#endif
