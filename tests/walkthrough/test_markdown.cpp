// from tests/test_walkthrough_document.py (the Markdown and manifest half)
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "base/identity.h"
#include "golden/golden.h"
#include "pyport/json.h"
#include "support.h"
#include "walkthrough/document.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using namespace std::chrono_literals;

namespace {

const std::chrono::time_zone* pacific() {
    return std::chrono::locate_zone("America/Los_Angeles");
}

// A Tuesday afternoon.
std::chrono::zoned_seconds started() {
    return local_moment(pacific(), std::chrono::local_days{std::chrono::year{2026} / 10 / 20} +
                                       14h + 32min + 5s);
}

std::string about_shotless() {
    return std::string("Spoken notes with screenshots, transcribed by ") +
           std::string(fv::identity().product) + ".";
}

std::string about() {
    return about_shotless() + " Each screenshot comes right after the words that describe it.";
}

Section words(std::string text) {
    return Section{std::move(text), std::nullopt, std::nullopt, std::nullopt, {}};
}

Section shot(std::string text, std::filesystem::path path, std::optional<double> kept_at_s) {
    return Section{std::move(text), std::move(path), kept_at_s, std::nullopt, {}};
}

bool ends_with(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find('\n', start);
        out.push_back(
            text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return out;
        }
        start = end + 1;
    }
}

Walkthrough one_shot(const TempFolder& tmp) {
    return {
        started(),
        136.4,
        {shot("The update card is grayed out.", tmp / "shot-1.png", 41.3), words("That's it.")}};
}

} // namespace

// --- Markdown ---------------------------------------------------------------------------------

TEST_CASE("each screenshot follows its words", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const Walkthrough walkthrough{
        started(),
        136.4,
        {shot("Okay, the update card is grayed out.", tmp / "shot-1.png", 41.3),
         shot("Next, this label is stale.", tmp / "shot-2.png", 72.6), words("That's it.")}};

    CHECK(markdown(walkthrough, tmp.path(), false) ==
          "# Walkthrough\n"
          "\n"
          "_Tue, Oct 20, 2026, 2:32 PM · 2:16 of speech · 2 screenshots. " +
              about() +
              "_\n"
              "\n"
              "Okay, the update card is grayed out.\n"
              "\n"
              "![Screenshot 1 (0:41)](shot-1.png)\n"
              "\n"
              "Next, this label is stale.\n"
              "\n"
              "![Screenshot 2 (1:12)](shot-2.png)\n"
              "\n"
              "That's it.\n");
}

TEST_CASE("absolute links keep a folder with spaces in one piece", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const auto folder = tmp / "Big D" / "captures" / "2026-10-20_14-32-05";
    const auto path = folder / "shot-1.png";
    const Walkthrough walkthrough{started(), 20, {shot("Look here.", path, 12.0)}};

    CHECK(markdown(walkthrough, folder, true) ==
          "# Walkthrough\n"
          "\n"
          "_Tue, Oct 20, 2026, 2:32 PM · 0:20 of speech · 1 screenshot. " +
              about() +
              "_\n"
              "\n"
              "Look here.\n"
              "\n"
              "![Screenshot 1 (0:12)](<" +
              fv::path_to_utf8(path) + ">)\n");
    // The platform's own separators, as a terminal agent would type it.
    const std::string sep(1, static_cast<char>(std::filesystem::path::preferred_separator));
    CHECK(fv::path_to_utf8(path).find(sep + "Big D" + sep + "captures" + sep) != std::string::npos);
}

TEST_CASE("a relative link with a space is bracketed too", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const Walkthrough walkthrough{started(), 20, {shot("", tmp / "shots" / "shot 1.png", 3.0)}};

    const auto text = markdown(walkthrough, tmp.path(), false);

    CHECK(ends_with(text, "\n![Screenshot 1 (0:03)](<shots/shot 1.png>)\n"));
}

