// Golden vectors of services/walkthrough/library.py, styles.py, write_up.py and finish.py
// (tools/parity/cases/services.walkthrough.{library,styles,write_up,finish}.py).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <stdexcept>

#include "base/cancel.h"
#include "golden/golden.h"
#include "library_support.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/keys.h"
#include "walkthrough/files.h"
#include "walkthrough/finish.h"
#include "walkthrough/library.h"
#include "walkthrough/styles.h"
#include "walkthrough/write_up.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

// The cases' pinned "now" for expired_entries, seconds since the epoch.
constexpr double kNow = 1'800'000'000.0;

void make_files(const std::filesystem::path& folder, const Json& files) {
    std::filesystem::create_directories(folder);
    for (const auto& item : files) {
        const auto name = item.get<std::string>();
        if (name.ends_with("/")) {
            std::filesystem::create_directory(folder /
                                              fv::path_from_utf8(name.substr(0, name.size() - 1)));
        } else {
            write_file(folder / fv::path_from_utf8(name), "");
        }
    }
}

// A path recorded on Windows, with this platform's separators.
std::string native(std::string text) {
#ifndef _WIN32
    std::ranges::replace(text, '\\', '/');
#endif
    return text;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return text;
    }
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

Json look_json(const Look& look) {
    return Json::array({look.text_color, look.heading_color, look.font});
}

Json styles_json(const std::vector<WriteUpStyle>& styles) {
    Json out = Json::array();
    for (const auto& style : styles) {
        out.push_back(Json::array({style.id, style.name, style.instructions}));
    }
    return out;
}

std::vector<std::string> strings(const Json& value) {
    return value.get<std::vector<std::string>>();
}

std::vector<Note> notes_of(const Json& value) {
    std::vector<Note> notes;
    for (const auto& item : value) {
        notes.push_back({item[0].get<std::string>(), item[1].get<int>()});
    }
    return notes;
}

// The error a call raised, as the vectors record it: [type, message].
Json raised(const std::function<void()>& call) {
    try {
        call();
    } catch (const fv::py::PyError& error) {
        return Json::array({std::string(error.type_name()), error.what()});
    }
    return nullptr;
}

} // namespace

// ---- services.walkthrough.library ----

TEST_CASE("golden: library write_up_parts", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "write_up_parts", [](const golden::Case& c) {
        Json parts = Json::array();
        for (const auto& [words, line] : write_up_parts(c.arg<std::string>(0))) {
            parts.push_back(Json::array({words, line}));
        }
        CHECK(golden::equal(c.result<Json>(), parts));
    });
}

TEST_CASE("golden: library join_write_up", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "join_write_up", [](const golden::Case& c) {
        std::vector<std::pair<std::string, std::string>> parts;
        for (const auto& part : c.arg<Json>(0)) {
            parts.emplace_back(part[0].get<std::string>(), part[1].get<std::string>());
        }
        CHECK(join_write_up(parts) == c.result<std::string>());
    });
}

TEST_CASE("golden: library write_up_shot", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "_write_up_shot", [](const golden::Case& c) {
        const TempFolder tmp;
        make_files(tmp.path(), c.arg<Json>(0));
        const auto found = write_up_shot(tmp.path(), c.arg<std::string>(1));
        const auto expected = c.result<std::optional<std::string>>();
        CHECK(found.has_value() == expected.has_value());
        if (found && expected) {
            CHECK(found->parent_path() == tmp.path());
            CHECK(fv::path_to_utf8(found->filename()) == *expected);
        }
    });
}

TEST_CASE("golden: library earlier_write_ups", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "_earlier_write_ups",
                     [](const golden::Case& c) {
                         const TempFolder tmp;
                         make_files(tmp.path(), c.arg<Json>(0));
                         CHECK(earlier_write_ups(tmp.path()) == c.result<std::vector<int>>());
                     });
}

TEST_CASE("golden: library load_document", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "_load_document", [](const golden::Case& c) {
        const TempFolder tmp;
        make_files(tmp.path(), c.arg<Json>(1));
        write_file(tmp / "walkthrough.json", c.arg<std::string>(0));
        const Json expected = c.result<Json>();
        std::optional<Walkthrough> loaded;
        const Json error = raised([&] { loaded = load_document(tmp.path()); });
        if (expected.contains("error")) {
            CHECK(error == expected["error"]);
            return;
        }
        REQUIRE(error.is_null());
        Json sections = Json::array();
        for (const auto& section : loaded->sections) {
            const auto name = [](const std::optional<std::filesystem::path>& path) {
                return path ? Json(fv::path_to_utf8(path->filename())) : Json(nullptr);
            };
            sections.push_back(
                Json::array({section.text, name(section.shot),
                             section.kept_at_s ? Json(*section.kept_at_s) : Json(nullptr),
                             name(section.omitted), Json(section.notes)}));
        }
        CHECK(golden::equal(expected["sections"], sections));
        CHECK(loaded->duration_s == expected["duration_s"].get<double>());
        if (!expected["started_utc"].is_null()) {
            // The instant, in whole seconds: a start keeps no fraction here.
            const auto utc = fv::py::fromisoformat(expected["started_utc"].get<std::string>());
            const auto instant = std::chrono::sys_days{std::chrono::year{utc.year} /
                                                       static_cast<unsigned>(utc.month) /
                                                       static_cast<unsigned>(utc.day)} +
                                 std::chrono::hours{utc.hour} + std::chrono::minutes{utc.minute} +
                                 std::chrono::seconds{utc.second};
            CHECK(loaded->started_at.get_sys_time() == instant);
        }
    });
}

