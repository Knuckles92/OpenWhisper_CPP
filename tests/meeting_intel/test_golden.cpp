// Golden vectors of meeting/recall.py, meeting/context_folder.py, meeting/semantic_search.py and
// the meeting resolvers of services/settings.py (tools/parity/cases/*.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/identity.h"
#include "golden/golden.h"
#include "intel_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting_intel/context_folder.h"
#include "meeting_intel/detail/text.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/pdf_text.h"
#include "meeting_intel/recall.h"
#include "meeting_intel/semantic_search.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

#if defined(_WIN32)
#include "base/testing/map_env_reader.h"
#endif

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using fv::Json;
using fv::golden::Case;
namespace fs = std::filesystem;

namespace {

std::string arg_text(const Case& c, std::size_t index) {
    return detail::text_or_empty(c.args().at(index));
}

std::vector<std::string> arg_strings(const Case& c, std::size_t index) {
    return c.arg<std::vector<std::string>>(index);
}

void check_equal(const Case& c, const Json& actual) {
    INFO(c.label());
    INFO("actual: " << actual.dump());
    CHECK(fv::golden::equal(c.raw().at("result"), actual));
}

std::vector<std::string> words(std::string_view text) {
    return fv::py::split(text);
}

} // namespace

// ---- services.settings ----

TEST_CASE("the Fast judgments gates match OpenWhisper", "[golden][services.settings]") {
    fv::golden::for_each("services.settings", "resolve_typesafe_enabled", [](const Case& c) {
        CHECK(resolve_typesafe_enabled(c.arg<Json>(0)) == c.result<bool>());
    });
    fv::golden::for_each(
        "services.settings", "resolve_typesafe_feature_enabled", [](const Case& c) {
            const std::string feature = c.arg<std::string>(0);
            if (c.raises()) {
                CHECK(c.raises_type() == "KeyError");
                CHECK_THROWS_AS(resolve_typesafe_feature_enabled(c.arg<Json>(1), feature),
                                std::out_of_range);
                return;
            }
            CHECK(resolve_typesafe_feature_enabled(c.arg<Json>(1), feature) == c.result<bool>());
        });
    fv::golden::for_each(
        "services.settings", "resolve_typesafe_topic_shift_enabled", [](const Case& c) {
            CHECK(resolve_typesafe_topic_shift_enabled(c.arg<Json>(0)) == c.result<bool>());
        });
    fv::golden::for_each(
        "services.settings", "resolve_typesafe_voice_commands_enabled", [](const Case& c) {
            CHECK(resolve_typesafe_voice_commands_enabled(c.arg<Json>(0)) == c.result<bool>());
        });
}

TEST_CASE("the voice command names match OpenWhisper", "[golden][services.settings]") {
    // OpenWhisper's defaults are its own spoken names: replayed with its identity.
    fv::golden::for_each(
        "services.settings", "resolve_typesafe_voice_command_names", [](const Case& c) {
            CHECK(resolve_typesafe_voice_command_names(c.arg<Json>(0), fv::legacy_identity()) ==
                  c.result<std::vector<std::string>>());
        });
}

TEST_CASE("the recall, folder and review resolvers match OpenWhisper",
          "[golden][services.settings]") {
    fv::golden::for_each(
        "services.settings", "resolve_meeting_past_recall_enabled", [](const Case& c) {
            CHECK(resolve_meeting_past_recall_enabled(c.arg<Json>(0)) == c.result<bool>());
        });
    fv::golden::for_each(
        "services.settings", "resolve_meeting_context_folder_enabled", [](const Case& c) {
            CHECK(resolve_meeting_context_folder_enabled(c.arg<Json>(0)) == c.result<bool>());
        });
    fv::golden::for_each("services.settings", "resolve_meeting_insight_review", [](const Case& c) {
        check_equal(c, resolve_meeting_insight_review(c.arg<Json>(0)));
    });
}

