// fiorivoce-cli --workflow-smoke: services/release_checks.py's synthetic lifecycle gate, run in a
// fresh process the way OpenWhisper's release health check runs main.py.
// from tests/test_release_health.py

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "cli/workflow_smoke.h"
#include "cli_process.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::clitest::CliResult;
using fv::clitest::EnvOverride;
using fv::clitest::read_text;
using fv::clitest::run_cli;
using fv::clitest::write_text;

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// check_release_health.py's LIMITS and REQUIRED_CHECKS.
const std::map<std::string, double> kLimits = {{"fixture_ready_s", 15}, {"stop_to_result_p95_s", 2},
                                               {"cancel_s", 2},         {"restart_s", 2},
                                               {"cleanup_s", 2},        {"peak_rss_mb", 1500}};
constexpr const char* kRequiredChecks[] = {"recording_recovery", "backup_restore",
                                           "previous_data_preserved"};

// check_release_health.failures(report) == [], as the release tool decides it.
void check_passes_gate(const Json& report) {
    REQUIRE(report.is_object());
    CHECK(report["kind"] == "synthetic_service_lifecycle");
    CHECK(report["passed"] == true);
    REQUIRE(report["checks"].is_object());
    for (const char* name : kRequiredChecks) {
        INFO(name);
        CHECK(report["checks"][name] == true);
    }
    REQUIRE(report["metrics"].is_object());
    for (const auto& [key, limit] : kLimits) {
        INFO(key);
        const Json& value = report["metrics"][key];
        REQUIRE(value.is_number());
        const double number = value.get<double>();
        CHECK(std::isfinite(number));
        CHECK(number >= 0);
        CHECK(number <= limit);
    }
}