TEST_CASE("golden: library retention_days", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "_retention_days", [](const golden::Case& c) {
        const TempFolder tmp;
        if (const auto text = c.arg<std::optional<std::string>>(0)) {
            write_file(tmp / "retention.json", *text);
        }
        CHECK(retention_days(tmp.path()) == c.result<int>());
    });
}

TEST_CASE("golden: library expired_entries", "[golden][library]") {
    golden::for_each("services.walkthrough.library", "_expired_entries", [](const golden::Case& c) {
        const TempFolder tmp;
        Json retention = Json::object();
        retention["days"] = c.arg<Json>(0);
        write_file(tmp / "retention.json", fv::py::dumps(retention));
        for (const auto& folder : c.arg<Json>(1)) {
            const auto path = tmp.path() / fv::path_from_utf8(folder[0].get<std::string>());
            std::filesystem::create_directory(path);
            if (!folder[1].is_null()) {
                const auto manifest = path / "walkthrough.json";
                write_file(manifest, "{}");
                const std::chrono::duration<double> at(kNow - folder[1].get<double>());
                const std::chrono::sys_time<std::chrono::seconds> when(
                    std::chrono::duration_cast<std::chrono::seconds>(at));
                std::filesystem::last_write_time(
                    manifest, std::chrono::clock_cast<std::chrono::file_clock>(when));
            }
            if (folder[2].get<bool>()) {
                write_file(path / "draft.json", "{}");
            }
        }
        std::vector<std::string> names;
        for (const auto& folder : expired_entries(tmp.path(), kNow)) {
            names.push_back(fv::path_to_utf8(folder.filename()));
        }
        CHECK(names == c.result<std::vector<std::string>>());
    });
}

// ---- services.walkthrough.styles ----

TEST_CASE("golden: styles saved_styles and load_styles", "[golden][styles]") {
    golden::for_each("services.walkthrough.styles", "_saved_styles", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), styles_json(saved_styles(c.arg<Json>(0)))));
    });
    golden::for_each("services.walkthrough.styles", "_load_styles", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), styles_json(load_styles(c.arg<Json>(0)))));
    });
}

TEST_CASE("golden: styles style_named and style_for", "[golden][styles]") {
    golden::for_each("services.walkthrough.styles", "_style_named", [](const golden::Case& c) {
        const auto style = style_named(c.arg<Json>(0), c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(), style ? Json(style->id) : Json(nullptr)));
    });
    golden::for_each("services.walkthrough.styles", "_style_for", [](const golden::Case& c) {
        const auto style = style_for(c.arg<Json>(0), c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(), style ? Json(style->id) : Json(nullptr)));
    });
}