TEST_CASE("a screenshot outside the folder keeps its full path", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const auto path = tmp / "elsewhere" / "shot-1.png";
    const Walkthrough walkthrough{started(), 20, {shot("", path, 3.0)}};

    const auto text = markdown(walkthrough, tmp / "capture", false);

    CHECK(ends_with(text, "\n![Screenshot 1 (0:03)](<" + fv::path_to_utf8(path) + ">)\n"));
}

TEST_CASE("speech never turns into markdown structure", "[test_walkthrough_document]") {
    const auto [spoken, written] = GENERATE(table<std::string, std::string>({
        {"# one thing", "\\# one thing"},
        {"#hashtag", "\\#hashtag"},
        {"> quoted", "\\> quoted"},
        {"- dash", "\\- dash"},
        {"+ plus", "\\+ plus"},
        {"* star", "\\* star"},
        {"1. First", "1\\. First"},
        {"12) Twelfth", "12\\) Twelfth"},
        {"1.5 seconds in", "1.5 seconds in"},
        {"Okay # not a heading - not a list", "Okay # not a heading - not a list"},
        {"Line one\n- line two\n  3. line three", "Line one\n\\- line two\n  3\\. line three"},
    }));
    const TempFolder tmp;
    const Walkthrough walkthrough{started(), 10, {words(spoken)}};

    CHECK(ends_with(markdown(walkthrough, tmp.path(), false), "_\n\n" + written + "\n"));
}

TEST_CASE("the meta line counts screenshots", "[test_walkthrough_document]") {
    const auto [shots, count, with_shots] = GENERATE(table<int, std::string, bool>({
        {0, "no screenshots", false},
        {1, "1 screenshot", true},
        {3, "3 screenshots", true},
    }));
    const TempFolder tmp;
    Walkthrough walkthrough{started(), 75, {}};
    for (int number = 1; number <= shots; ++number) {
        walkthrough.sections.push_back(shot("Words " + std::to_string(number) + ".",
                                            tmp / ("shot-" + std::to_string(number) + ".png"),
                                            static_cast<double>(number)));
    }
    walkthrough.sections.push_back(words("The end."));

    const auto text = lines(markdown(walkthrough, tmp.path(), false));

    const std::string meta = "Tue, Oct 20, 2026, 2:32 PM · 1:15 of speech · " + count;
    CHECK(text.at(2) == "_" + meta + ". " + (with_shots ? about() : about_shotless()) + "_");
}

TEST_CASE("times read as minutes and seconds or hours", "[test_walkthrough_document]") {
    const auto [seconds, expected] = GENERATE(table<double, std::string>({
        {0, "0:00"},
        {9.99, "0:09"},
        {136.4, "2:16"},
        {3599.9, "59:59"},
        {3600, "1:00:00"},
        {3725, "1:02:05"},
        {-1, "0:00"},
    }));
    const TempFolder tmp;
    const Walkthrough walkthrough{started(), seconds, {shot("Look.", tmp / "shot-1.png", seconds)}};

    const auto text = markdown(walkthrough, tmp.path(), false);

    CHECK(text.find(" · " + expected + " of speech · ") != std::string::npos);
    CHECK(text.find("![Screenshot 1 (" + expected + ")](shot-1.png)") != std::string::npos);
}

TEST_CASE("empty sections are dropped and screenshots numbered in order",
          "[test_walkthrough_document]") {
    const TempFolder tmp;
    const Walkthrough walkthrough{started(),
                                  30,
                                  {words("   "), shot("", tmp / "shot-a.png", 5.0), words("\n"),
                                   shot("Then this.", tmp / "shot-b.png", std::nullopt),
                                   words("")}};

    const auto text = markdown(walkthrough, tmp.path(), false);
    const auto body = text.substr(text.find("_\n\n") + 3);

    CHECK(body == "![Screenshot 1 (0:05)](shot-a.png)\n"
                  "\n"
                  "Then this.\n"
                  "\n"
                  "![Screenshot 2](shot-b.png)\n");
}

