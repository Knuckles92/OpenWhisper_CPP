// fiorivoce-cli, run as a child process the way OpenWhisper's tests run main.py.
// from tests/test_main_entrypoint.py and tests/test_release_health.py
//
// The decode test is skipped unless these are set (see README.md):
//   FIORIVOCE_TEST_RUNTIME_DIR  an unpacked NeMo-Speech.cpp release
//   FIORIVOCE_TEST_MODEL        a Parakeet .gguf
//   FIORIVOCE_TEST_WAV          a recording with speech in it

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "cli/pe_imports.h"
#include "cli_process.h"
#include "engine/runtime.h"
#include "testkit/testkit.h"

#if defined(__linux__)
#include <elf.h>
#endif

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

#if defined(__linux__)
// The shared libraries a 64-bit ELF image names in its dynamic section (DT_NEEDED), the Linux
// counterpart of a PE import table; nullopt when the image is not one.
std::optional<std::vector<std::string>> elf_needed(const std::string& image) {
    const auto read = [&image]<class T>(std::uint64_t offset, T& out) {
        if (offset > image.size() || image.size() - offset < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, image.data() + offset, sizeof(T));
        return true;
    };
    Elf64_Ehdr header{};
    if (!read(0, header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64) {
        return std::nullopt;
    }
    for (std::uint16_t index = 0; index < header.e_shnum; ++index) {
        Elf64_Shdr section{};
        if (!read(header.e_shoff + std::uint64_t{index} * header.e_shentsize, section)) {
            return std::nullopt;
        }
        if (section.sh_type != SHT_DYNAMIC) {
            continue;
        }
        Elf64_Shdr strings{};
        if (!read(header.e_shoff + std::uint64_t{section.sh_link} * header.e_shentsize, strings)) {
            return std::nullopt;
        }
        std::vector<std::string> names;
        for (std::uint64_t at = 0; at + sizeof(Elf64_Dyn) <= section.sh_size;
             at += sizeof(Elf64_Dyn)) {
            Elf64_Dyn entry{};
            if (!read(section.sh_offset + at, entry) || entry.d_tag == DT_NULL) {
                break;
            }
            const std::uint64_t name = strings.sh_offset + entry.d_un.d_val;
            if (entry.d_tag == DT_NEEDED && name < image.size()) {
                names.emplace_back(image.c_str() + name);
            }
        }
        return names;
    }
    return std::nullopt;
}
#endif

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

EnvOverride data_dir(const fs::path& path) {
    return {"FIORIVOCE_DATA_DIR", utf8(path)};
}

// A runtime folder whose library is the decoy: find_runtime_library accepts it, and loading it
// leaves `marker` behind.
fs::path make_decoy_runtime(const fs::path& root) {
    const fs::path bin = root / "decoy-runtime" / "bin";
    fs::create_directories(bin);
    fs::copy_file(FV_CLI_DECOY_RUNTIME, bin / fv::runtime_library_name(),
                  fs::copy_options::overwrite_existing);
    return root / "decoy-runtime";
}

// main.py's _USAGE, rebranded, without the GUI's line ("With no option, opens ...") and the
// options of dropped platforms (OPENWHISPER_UI); with this executable's transcribe command and
// its options and the installed check. The lines both share are _USAGE's word for word.
constexpr std::string_view kUsage = R"(Usage: fiorivoce-cli COMMAND [OPTION]...
       fiorivoce-cli [OPTION]

Command-line tools of FioriVoce: the speech engine, and checks that need
no window.

Commands:
  transcribe AUDIO.wav [TRANSCRIBE OPTION]...
                          Transcribe a WAV file with Parakeet and print the
                          text.

Options:
  -h, --help              Show this help and exit.
  --version               Print the version and exit.
  --diagnostics OUTPUT.json
                          Write a sanitized support report to a new JSON file
                          and exit. Nothing is uploaded.
  --api [--database PATH] [--port N]
                          Serve the read-only History API on 127.0.0.1 (port
                          8766 by default) without opening the app. Set
                          FIORIVOCE_API_TOKEN to a random token of at least
                          32 characters first.
  --channel               Run as a Claude Code channel (an MCP server over
                          stdio) that forwards FioriVoce actions into a
                          running Claude Code session. FioriVoce must be
                          running with API & MCP on.
  --self-test             Check that the data folder, the settings and the
                          speech runtime are usable, run the workflow smoke,
                          and exit.
  --self-test --installed
                          Check that every file the installed app needs is
                          beside this program, and exit.
  --workflow-smoke [REPORT.json]
                          Run a headless check of the dictation pipeline,
                          optionally saving its report, and exit.

Transcribe options:
  --model FILE            The model's .gguf file. Default: $FIORIVOCE_MODEL.
  --runtime DIR           An unpacked NeMo-Speech.cpp release.
                          Default: $FIORIVOCE_RUNTIME_DIR.
  --gpu N                 Decode on the runtime's GPU number N (0 is the
                          first). Default: CPU.
  --language CODE         The spoken language, such as en. Default: detect it.
  --timestamps            Print one segment per line with its start and end
                          time.