std::vector<std::string> names_in(const fs::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder)) {
        names.push_back(utf8(entry.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

} // namespace

TEST_CASE("fresh process workflow exits and preserves user history",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path sentinel = root.path() / "transcription_history.json";
    write_text(sentinel, R"({"entries": [{"text": "private history"}]})");
    // The smoke's own folder goes into the system's temporary folder, which the run points here
    // so that the test sees it removed.
    const fs::path temporary = root.path() / "tmp";
    fs::create_directories(temporary);
    const fs::path report_path = root.path() / "health.json";
    const std::vector<EnvOverride> env = {{"FIORIVOCE_DATA_DIR", utf8(root.path())},
                                          {"TMP", utf8(temporary)},
                                          {"TEMP", utf8(temporary)},
                                          {"TMPDIR", utf8(temporary)}};

    // check_release_health.py's process exit deadline.
    const CliResult result =
        run_cli({"--workflow-smoke", utf8(report_path)}, env, std::chrono::seconds(45));

    INFO("stdout: " << result.out << "\nstderr: " << result.err);
    REQUIRE(result.exit_code == 0);
    CHECK(read_text(sentinel) == R"({"entries": [{"text": "private history"}]})");
    CHECK_FALSE(fs::exists(root.path() / "fiorivoce.db"));
    CHECK(names_in(root.path()) ==
          std::vector<std::string>{"health.json", "tmp", "transcription_history.json"});
    CHECK(names_in(temporary).empty());
    CHECK(result.out.find("private history") == std::string::npos);
    CHECK(result.err.find("private history") == std::string::npos);
    // A path given: the report only, no "passed" line.
    CHECK(result.out.find("passed\n") == std::string::npos);

    const std::string saved = read_text(report_path);
    const Json data = fv::py::loads(saved);
    check_passes_gate(data);
    REQUIRE(data["limitations"].is_array());
    CHECK_FALSE(data["limitations"].empty());
    CHECK(data["schema"] == 2);
    CHECK(data["repeats"] == 5);
    // json.dumps(report, indent=2) in text mode, keys in the report's order.
    CHECK(saved == fv::cli::report_file_text(data));
    CHECK_THAT(saved, StartsWith(std::string("{") +
                                 (saved.find("\r\n") != std::string::npos ? "\r\n" : "\n") +
                                 "  \"schema\": 2,"));
    // stdout: the same report as json.dumps(report, sort_keys=True), one line.
    std::string line = result.out;
    std::erase(line, '\r');
    fv::py::DumpOptions sorted;
    sorted.sort_keys = true;
    CHECK(line == fv::py::dumps(data, sorted) + "\n");
}

TEST_CASE("workflow smoke takes at most one report path", "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path first = root.path() / "a.json";
    const fs::path second = root.path() / "b.json";

    const CliResult result = run_cli({"--workflow-smoke", utf8(first), utf8(second)},
                                     {{"FIORIVOCE_DATA_DIR", utf8(root.path())}});

    CHECK(result.exit_code == 1);
    CHECK(result.out.empty());
    CHECK_THAT(result.err, StartsWith("Usage: --workflow-smoke [report.json]"));
    CHECK_FALSE(fs::exists(first));
    CHECK_FALSE(fs::exists(second));
}

TEST_CASE("workflow smoke without a path prints the report and saves nothing",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;

    const CliResult result =
        run_cli({"--workflow-smoke"}, {{"FIORIVOCE_DATA_DIR", utf8(root.path())}},
                std::chrono::seconds(45));

    INFO("stderr: " << result.err);
    REQUIRE(result.exit_code == 0);
    check_passes_gate(fv::py::loads(result.out));
    CHECK(names_in(root.path()).empty());
}

TEST_CASE("the smoke writes the fixture as python's wave module does", "[test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path written = root.path() / "fixture.wav";
    const std::vector<std::int16_t> samples = fv::cli::fixture_samples();
    REQUIRE(samples.size() == 16000);

    fv::cli::write_fixture_wav(written, samples);

    const fs::path reference =
        fv::testkit::source_root() / "tests" / "fixtures" / "workflow_smoke" / "fixture.wav";
    CHECK(read_text(written) == read_text(reference));
    // 1000 is E8 03 in little-endian 16-bit PCM.
    std::string pcm;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        pcm += "\xE8\x03";
    }
    CHECK(fv::cli::wav_data(reference) == pcm);
}

TEST_CASE("wav data is the data chunk, past the chunks before it", "[test_release_health]") {
    fv::testkit::TempDataRoot root;
    const auto le32 = [](std::uint32_t value) {
        std::string out;
        for (int i = 0; i < 4; ++i) {
            out += static_cast<char>((value >> (8 * i)) & 0xFF);
        }
        return out;
    };
    // A LIST chunk of odd size is padded to an even one before the data chunk.
    const std::string body = "WAVE" + std::string("LIST") + le32(3) + "abc" + std::string(1, '\0') +
                             "data" + le32(4) + "\x01\x02\x03\x04";
    write_text(root.path() / "tagged.wav",
               "RIFF" + le32(static_cast<std::uint32_t>(body.size())) + body);
    CHECK(fv::cli::wav_data(root.path() / "tagged.wav") == "\x01\x02\x03\x04");

    write_text(root.path() / "text.wav", "not a wave file");
    CHECK_THROWS_AS(fv::cli::wav_data(root.path() / "text.wav"), std::runtime_error);
    write_text(root.path() / "empty.wav", "RIFF" + le32(4) + "WAVE");
    CHECK_THROWS_AS(fv::cli::wav_data(root.path() / "empty.wav"), std::runtime_error);
}

TEST_CASE("stop to result p50 and p95 follow statistics.median and numpy.percentile",
          "[test_release_health]") {
    // The values OpenWhisper's interpreter gives (statistics.median, numpy.percentile).
    CHECK(fv::cli::median({0.3, 0.1, 0.4, 0.2}) == 0.25);
    CHECK(fv::cli::median({3.0, 1.0, 2.0}) == 2.0);
    CHECK(fv::cli::percentile({0.1, 0.2, 0.3, 0.4, 0.5}, 95) == 0.48);
    CHECK(fv::cli::percentile({0.012, 0.011, 0.019, 0.013, 0.010}, 95) == 0.0178);
    CHECK(fv::cli::percentile({1.0, 2.0}, 95) == 1.95);
    CHECK(fv::cli::percentile({0.5}, 95) == 0.5);
    CHECK(fv::cli::median({0.5}) == 0.5);
}

TEST_CASE("the saved report is indented json with text-mode line ends", "[test_release_health]") {
    Json report = Json::object();
    report["b"] = 1;
    report["a"] = Json::array({0.5, nullptr});
    const std::string text = fv::cli::report_file_text(report);
#if defined(_WIN32)
    CHECK(text == "{\r\n  \"b\": 1,\r\n  \"a\": [\r\n    0.5,\r\n    null\r\n  ]\r\n}");
#else
    CHECK(text == "{\n  \"b\": 1,\n  \"a\": [\n    0.5,\n    null\n  ]\n}");
#endif
}

TEST_CASE("peak rss is measured", "[test_release_health]") {
    const std::optional<double> rss = fv::cli::peak_rss_mb();
    REQUIRE(rss.has_value());
    CHECK(*rss > 0);
}

TEST_CASE("the smoke refuses a repeat count outside 1 to 1000", "[test_release_health]") {
    CHECK_THROWS_WITH(fv::cli::run_workflow_smoke(0), "repeats must be between 1 and 1000");
    CHECK_THROWS_WITH(fv::cli::run_workflow_smoke(1001), "repeats must be between 1 and 1000");
}