TEST_CASE("golden: styles save_style", "[golden][styles]") {
    golden::for_each("services.walkthrough.styles", "_save_style", [](const golden::Case& c) {
        Json settings = c.arg<Json>(0);
        std::set<std::string> known;
        for (const auto& style : saved_styles(settings)) {
            known.insert(style.id);
        }
        const std::string key(fv::keys::walkthrough_write_up_styles.key());
        const auto stored = [&]() -> Json {
            if (!settings.contains(key)) {
                return nullptr;
            }
            Json out = Json::array();
            for (const auto& item : settings[key]) {
                const auto id = item["id"].get<std::string>();
                out.push_back(Json::array(
                    {known.contains(id) ? id : "<new>", item["name"], item["instructions"]}));
            }
            return out;
        };
        Json actual = Json::object();
        try {
            // A new style's id is the vectors' placeholder for the random one.
            const auto style =
                save_style(settings, c.arg<std::string>(1), c.arg<std::string>(2), "<new>");
            actual["style"] = Json::array({style.id, style.name, style.instructions});
        } catch (const fv::py::ValueError& error) {
            actual["error"] = error.what();
        }
        actual["saved"] = stored();
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden: styles delete_style", "[golden][styles]") {
    golden::for_each("services.walkthrough.styles", "_delete_style", [](const golden::Case& c) {
        Json settings = c.arg<Json>(0);
        delete_style(settings, c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(),
                            settings[std::string(fv::keys::walkthrough_write_up_styles.key())]));
    });
}

// ---- services.walkthrough.write_up ----

TEST_CASE("golden: write_up document_text and split_look", "[golden][write_up]") {
    golden::for_each("services.walkthrough.write_up", "document_text", [](const golden::Case& c) {
        const auto names = strings(c.arg<Json>(1));
        const auto [text, look] = write_up::document_text(c.arg<std::string>(0), names);
        CHECK(golden::equal(c.result<Json>(), Json::array({text, look_json(look)})));
    });
    golden::for_each("services.walkthrough.write_up", "_split_look", [](const golden::Case& c) {
        const auto [look, rest] = write_up::split_look(c.arg<std::string>(0));
        CHECK(golden::equal(c.result<Json>(), Json::array({look_json(look), rest})));
    });
}

TEST_CASE("golden: write_up notes and agent names", "[golden][write_up]") {
    golden::for_each("services.walkthrough.write_up", "_notes", [](const golden::Case& c) {
        const auto notes = notes_of(c.arg<Json>(0));
        const auto names = strings(c.arg<Json>(1));
        CHECK(write_up::notes_text(notes, names) == c.result<std::string>());
    });
    golden::for_each("services.walkthrough.write_up", "agent_name", [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(write_up::agent_name(c.arg<std::string>(0)), std::out_of_range);
            return;
        }
        CHECK(write_up::agent_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden: write_up prompts", "[golden][write_up]") {
    golden::for_each("services.walkthrough.write_up", "_prompts", [](const golden::Case& c) {
        const TempFolder tmp;
        const auto renderer = std::make_shared<FakeRenderer>();
        const ScopedRenderer installed(renderer);
        const auto& root = tmp.path();
        write_file(root / "walkthrough.pdf", "%PDF");
        if (const auto markdown = c.arg<std::optional<std::string>>(6)) {
            write_file(root / "walkthrough.md", *markdown);
        }
        const auto names = strings(c.arg<Json>(4));
        std::vector<std::filesystem::path> shots;
        for (const auto& name : names) {
            write_file(root / fv::path_from_utf8(name), "");
            shots.push_back(root / fv::path_from_utf8(name));
        }
        WalkthroughResult result;
        result.folder = root;
        result.pdf = root / "walkthrough.pdf";
        result.markdown = root / "walkthrough.md";
        result.shots = static_cast<int>(names.size());
        result.duration_s = c.arg<double>(5);
        const auto notes = notes_of(c.arg<Json>(3));
        std::optional<write_up::AgentTaskRequest> seen;
        const write_up::AgentTaskRunner run = [&](const write_up::AgentTaskRequest& request) {
            seen = request;
            fv::agents::PassOutcome outcome;
            outcome.canceled = true;
            outcome.error = "canceled";
            return outcome;
        };
        try {
            write_up::write_up(result, shots, c.arg<std::string>(0), c.arg<std::string>(1),
                               c.arg<bool>(2), notes, Json::object(), fv::CancelToken{}, run);
        } catch (const write_up::WriteUpFailed&) {
        }
        const Json expected = c.result<Json>();
        if (expected.contains("error")) {
            CHECK(expected["error"] == "WriteUpFailed");
            CHECK_FALSE(seen);
            return;
        }
        REQUIRE(seen);
        const std::string real =
            fv::path_to_utf8(std::filesystem::weakly_canonical(root).make_preferred());
        const std::string plain = fv::path_to_utf8(std::filesystem::path(root).make_preferred());
        const auto rooted = [&](const std::string& text) {
            return replace_all(replace_all(text, real, "<root>"), plain, "<root>");
        };
        CHECK(golden::equal_rebranded(expected["system"].get<std::string>(), seen->system_prompt));
        CHECK(rooted(seen->user_prompt) == native(expected["user"].get<std::string>()));
        if (expected["folder"].is_null()) {
            CHECK_FALSE(seen->reading);
            CHECK(seen->folder.empty());
        } else {
            CHECK(seen->reading);
            CHECK(rooted(fv::path_to_utf8(seen->folder)) ==
                  native(expected["folder"].get<std::string>()));
        }
        if (expected["images"].is_null()) {
            CHECK(seen->images.empty());
        } else {
            std::vector<std::string> images;
            for (const auto& image : seen->images) {
                images.push_back(rooted(fv::path_to_utf8(image)));
            }
            std::vector<std::string> wanted;
            for (const auto& image : expected["images"]) {
                wanted.push_back(native(image.get<std::string>()));
            }
            CHECK(images == wanted);
        }
        CHECK(seen->timeout_s == write_up::TIMEOUT_S);
        CHECK(seen->stall_s == write_up::STALL_S);
    });
}

// ---- services.walkthrough.finish ----

TEST_CASE("golden: finish document_settings", "[golden][finish]") {
    golden::for_each("services.walkthrough.finish", "document_settings", [](const golden::Case& c) {
        const auto [format, paper] = document_settings(c.arg<Json>(0));
        const Json paper_name = paper == PaperSize::Letter ? Json("letter")
                                : paper == PaperSize::A4   ? Json("a4")
                                                           : Json(nullptr);
        CHECK(
            golden::equal(c.result<Json>(), Json::array({format.title, format.details, format.about,
                                                         format.captions, paper_name})));
    });
}