Environment:
  FIORIVOCE_DATA_DIR=PATH
                          Keep settings, history and recordings in PATH.
  FIORIVOCE_LOG_LEVEL=LEVEL
                          Log at DEBUG, INFO (the default), WARNING or ERROR.
  FIORIVOCE_RUNTIME_DIR=PATH
                          The speech runtime to use when none is given.
)";

// 0.1 s of silence: 16-bit mono PCM at 16 kHz.
void write_silent_wav(const fs::path& path) {
    const auto le = [](std::uint32_t value, int bytes) {
        std::string out;
        for (int i = 0; i < bytes; ++i) {
            out += static_cast<char>((value >> (8 * i)) & 0xFF);
        }
        return out;
    };
    const std::uint32_t data_bytes = 1600 * 2;
    std::string wav = "RIFF" + le(36 + data_bytes, 4) + "WAVEfmt " + le(16, 4) + le(1, 2) +
                      le(1, 2) + le(16000, 4) + le(32000, 4) + le(2, 2) + le(16, 2) + "data" +
                      le(data_bytes, 4) + std::string(data_bytes, '\0');
    write_text(path, wav);
}

} // namespace

TEST_CASE("help prints usage and exits without opening the app",
          "[platform][test_main_entrypoint]") {
    const std::string flag = GENERATE("--help", "-h");
    fv::testkit::TempDataRoot root;
    const fs::path untouched = root.path() / "never-created";

    const CliResult result = run_cli({flag}, {data_dir(untouched)});

    REQUIRE(result.exit_code == 0);
    CHECK(result.err.empty());
    CHECK_THAT(result.out, StartsWith("Usage: fiorivoce-cli"));
    // The console's text mode ends lines with CRLF on Windows, as Python's print does.
    std::string text = result.out;
    std::erase(text, '\r');
    CHECK(text == kUsage);
    for (const char* option :
         {"--version", "--diagnostics OUTPUT.json", "--api [--database PATH] [--port N]",
          "--channel", "--self-test", "--self-test --installed", "--workflow-smoke [REPORT.json]",
          "transcribe AUDIO.wav", "FIORIVOCE_DATA_DIR=PATH", "FIORIVOCE_LOG_LEVEL=LEVEL",
          "FIORIVOCE_RUNTIME_DIR=PATH"}) {
        CHECK_THAT(result.out, ContainsSubstring(option));
    }
    // OpenWhisper's internal modes are not listed.
    for (const char* hidden : {"--local-asr-worker", "--isolated-worker", "--macos-update-helper",
                               "internal", "OPENWHISPER"}) {
        CHECK_THAT(result.out, !ContainsSubstring(hidden));
    }
    // Help answers before the data folder is looked at.
    CHECK_FALSE(fs::exists(untouched));
}

TEST_CASE("help still answers when more arguments follow", "[platform][test_main_entrypoint]") {
    const CliResult result = run_cli({"--help", "--no-such-option"}, {});
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.out, StartsWith("Usage: fiorivoce-cli"));
}