#if defined(_WIN32)
// The vectors are ntpath's; a POSIX build follows posixpath and is covered by hand.
TEST_CASE("the knowledge folder path matches OpenWhisper", "[golden][services.settings]") {
    const std::string cwd = utf8(fs::current_path());
    fv::golden::for_each(
        "services.settings", "resolve_meeting_context_folder_path", [&cwd](const Case& c) {
            fv::MapEnvReader env;
            const auto environment = c.arg<Json>(1);
            for (const auto& [name, value] : environment.items()) {
                if (value.is_string()) {
                    env.set(name, value.get<std::string>());
                }
            }
            std::string path = resolve_meeting_context_folder_path(c.arg<Json>(0), env);
            if (const auto at = path.find(cwd); at != std::string::npos) {
                path.replace(at, cwd.size(), "<cwd>");
            }
            CHECK(path == c.result<std::string>());
        });
}
#endif

// ---- meeting.context_folder ----

TEST_CASE("knowledge-folder scoring and snippets match OpenWhisper",
          "[golden][meeting.context_folder]") {
    fv::golden::for_each("meeting.context_folder", "_query_tokens", [](const Case& c) {
        CHECK(detail::query_tokens(arg_text(c, 0)) == c.result<std::vector<std::string>>());
    });
    fv::golden::for_each("meeting.context_folder", "_score", [](const Case& c) {
        CHECK(detail::score(c.arg<std::string>(0), c.arg<std::string>(1), arg_strings(c, 2)) ==
              c.result<int>());
    });
    fv::golden::for_each("meeting.context_folder", "_snippet", [](const Case& c) {
        CHECK(detail::snippet(c.arg<std::string>(0), arg_strings(c, 1), c.arg<std::size_t>(2)) ==
              c.result<std::string>());
    });
    fv::golden::for_each("meeting.context_folder", "_clip", [](const Case& c) {
        CHECK(detail::clip(arg_text(c, 0), c.arg<std::size_t>(1)) == c.result<std::string>());
    });
    fv::golden::for_each("meeting.context_folder", "_sanitize", [](const Case& c) {
        CHECK(detail::sanitize(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("plain text decoding matches OpenWhisper", "[golden][meeting.context_folder]") {
    fv::golden::for_each("meeting.context_folder", "_extract_plain", [](const Case& c) {
        const std::string hex = c.arg<std::string>(0);
        std::string bytes;
        for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
            bytes += static_cast<char>(std::stoi(hex.substr(at, 2), nullptr, 16));
        }
        CHECK(detail::extract_plain(bytes) == c.result<std::string>());
    });
}

TEST_CASE("Office text matches python-docx, python-pptx and openpyxl",
          "[golden][meeting.context_folder]") {
    fv::golden::for_each("meeting.context_folder", "_extract_text", [](const Case& c) {
        CHECK(detail::extract_text(knowledge_fixtures() / c.arg<std::string>(0)) ==
              c.result<std::string>());
    });
}

TEST_CASE("PDF text holds pypdf's words", "[golden][meeting.context_folder]") {
    const auto library = fv::testkit::get_env("FIORIVOCE_TEST_PDFIUM");
    if (!library || library->empty()) {
        SKIP("FIORIVOCE_TEST_PDFIUM is not set: PDFs are not read");
    }
    set_pdfium_path(fv::base_utf8::path_from_utf8(*library));
    // PDFium and pypdf lay text out differently; the words and their order must agree.
    fv::golden::for_each("meeting.context_folder", "_extract_pdf", [](const Case& c) {
        const fs::path pdf = knowledge_fixtures() / c.arg<std::string>(0);
        if (c.raises()) {
            CHECK_THROWS_AS(pdf_text(pdf, kFolderMaxPdfPages), PdfError);
            return;
        }
        const auto text = pdf_text(pdf, kFolderMaxPdfPages);
        REQUIRE(text);
        CHECK(words(*text) == words(c.result<std::string>()));
    });
    set_pdfium_path(std::nullopt);
}

TEST_CASE("safe_join matches OpenWhisper", "[golden][meeting.context_folder]") {
    TempFolder temp;
    write_text(temp.path(), "vault/note.md", "note");
    write_text(temp.path(), "vault/Projects/plan.md", "plan");
    write_text(temp.path(), "vault/.hidden/x.md", "hidden");
    write_text(temp.path(), "outside.md", "outside");
    const fs::path root = fs::canonical(temp.path() / "vault");
    fv::golden::for_each("meeting.context_folder", "_safe_join", [&root](const Case& c) {
        const auto joined = detail::safe_join(root, c.arg<std::string>(0));
        std::optional<std::string> relative;
        if (joined) {
            relative = utf8(joined->lexically_relative(root));
            std::replace(relative->begin(), relative->end(), '\\', '/');
        }
        CHECK(relative == c.result<std::optional<std::string>>());
    });
}

TEST_CASE("knowledge-folder search matches OpenWhisper", "[golden][meeting.context_folder]") {
    fv::golden::for_each("meeting.context_folder", "search_context_files", [](const Case& c) {
        clear_context_folder_cache();
        TempFolder temp;
        const fs::path folder = temp.path() / "vault";
        fs::create_directories(folder);
        for (const char* name : {"memo.docx", "deck.pptx", "sheet.xlsx"}) {
            fs::copy_file(knowledge_fixtures() / name, folder / name);
        }
        const auto files = c.arg<Json>(0);
        for (const auto& [relpath, text] : files.items()) {
            write_text(folder, relpath, text.get<std::string>());
        }
        const Json result = search_context_files(folder_settings(true, utf8(folder)),
                                                 arg_text(c, 1), arg_text(c, 2), c.arg<Json>(3));
        check_equal(c, result);
        clear_context_folder_cache();
    });
}

// ---- meeting.recall ----

TEST_CASE("recall's labels and clips match OpenWhisper", "[golden][meeting.recall]") {
    fv::golden::for_each("meeting.recall", "_date_label", [](const Case& c) {
        CHECK(detail::date_label(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("meeting.recall", "_title", [](const Case& c) {
        CHECK(detail::title_of(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("meeting.recall", "_start_s", [](const Case& c) {
        CHECK(fv::golden::near(c.result<double>(), detail::start_seconds(c.arg<Json>(0)), 0.0));
    });
    fv::golden::for_each("meeting.recall", "_clip", [](const Case& c) {
        CHECK(detail::clip(arg_text(c, 0), c.arg<std::size_t>(1)) == c.result<std::string>());
    });
    fv::golden::for_each("meeting.recall", "_sanitize", [](const Case& c) {
        CHECK(detail::sanitize(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("meeting.recall", "_clamp_limit", [](const Case& c) {
        CHECK(detail::clamp_limit(c.arg<Json>(0), kRecallDefaultHitLimit, kRecallMaxHitLimit) ==
              c.result<std::int64_t>());
    });
}

namespace {

StubHistory stub_from(const Json& rows, const Json& meetings, const Json& segments) {
    StubHistory stub;
    if (rows.is_array()) {
        for (const auto& row : rows) {
            stub.rows.push_back(row);
        }
    }
    for (const auto& [id, row] : meetings.items()) {
        stub.meetings[id] = row;
    }
    for (const auto& [id, list] : segments.items()) {
        for (const auto& row : list) {
            stub.segments[id].push_back(row);
        }
    }
    return stub;
}

} // namespace

TEST_CASE("recall's hits and slices match OpenWhisper", "[golden][meeting.recall]") {
    fv::golden::for_each("meeting.recall", "_format_hits", [](const Case& c) {
        std::vector<Json> rows;
        for (const auto& row : c.arg<Json>(0)) {
            rows.push_back(row);
        }
        check_equal(c, detail::format_hits(rows, c.arg<std::int64_t>(1), c.arg<std::string>(2)));
    });
    fv::golden::for_each("meeting.recall", "_slice_meeting", [](const Case& c) {
        StubHistory stub = stub_from(Json::array(), c.arg<Json>(0), c.arg<Json>(1));
        check_equal(c, detail::slice_meeting(stub, c.arg<std::string>(2), c.arg<std::string>(3),
                                             c.arg<std::int64_t>(4)));
    });
}

TEST_CASE("search_past_meetings matches OpenWhisper", "[golden][meeting.recall]") {
    fv::golden::for_each("meeting.recall", "search_past_meetings", [](const Case& c) {
        StubHistory stub = stub_from(c.arg<Json>(1), c.arg<Json>(2), c.arg<Json>(3));
        RecallDeps deps;
        deps.history = c.args().at(1).is_null() ? nullptr : &stub;
        deps.settings = fixed(Json{{"meeting_past_recall_enabled", c.arg<bool>(0)}});
        check_equal(c, search_past_meetings(deps, arg_text(c, 4), arg_text(c, 5), arg_text(c, 6),
                                            c.args().at(7)));
    });
}

// ---- meeting.semantic_search ----

TEST_CASE("semantic search's texts match OpenWhisper", "[golden][meeting.semantic_search]") {
    fv::golden::for_each("meeting.semantic_search", "constants", [](const Case& c) {
        const Json& expected = c.raw().at("result");
        CHECK(expected.at("MAX_CANDIDATES") == kMaxCandidates);
        CHECK(expected.at("SEARCH_BUDGET_S") == kSearchBudgetS);
        CHECK(expected.at("SCOPE") == std::string(kScope));
        CHECK(expected.at("LOCAL_SCOPE") == std::string(kLocalScope));
        CHECK(expected.at("BATCH_SIZE") == kBatchSize);
        CHECK(expected.at("RELEVANT_SCORE") == kRelevantScore);
        CHECK(expected.at("DISABLED_MESSAGE") == std::string(kDisabledMessage));
        CHECK(expected.at("UNAVAILABLE_MESSAGE") == std::string(kUnavailableMessage));
        CHECK(expected.at("RELEVANCE_QUESTION") == std::string(kRelevanceQuestion));
    });
}

TEST_CASE("semantic search matches OpenWhisper", "[golden][meeting.semantic_search]") {
    fv::golden::for_each("meeting.semantic_search", "search_history", [](const Case& c) {
        const Json setup = c.arg<Json>(0);
        StubHistory stub;
        for (const auto& row : setup.at("keyword")) {
            stub.rows.push_back(row);
        }
        for (const auto& row : setup.at("candidates")) {
            stub.candidates.push_back(row);
        }
        for (const auto& [id, cloud] : setup.at("cloud").items()) {
            stub.meetings[id] = Json{{"id", id}, {"cloud_enabled", cloud}};
        }
        // resolve_typesafe_feature_enabled's answers, one per settings read, the last repeated.
        std::vector<bool> feeds;
        if (setup.at("feature").is_array()) {
            for (const auto& value : setup.at("feature")) {
                feeds.push_back(value.get<bool>());
            }
        } else {
            feeds.push_back(setup.at("feature").get<bool>());
        }
        auto reads = std::make_shared<std::size_t>(0);
        const std::string provider = setup.at("route").get<std::string>();
        const auto settings = [feeds, reads, provider] {
            const bool on = feeds[std::min(*reads, feeds.size() - 1)];
            ++*reads;
            return Json{{"typesafe_enabled", true},
                        {"typesafe_semantic_search_enabled", on},
                        {"typesafe_provider", provider}};
        };
        std::shared_ptr<fv::llm::FakeJudge> judge;
        if (!setup.at("judge").is_null()) {
            judge = std::make_shared<fv::llm::FakeJudge>();
            judge->set_local(setup.at("judge").at("is_local").get<bool>());
            for (const auto& batch : setup.at("judge").at("answers")) {
                if (batch.is_null()) {
                    judge->push(std::nullopt);
                    continue;
                }
                Json answers = Json::object();
                for (const auto& [key, score] : batch.items()) {
                    answers[key] = Json{{"type", "noul"}, {"noul", score}};
                }
                judge->push(answers);
            }
        }
        std::optional<std::string> exclude;
        if (setup.at("exclude").is_string()) {
            exclude = setup.at("exclude").get<std::string>();
        }
        const Json result = search_history(
            stub, settings, setup.at("query").get<std::string>(), setup.at("semantic").get<bool>(),
            exclude, setup.at("limit").get<std::int64_t>(),
            [judge]() -> std::shared_ptr<fv::llm::Judge> { return judge; });

        Json actual = Json::object();
        actual["result"] = result;
        actual["asks"] = Json::array();
        if (judge) {
            for (const auto& call : judge->calls()) {
                actual["asks"].push_back(
                    Json{{"state", call.state}, {"questions", call.questions}});
            }
        }
        actual["candidate_calls"] = Json::array();
        for (const auto& call : stub.candidate_calls) {
            actual["candidate_calls"].push_back(
                Json{{"query", call.query},
                     {"exclude", call.exclude_meeting_id ? Json(*call.exclude_meeting_id) : Json()},
                     {"limit", call.limit},
                     {"shared_only", call.shared_only}});
        }
        actual["meeting_reads"] = Json::array();
        for (const auto& id : stub.meeting_reads) {
            actual["meeting_reads"].push_back(id);
        }
        check_equal(c, actual);
    });
}