TEST_CASE("markdown file is utf8 with lf line endings", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const std::string spoken = "Café, line one\r\nline two\r\rafter a blank line";
    const Walkthrough walkthrough{started(), 30, {shot(spoken, tmp / "shot-1.png", 3.0)}};

    const auto path = write_markdown(walkthrough, tmp.path());

    const auto data = read_file(path);
    CHECK(path == tmp / "walkthrough.md");
    CHECK(data.find('\r') == std::string::npos);
    CHECK(data == markdown(walkthrough, tmp.path(), false));
    CHECK(data.find("Café, line one\nline two\n\nafter a blank line\n") != std::string::npos);
}

// --- Manifest ---------------------------------------------------------------------------------

TEST_CASE("manifest keeps the sections for a later write up", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const Walkthrough walkthrough{
        started(),
        136.4,
        {shot(" Okay, look here. ", tmp / "shot-1.png", 41.3), words(""), words("That's it.")}};

    const auto path = write_manifest(walkthrough, tmp.path());

    const auto bytes = read_file(path);
    const auto manifest = fv::py::loads(bytes);
    CHECK(path == tmp / "walkthrough.json");
    CHECK(bytes.find('\r') == std::string::npos);
    CHECK(manifest.at("version") == 1);
    // The local time with its offset (Pacific Daylight Time in October).
    CHECK(manifest.at("started_at") == "2026-10-20T14:32:05-07:00");
    CHECK(manifest.at("duration_s").get<double>() == 136.4);
    CHECK(fv::golden::equal(fv::py::loads(R"([
        {"text": "Okay, look here.", "shot": "shot-1.png", "kept_at_s": 41.3},
        {"text": "That's it.", "shot": null, "kept_at_s": null}])"),
                            manifest.at("sections")));
}

// --- Format -----------------------------------------------------------------------------------

TEST_CASE("markdown without a header starts with the words", "[test_walkthrough_document]") {
    const TempFolder tmp;
    const DocumentFormat bare{"", false, false, false};

    CHECK(markdown(one_shot(tmp), tmp.path(), false, bare) == "The update card is grayed out.\n"
                                                              "\n"
                                                              "![Screenshot 1 (0:41)](shot-1.png)\n"
                                                              "\n"
                                                              "That's it.\n");
}

TEST_CASE("markdown keeps the header lines chosen", "[test_walkthrough_document]") {
    const auto [details, show_about] = GENERATE(table<bool, bool>({{true, false}, {false, true}}));
    const TempFolder tmp;
    const DocumentFormat chosen{"Bug report", details, show_about, true};
    const std::string line = details
                                 ? "_Tue, Oct 20, 2026, 2:32 PM · 2:16 of speech · 1 screenshot_"
                                 : "_" + about() + "_";

    const auto text = lines(markdown(one_shot(tmp), tmp.path(), false, chosen));

    REQUIRE(text.size() >= 4);
    CHECK(std::vector<std::string>(text.begin(), text.begin() + 4) ==
          std::vector<std::string>{"# Bug report", "", line, ""});
}

// --- beyond OpenWhisper's tests ------------------------------------------------------------------

TEST_CASE("a write-up's shared helpers number, tidy and split as the documents do", "[markdown]") {
    const TempFolder tmp;
    const Walkthrough walkthrough{
        started(), 30, {words("  "), shot("a\r\nb", tmp / "x.png", 1.0), words("tail")}};

    const auto written = parts(walkthrough);

    REQUIRE(written.size() == 2);
    CHECK(written[0] == Part{"a\nb", tmp / "x.png", 1, 1.0});
    CHECK(written[1] == Part{"tail", std::nullopt, 0, std::nullopt});
    CHECK(shot_count(written) == 1);
    CHECK(paragraphs("one\n \n two \n\n\nthree") ==
          std::vector<std::string>{"one", "two", "three"});
    CHECK(shot_label(written[0], " · ") == "Screenshot 1 · 0:01");
    CHECK(header_lines(walkthrough, 1, {"", false, false, true}).empty());
    CHECK(shot_name(std::nullopt, tmp.path()) == std::nullopt);
    CHECK(shot_name(tmp / "x.png", tmp.path()) == "x.png");
}