TEST_CASE("help loads neither qt nor the speech runtimes", "[platform][test_main_entrypoint]") {
    fv::testkit::TempDataRoot root;
    const fs::path runtime = make_decoy_runtime(root.path());
    const fs::path marker = root.path() / "runtime-loaded";
    const std::vector<EnvOverride> env = {data_dir(root.path() / "data"),
                                          {"FIORIVOCE_RUNTIME_DIR", utf8(runtime)},
                                          {"FV_CLI_DECOY_MARKER", utf8(marker)}};

    SECTION("the commands that answer at once leave the runtime alone") {
        for (const std::vector<std::string>& args :
             {std::vector<std::string>{"--help"}, {"-h"}, {"--version"}}) {
            const CliResult result = run_cli(args, env);
            INFO("command: " << args.front());
            CHECK(result.exit_code == 0);
            CHECK_FALSE(fs::exists(marker));
            // They answer before the data folder is resolved, so it is never created.
            CHECK_FALSE(fs::exists(root.path() / "data"));
        }
    }

    SECTION("the self-test finds the runtime library without loading it") {
        const CliResult result = run_cli({"--self-test"}, env);
        CHECK(result.exit_code == 0);
        CHECK_FALSE(fs::exists(marker));
    }

    SECTION("the decoy does show a load, so the check above can fail") {
        const fs::path wav = root.path() / "silence.wav";
        write_silent_wav(wav);
        const CliResult result = run_cli(
            {"transcribe", utf8(wav), "--model", utf8(wav), "--runtime", utf8(runtime)}, env);
        CHECK(result.exit_code == 1);
        CHECK(fs::exists(marker));
    }

    SECTION("the executable imports no Qt library") {
#if defined(_WIN32)
        // Its import table, not its bytes: the installed check names the Qt DLLs it looks for.
        const std::optional<std::vector<std::string>> imports =
            fv::cli::pe_imports(fv::clitest::cli_executable());
        REQUIRE(imports.has_value());
        REQUIRE_FALSE(imports->empty());
        for (const std::string& name : *imports) {
            INFO(name);
            CHECK_FALSE((name.starts_with("Qt") || name.starts_with("qt")));
        }
#elif defined(__linux__)
        // Its DT_NEEDED entries, not its bytes: "Qt6" also turns up in relocation data.
        const std::optional<std::vector<std::string>> needed =
            elf_needed(read_text(fv::clitest::cli_executable()));
        REQUIRE(needed.has_value());
        REQUIRE_FALSE(needed->empty());
        for (const std::string& name : *needed) {
            INFO(name);
            CHECK_FALSE((name.starts_with("libQt") || name.starts_with("libqt")));
        }
#endif
    }
}

TEST_CASE("version is the product and the version, and only as the whole command line",
          "[platform][test_main_entrypoint]") {
    const CliResult version = run_cli({"--version"}, {});
    CHECK(version.exit_code == 0);
    CHECK_THAT(version.out, StartsWith("FioriVoce "));
    CHECK(version.out.find('\n') == version.out.size() - 1);

    const CliResult extra = run_cli({"--version", "extra"}, {});
    CHECK(extra.exit_code == 2);
    CHECK(extra.out.empty());
    CHECK_THAT(extra.err, ContainsSubstring("Unknown command --version."));
}

TEST_CASE("an unknown or missing command is a usage error", "[platform][test_main_entrypoint]") {
    const CliResult unknown = run_cli({"--no-such-flag"}, {});
    CHECK(unknown.exit_code == 2);
    CHECK_THAT(unknown.err, ContainsSubstring("Unknown command --no-such-flag."));
    CHECK_THAT(unknown.err, ContainsSubstring("Usage: fiorivoce-cli"));

    const CliResult none = run_cli({}, {});
    CHECK(none.exit_code == 2);
    CHECK_THAT(none.err, ContainsSubstring("No command given."));
}

TEST_CASE("transcribe reports a bad command line before it loads anything",
          "[platform][test_main_entrypoint]") {
    fv::testkit::TempDataRoot root;
    const std::vector<EnvOverride> env = {data_dir(root.path())};

    const CliResult no_audio = run_cli({"transcribe"}, env);
    CHECK(no_audio.exit_code == 2);
    CHECK_THAT(no_audio.err, ContainsSubstring("No audio file given."));

    const CliResult no_model = run_cli({"transcribe", "a.wav"}, env);
    CHECK(no_model.exit_code == 2);
    CHECK_THAT(no_model.err,
               ContainsSubstring("No model given. Pass --model or set FIORIVOCE_MODEL."));

    const CliResult no_runtime = run_cli({"transcribe", "a.wav", "--model", "m.gguf"}, env);
    CHECK(no_runtime.exit_code == 2);
    CHECK_THAT(no_runtime.err, ContainsSubstring("No speech runtime given. Pass --runtime or set "
                                                 "FIORIVOCE_RUNTIME_DIR."));

    const CliResult bad_gpu = run_cli({"transcribe", "a.wav", "--gpu", "x"}, env);
    CHECK(bad_gpu.exit_code == 2);
    CHECK_THAT(bad_gpu.err, ContainsSubstring("--gpu needs a number"));
}

TEST_CASE("diagnostics entrypoint does not initialize gui or history",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "support.json";

    const CliResult result = run_cli({"--diagnostics", utf8(output)}, {data_dir(root.path())});

    REQUIRE(result.exit_code == 0);
    const Json report = Json::parse(read_text(output));
    CHECK_FALSE(report["app_version"].get<std::string>().empty());
    CHECK(report["schema"] == 1);
    CHECK_FALSE(fs::exists(root.path() / "fiorivoce.db"));
}

TEST_CASE("the diagnostics report reads the settings file in the data folder",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    write_text(root.path() / "fiorivoce_settings.json", "{\"selected_model\": \"remote\"}\n");
    const fs::path output = root.path() / "support.json";

    REQUIRE(run_cli({"--diagnostics", utf8(output)}, {data_dir(root.path())}).exit_code == 0);

    CHECK(Json::parse(read_text(output))["engine"]["selected_model"] == "remote");
}

TEST_CASE("the diagnostics report lists catalog models and the installed speech runtime",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    // Components live under LOCALAPPDATA (Windows) or XDG_DATA_HOME (Linux), which the run points
    // at the test's folder.
    const fs::path local = root.path() / "Local";
    const fs::path runtime = local / "FioriVoce" / "components" / "asr-nvidia-cpu";
    fs::create_directories(runtime);
    write_text(runtime / "manifest.json", "{\"version\": \"0.1.0-test\"}");
    write_text(runtime / ".installed", "0.1.0-test");
    write_text(root.path() / "fiorivoce_settings.json",
               "{\"local_asr_models\": {\"parakeet\": \"parakeet-v3\", \"custom\": "
               "\"C:/models/mine.gguf\"}, \"local_asr_devices\": {\"parakeet\": \"cpu\"}, "
               "\"fv_speech_runtime\": \"cpu\"}");
    const fs::path output = root.path() / "support.json";

    const CliResult result = run_cli(
        {"--diagnostics", utf8(output)},
        {data_dir(root.path()), {"LOCALAPPDATA", utf8(local)}, {"XDG_DATA_HOME", utf8(local)}});

    REQUIRE(result.exit_code == 0);
    const Json report = Json::parse(read_text(output));
    // Only catalog ids: a custom model path is never exported.
    CHECK(report["engine"]["local_models"] == Json::array({"parakeet-v3"}));
    CHECK(report["components"]["nemo_speech_runtime"] == "0.1.0-test");
    CHECK(report["platform"].contains("nvidia_gpu"));
}

TEST_CASE("the diagnostics report has no speech runtime version when none is installed",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "support.json";

    const CliResult result =
        run_cli({"--diagnostics", utf8(output)}, {data_dir(root.path()),
                                                  {"LOCALAPPDATA", utf8(root.path())},
                                                  {"XDG_DATA_HOME", utf8(root.path())}});

    REQUIRE(result.exit_code == 0);
    CHECK(Json::parse(read_text(output))["components"]["nemo_speech_runtime"].is_null());
}