TEST_CASE("a skipped or repeated local hour reads as Python's astimezone does", "[markdown]") {
    // 02:30 on 8 March 2026 does not exist in Los Angeles; Python reads it as 03:30 PDT.
    const auto skipped = local_moment(
        pacific(), std::chrono::local_days{std::chrono::year{2026} / 3 / 8} + 2h + 30min);
    CHECK(skipped.get_local_time() ==
          std::chrono::local_days{std::chrono::year{2026} / 3 / 8} + 3h + 30min);
    CHECK(skipped.get_info().offset == -7h);
    // 01:30 on 1 November 2026 happens twice; fold 0 is the first, still daylight time.
    const auto repeated = local_moment(
        pacific(), std::chrono::local_days{std::chrono::year{2026} / 11 / 1} + 1h + 30min);
    CHECK(repeated.get_info().offset == -7h);
}

TEST_CASE("a manifest keeps omitted screenshots and held notes", "[markdown]") {
    const TempFolder tmp;
    Walkthrough walkthrough{started(), 10.0, {}};
    walkthrough.sections.push_back(
        Section{"", tmp / "shot-1.png", 0.5, tmp / "shot-2.png", {"make it purple"}});
    walkthrough.sections.push_back(Section{"", std::nullopt, std::nullopt, std::nullopt, {"note"}});

    const auto manifest = fv::py::loads(read_file(write_manifest(walkthrough, tmp.path())));

    CHECK(fv::golden::equal(fv::py::loads(R"([
        {"text": "", "shot": "shot-1.png", "kept_at_s": 0.5, "omitted_shot": "shot-2.png",
         "notes": ["make it purple"]},
        {"text": "", "shot": null, "kept_at_s": null, "notes": ["note"]}])"),
                            manifest.at("sections")));
}

// --- golden vectors ---------------------------------------------------------------------------

namespace {

// Puts `root` where a vector says "<root>". The vectors were recorded on Windows, so the path
// that follows the placeholder is written with `separator` ("\", or "\\" inside JSON text); on
// other systems the same path has "/". The path ends at the ">" or the quote that closes it.
std::string with_root(std::string text, const std::string& root,
                      [[maybe_unused]] std::string_view separator = "\\") {
    const std::string placeholder = "<root>";
    for (auto at = text.find(placeholder); at != std::string::npos;
         at = text.find(placeholder, at + root.size())) {
        text.replace(at, placeholder.size(), root);
#ifndef _WIN32
        const auto end = std::min(text.find_first_of(">\"\n", at + root.size()), text.size());
        std::string tail = text.substr(at + root.size(), end - at - root.size());
        for (auto sep = tail.find(separator); sep != std::string::npos;
             sep = tail.find(separator, sep + 1)) {
            tail.replace(sep, separator.size(), "/");
        }
        text.replace(at + root.size(), end - at - root.size(), tail);
#endif
    }
    return text;
}

std::string with_root(std::string text, const std::filesystem::path& root) {
    return with_root(std::move(text), fv::path_to_utf8(root));
}

#ifndef _WIN32
// True when `shot` is inside `folder` only on a file system that ignores case, as the one the
// vectors were recorded on does.
bool inside_only_ignoring_case(const std::string& shot, const std::string& folder) {
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    };
    return !shot.starts_with(folder) && lower(shot).starts_with(lower(folder));
}
#endif