TEST_CASE("diagnostics without a path prints usage and exits 2",
          "[platform][test_release_health]") {
    const CliResult missing = run_cli({"--diagnostics"}, {});
    CHECK(missing.exit_code == 2);
    CHECK_THAT(missing.err, StartsWith("usage: fiorivoce-cli --diagnostics [-h] output"));
    CHECK_THAT(missing.err, ContainsSubstring("the following arguments are required: output"));

    const CliResult surplus = run_cli({"--diagnostics", "a.json", "b.json", "--x"}, {});
    CHECK(surplus.exit_code == 2);
    CHECK_THAT(surplus.err, ContainsSubstring("unrecognized arguments: b.json --x"));

    const CliResult help = run_cli({"--diagnostics", "--help"}, {});
    CHECK(help.exit_code == 0);
    CHECK_THAT(help.out, ContainsSubstring("Nothing is uploaded."));
}

TEST_CASE("diagnostics parses its command line as argparse does",
          "[platform][test_release_health]") {
    // argparse takes a unique abbreviation of --help, even after the positional.
    for (const char* flag : {"--h", "--he", "--hel"}) {
        const CliResult help = run_cli({"--diagnostics", "out.json", flag}, {});
        INFO(flag);
        CHECK(help.exit_code == 0);
        CHECK_THAT(help.out, StartsWith("usage: fiorivoce-cli --diagnostics [-h] output"));
    }
    // "--helpx" is no abbreviation; "-" alone and a negative number are positionals.
    const CliResult unknown = run_cli({"--diagnostics", "out.json", "--helpx"}, {});
    CHECK(unknown.exit_code == 2);
    CHECK_THAT(unknown.err, ContainsSubstring("unrecognized arguments: --helpx"));
    for (const char* surplus : {"-", "-1", "-.5"}) {
        const CliResult extra = run_cli({"--diagnostics", "out.json", surplus}, {});
        INFO(surplus);
        CHECK(extra.exit_code == 2);
        CHECK_THAT(extra.err, ContainsSubstring(std::string("unrecognized arguments: ") + surplus));
    }
    // Arguments reach the child as given: a trailing backslash, quotes and spaces.
    const CliResult quoted = run_cli({"--diagnostics", "x\\", "y \"z\\\" w\\"}, {});
    CHECK(quoted.exit_code == 2);
    std::string quoted_err = quoted.err;
    std::erase(quoted_err, '\r');
    CHECK_THAT(quoted_err, ContainsSubstring("unrecognized arguments: y \"z\\\" w\\\n"));
    // After "--" everything is a positional, -h included (the surplus one keeps it from writing).
    const CliResult ended = run_cli({"--diagnostics", "--", "-h", "--help"}, {});
    CHECK(ended.exit_code == 2);
    CHECK(ended.out.empty());
    CHECK_THAT(ended.err, ContainsSubstring("unrecognized arguments: --help"));
}

TEST_CASE("diagnostics says why the report could not be written",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "no-such-folder" / "support.json";

    const CliResult result = run_cli({"--diagnostics", utf8(output)}, {data_dir(root.path())});

    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, ContainsSubstring("could not be written to " + utf8(output)));
}

TEST_CASE("diagnostics never overwrites an existing file", "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path output = root.path() / "support.json";
    write_text(output, "precious");

    const CliResult result = run_cli({"--diagnostics", utf8(output)}, {data_dir(root.path())});

    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, ContainsSubstring("already exists"));
    CHECK(read_text(output) == "precious");
}

TEST_CASE("source self test does not recursively spawn", "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path runtime = make_decoy_runtime(root.path());

    const CliResult result =
        run_cli({"--self-test"}, {data_dir(root.path()), {"FIORIVOCE_RUNTIME_DIR", utf8(runtime)}});

    REQUIRE(result.exit_code == 0);
    CHECK_THAT(result.out, ContainsSubstring("FioriVoce package self-test passed"));
    // It ends with the workflow smoke, as main.py's does, whose report line comes first.
    CHECK_THAT(result.out, ContainsSubstring("\"kind\": \"synthetic_service_lifecycle\""));
    CHECK(result.out.find("synthetic_service_lifecycle") < result.out.find("self-test passed"));
    CHECK_FALSE(fs::exists(root.path() / "fiorivoce.db"));
    // The process the test started is the only one: the self-test ran in it.
    if (result.processes) {
        CHECK(*result.processes == 1);
    }
}

TEST_CASE("self test checks that the runtime folder holds a runtime",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    const fs::path empty_folder = root.path() / "empty";
    fs::create_directories(empty_folder);

    SECTION("a folder without the library fails") {
        const CliResult result =
            run_cli({"--self-test"},
                    {data_dir(root.path()), {"FIORIVOCE_RUNTIME_DIR", utf8(empty_folder)}});
        CHECK(result.exit_code == 1);
        CHECK_THAT(result.err, ContainsSubstring("self-test failed"));
        CHECK_THAT(result.err, ContainsSubstring("No speech runtime in"));
        CHECK_THAT(result.out, !ContainsSubstring("passed"));
    }

    SECTION("the saved fv_runtime_dir is used when the environment names none") {
        const fs::path runtime = make_decoy_runtime(root.path());
        Json settings = {{"fv_runtime_dir", utf8(runtime)}};
        write_text(root.path() / "fiorivoce_settings.json", settings.dump());
        const CliResult result = run_cli({"--self-test"}, {data_dir(root.path())});
        CHECK(result.exit_code == 0);
        CHECK_THAT(result.out, ContainsSubstring(fv::runtime_library_name()));
    }

    SECTION("the environment wins over the saved folder") {
        Json settings = {{"fv_runtime_dir", utf8(make_decoy_runtime(root.path()))}};
        write_text(root.path() / "fiorivoce_settings.json", settings.dump());
        const CliResult result =
            run_cli({"--self-test"},
                    {data_dir(root.path()), {"FIORIVOCE_RUNTIME_DIR", utf8(empty_folder)}});
        CHECK(result.exit_code == 1);
    }

    SECTION("with no runtime configured it says so and passes") {
        const CliResult result = run_cli({"--self-test"}, {data_dir(root.path())});
        CHECK(result.exit_code == 0);
        CHECK_THAT(result.out, ContainsSubstring("none configured"));
    }

    SECTION("it counts only as the whole command line") {
        const CliResult result = run_cli({"--self-test", "extra"}, {data_dir(root.path())});
        CHECK(result.exit_code == 2);
    }
}

TEST_CASE("self test fails when the data folder cannot take a file",
          "[platform][test_release_health]") {
    fv::testkit::TempDataRoot root;
    // A file where the data folder should be.
    const fs::path blocked = root.path() / "not-a-folder";
    write_text(blocked, "x");

    const CliResult result = run_cli({"--self-test"}, {data_dir(blocked)});

    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, ContainsSubstring("self-test failed"));
}

TEST_CASE("transcribe decodes a recording through the command line", "[decode]") {
    const auto runtime = fv::testkit::get_env("FIORIVOCE_TEST_RUNTIME_DIR");
    const auto model = fv::testkit::get_env("FIORIVOCE_TEST_MODEL");
    const auto wav = fv::testkit::get_env("FIORIVOCE_TEST_WAV");
    if (!runtime || !model || !wav) {
        SKIP("Set FIORIVOCE_TEST_RUNTIME_DIR, FIORIVOCE_TEST_MODEL and FIORIVOCE_TEST_WAV.");
    }
    fv::testkit::TempDataRoot root;

    const CliResult plain = run_cli({"transcribe", *wav, "--model", *model, "--runtime", *runtime},
                                    {data_dir(root.path())}, std::chrono::minutes(5));
    REQUIRE(plain.exit_code == 0);
    INFO("stdout: " << plain.out << "stderr: " << plain.err);
    CHECK(plain.out.size() > 1);
    CHECK_THAT(plain.err, ContainsSubstring("runtime "));

    // The environment names the model and runtime when the options do not.
    const CliResult from_env = run_cli(
        {"transcribe", *wav, "--timestamps"},
        {data_dir(root.path()), {"FIORIVOCE_MODEL", *model}, {"FIORIVOCE_RUNTIME_DIR", *runtime}},
        std::chrono::minutes(5));
    REQUIRE(from_env.exit_code == 0);
    CHECK_THAT(from_env.out, StartsWith("["));
}