Walkthrough walkthrough_from(const fv::Json& value, const std::filesystem::path& root) {
    Walkthrough walkthrough{local_moment(pacific(), local_from_iso(value.at("started_at"))),
                            fv::golden::decode<double>(value.at("duration_s")),
                            {}};
    const auto path = [&](const fv::Json& relative) -> std::optional<std::filesystem::path> {
        if (relative.is_null()) {
            return std::nullopt;
        }
        return root / fv::path_from_utf8(relative.get<std::string>());
    };
    for (const auto& item : value.at("sections")) {
        Section section;
        section.text = item.at(0).get<std::string>();
        section.shot = path(item.at(1));
        section.kept_at_s = fv::golden::decode<std::optional<double>>(item.at(2));
        section.omitted = path(item.at(3));
        section.notes = item.at(4).get<std::vector<std::string>>();
        walkthrough.sections.push_back(std::move(section));
    }
    return walkthrough;
}

DocumentFormat format_from(const fv::Json& value) {
    if (value.is_null()) {
        return {};
    }
    return {value.at(0).get<std::string>(), value.at(1).get<bool>(), value.at(2).get<bool>(),
            value.at(3).get<bool>()};
}

// The golden texts name OpenWhisper where FioriVoce says its own name.
std::string rebranded(const std::string& text) {
    return fv::golden::rebrand(text);
}

} // namespace

TEST_CASE("golden: clock", "[golden][markdown]") {
    fv::golden::for_each("services.walkthrough.document", "clock", [](const fv::golden::Case& c) {
        CHECK(clock(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden: _when", "[golden][markdown]") {
    fv::golden::for_each("services.walkthrough.document", "_when", [](const fv::golden::Case& c) {
        CHECK(when_text(local_from_iso(c.arg<std::string>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden: _escape_markers", "[golden][markdown]") {
    fv::golden::for_each(
        "services.walkthrough.document", "_escape_markers", [](const fv::golden::Case& c) {
            CHECK(escape_markers(c.arg<std::string>(0)) == c.result<std::string>());
        });
}

TEST_CASE("golden: _paragraphs", "[golden][markdown]") {
    fv::golden::for_each(
        "services.walkthrough.document", "_paragraphs", [](const fv::golden::Case& c) {
            CHECK(paragraphs(c.arg<std::string>(0)) == c.result<std::vector<std::string>>());
        });
}

TEST_CASE("golden: _tidy", "[golden][markdown]") {
    fv::golden::for_each("services.walkthrough.document", "_tidy", [](const fv::golden::Case& c) {
        CHECK(tidy(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden: _image_target", "[golden][markdown]") {
    fv::golden::for_each(
        "services.walkthrough.document", "_image_target", [](const fv::golden::Case& c) {
#ifndef _WIN32
            if (inside_only_ignoring_case(c.arg<std::string>(0), c.arg<std::string>(1))) {
                return;
            }
#endif
            const TempFolder tmp;
            const auto target = image_target(tmp.path() / fv::path_from_utf8(c.arg<std::string>(0)),
                                             tmp.path() / fv::path_from_utf8(c.arg<std::string>(1)),
                                             c.arg<bool>(2));
            CHECK(target == with_root(c.result<std::string>(), tmp.path()));
        });
}

TEST_CASE("golden: markdown", "[golden][markdown]") {
    fv::golden::for_each(
        "services.walkthrough.document", "markdown", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            const auto text = markdown(walkthrough_from(c.args()[0], tmp.path()), tmp / "capture",
                                       c.arg<bool>(1), format_from(c.args()[2]));
            CHECK(text == with_root(rebranded(c.result<std::string>()), tmp.path()));
        });
}

TEST_CASE("golden: write_manifest", "[golden][markdown]") {
    fv::golden::for_each(
        "services.walkthrough.document", "write_manifest", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            std::filesystem::create_directory(tmp / "capture");
            const auto path =
                write_manifest(walkthrough_from(c.args()[0], tmp.path()), tmp / "capture");
            // Python writes a backslash in a path as "\\" inside JSON text.
            std::string root = fv::path_to_utf8(tmp.path());
            std::string escaped;
            for (const char ch : root) {
                escaped += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
            }
            CHECK(read_file(path) == with_root(c.result<std::string>(), escaped, "\\\\"));
        });
}
