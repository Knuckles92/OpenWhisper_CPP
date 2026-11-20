// Where settings live: one place per computer, whoever launches the app. Ported from OpenWhisper's
// tests/test_data_root.py; its git-checkout and working-directory cases are dropped on purpose
// (architecture 3.1: a source checkout never becomes the data root).

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "test_support.h"

using fv::identity;
using fv::MapEnvReader;
using fv::Paths;
using fv::test::ScopedCwd;
using fv::test::TempDir;

namespace {

// The `places` fixture: a per-user folder (by the platform's own variable), a checkout and
// somewhere else, with the data-directory variable unset.
struct Places {
    TempDir temp;
    std::filesystem::path base = temp.path() / "per-user-base";
    std::filesystem::path per_user = base / "OpenWhisper_CPP";
    std::filesystem::path checkout = temp.path() / "checkout";
    std::filesystem::path elsewhere = temp.path() / "elsewhere";
    MapEnvReader env;

    Places() {
        std::filesystem::create_directories(checkout);
        std::filesystem::create_directories(elsewhere);
#if defined(_WIN32)
        env.set("LOCALAPPDATA", fv::path_to_utf8(base));
#elif defined(__APPLE__)
        env.set("HOME", fv::path_to_utf8(temp.path()));
        base = temp.path() / "Library" / "Application Support";
        per_user = base / "OpenWhisper_CPP";
#else
        env.set("XDG_DATA_HOME", fv::path_to_utf8(base));
#endif
    }
};

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
}

} // namespace

TEST_CASE("a source tree without git shares the per user directory", "[test_data_root]") {
    // from tests/test_data_root.py::test_a_source_tree_without_git_shares_the_per_user_directory
    Places places;
    ScopedCwd cwd(places.elsewhere);

    const auto paths = Paths::resolve(identity(), places.env);

    CHECK(paths.data_root() == places.per_user);
    CHECK(std::filesystem::is_directory(places.per_user));
    CHECK(paths.user_file(identity().settings_file) ==
          places.per_user / "openwhisper_cpp_settings.json");
}

TEST_CASE("it does not matter where it was started", "[test_data_root]") {
    // from tests/test_data_root.py::test_it_does_not_matter_where_it_was_started
    Places places;
    ScopedCwd cwd(places.checkout);

    CHECK(Paths::resolve(identity(), places.env).data_root() == places.per_user);
}

TEST_CASE("the environment variable wins", "[test_data_root]") {
    // from tests/test_data_root.py::test_the_environment_variable_wins
    Places places;
    write_text(places.checkout / identity().settings_file, "{}");
    places.env.set("OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(places.temp.path() / "chosen"));

    const auto paths = Paths::resolve(identity(), places.env);

    CHECK(paths.data_root() == places.temp.path() / "chosen");
    CHECK(std::filesystem::is_directory(places.temp.path() / "chosen"));
}

TEST_CASE("frozen builds use the per user directory", "[test_data_root]") {
    // from tests/test_data_root.py::test_frozen_builds_use_the_per_user_directory
    // OpenWhisper_CPP is always "frozen": settings in the working directory are never picked up.
    Places places;
    write_text(places.elsewhere / identity().settings_file, "{}");
    ScopedCwd cwd(places.elsewhere);

    CHECK(Paths::resolve(identity(), places.env).data_root() == places.per_user);
}

TEST_CASE("a git checkout as the working directory is not the data root") {
    Places places;
    std::filesystem::create_directories(places.checkout / ".git");
    write_text(places.checkout / identity().settings_file, "{}");
    ScopedCwd cwd(places.checkout);

    CHECK(Paths::resolve(identity(), places.env).data_root() == places.per_user);
}

TEST_CASE("the data directory variable is made absolute and an empty one is ignored") {
    Places places;
    ScopedCwd cwd(places.elsewhere);

    places.env.set("OPENWHISPER_CPP_DATA_DIR", "relative/./data/../chosen");
    const auto relative = Paths::resolve(identity(), places.env);
    CHECK(relative.data_root().is_absolute());
    CHECK(relative.data_root() == (places.elsewhere / "relative" / "chosen").make_preferred());

    places.env.set("OPENWHISPER_CPP_DATA_DIR", "");
    CHECK(Paths::resolve(identity(), places.env).data_root() == places.per_user);
}

TEST_CASE("a leading tilde in the data directory variable is the home folder") {
    Places places;
#if defined(_WIN32)
    places.env.set("USERPROFILE", fv::path_to_utf8(places.temp.path() / "home"));
#else
    places.env.set("HOME", fv::path_to_utf8(places.temp.path() / "home"));
#endif
    places.env.set("OPENWHISPER_CPP_DATA_DIR", "~/fv-data");

    const auto paths = Paths::resolve(identity(), places.env);

    CHECK(paths.data_root() == (places.temp.path() / "home" / "fv-data").make_preferred());
}

TEST_CASE("components and speech models stay in the per user directory") {
    // config.py components_root: never per data directory, so multi-gigabyte downloads are not
    // duplicated.
    Places places;
    places.env.set("OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(places.temp.path() / "chosen"));

    const auto paths = Paths::resolve(identity(), places.env);

    CHECK(paths.local_app_dir() == places.per_user);
    CHECK(paths.components_root() == places.per_user / "components");
    CHECK(paths.speech_models_root() == places.per_user / "speech-models");
}

TEST_CASE("the env file is in the data root even when it does not exist") {
    Places places;
    const auto paths = Paths::resolve(identity(), places.env);

    CHECK_FALSE(std::filesystem::exists(paths.env_file()));
    CHECK(paths.env_file() == places.per_user / ".env");
}

TEST_CASE("OpenWhisper's roots are its data directory variable, then its per user directory") {
    Places places;
    const auto legacy_per_user = places.base / "OpenWhisper";

    CHECK(Paths::resolve(identity(), places.env).openwhisper_roots() ==
          std::vector<std::filesystem::path>{legacy_per_user});

    places.env.set("OPENWHISPER_DATA_DIR", fv::path_to_utf8(places.temp.path() / "ow-data"));
    CHECK(Paths::resolve(identity(), places.env).openwhisper_roots() ==
          std::vector<std::filesystem::path>{places.temp.path() / "ow-data", legacy_per_user});

    // Pointing the variable at the per-user directory names it once.
    places.env.set("OPENWHISPER_DATA_DIR", fv::path_to_utf8(legacy_per_user));
    CHECK(Paths::resolve(identity(), places.env).openwhisper_roots() ==
          std::vector<std::filesystem::path>{legacy_per_user});
    CHECK_FALSE(std::filesystem::exists(legacy_per_user)); // never created
}

#if defined(_WIN32)
TEST_CASE("without LOCALAPPDATA the per user directory is in the home folder") {
    TempDir temp;
    MapEnvReader env{{"USERPROFILE", fv::path_to_utf8(temp.path())}};

    CHECK(Paths::resolve(identity(), env).data_root() == temp.path() / "OpenWhisper_CPP");
}
#endif

TEST_CASE("an environment pinned without a home never resolves to the real user's") {
    // A test that pins only another platform's variable must not land in the real per-user
    // folders: "~" is looked up in the password database for the process's environment only.
    TempDir temp;
    ScopedCwd cwd(temp.path());
    MapEnvReader env;
    env.set("OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(temp.path() / "data"));
    CHECK_FALSE(env.is_process());
    CHECK(fv::system_env().is_process());

    const auto paths = Paths::resolve(identity(), env);

    const std::filesystem::path working = std::filesystem::weakly_canonical(temp.path());
    const auto under_working = [&](const std::filesystem::path& path) {
        const auto relative = std::filesystem::weakly_canonical(std::filesystem::absolute(path))
                                  .lexically_relative(working);
        return !relative.empty() && *relative.begin() != "..";
    };
    // With no home to expand it, "~" stays as written: a folder under the working directory.
    CHECK(under_working(paths.local_app_dir()));
    REQUIRE_FALSE(paths.openwhisper_roots().empty());
    for (const auto& root : paths.openwhisper_roots()) {
        CHECK(under_working(root));
    }
}

TEST_CASE("identity names every OpenWhisper_CPP file and OpenWhisper's for import") {
    const auto& app = fv::identity();
    CHECK(app.product == "OpenWhisper_CPP");
    CHECK(app.data_dir_name == "OpenWhisper_CPP");
    CHECK(app.env_prefix == "OPENWHISPER_CPP_");
    CHECK(app.settings_file == "openwhisper_cpp_settings.json");
    CHECK(app.database_file == "openwhisper-cpp.db");
    CHECK(app.log_file == "openwhisper-cpp.log");
    CHECK(app.lease_file == ".openwhisper-cpp-data.lock");
    CHECK(app.restore_dir == ".openwhisper-cpp-restore");
    CHECK(app.credential_service == "OpenWhisper_CPP");
    CHECK(app.backup_format == "openwhisper-cpp-backup");
    CHECK(app.backup_extension == ".fvbackup");
    CHECK(app.instance_key_prefix == "openwhisper-cpp-ui");

    const auto& legacy = fv::legacy_identity();
    CHECK(legacy.product == "OpenWhisper");
    CHECK(legacy.data_dir_name == "OpenWhisper");
    CHECK(legacy.env_prefix == "OPENWHISPER_");
    CHECK(legacy.settings_file == "openwhisper_settings.json");
    CHECK(legacy.database_file == "openwhisper.db");
    CHECK(legacy.log_file == "openwhisper.log");
    CHECK(legacy.lease_file == ".openwhisper-data.lock");
    CHECK(legacy.restore_dir == ".openwhisper-restore");
    CHECK(legacy.credential_service == "OpenWhisper");
    CHECK(legacy.backup_format == "openwhisper-backup");
    CHECK(legacy.backup_extension == ".owbackup");
    CHECK(legacy.instance_key_prefix == "openwhisper-ui");

    CHECK(app.wake_name == "openwhispercpp");
    CHECK(app.wake_name_spaced == "open whisper c p p");
    CHECK(legacy.wake_name == "openwhisper");
    CHECK(legacy.wake_name_spaced == "open whisper");
    // A stored value keeps OpenWhisper's spelling in both apps.
    CHECK(fv::legacy_builtin_agent_id == "openwhisper");
}

TEST_CASE("paths round-trip through UTF-8") {
    const std::string text = "C\xC3\xA9line/\xE6\x97\xA5\xE6\x9C\xAC/\xF0\x9F\x8E\xA4.wav";
    CHECK(fv::path_to_utf8(fv::path_from_utf8(text)) == text);
}

TEST_CASE("a pinned environment with its per-user folders names nothing outside the test") {
    // OpenWhisper's override moves neither: config.py components_root() and local_asr/cache.py
    // keep components and speech models in local_app_dir(). A pinned environment that sets only
    // the data folder leaves "~" as written, so the test rigs pin the per-user folders too.
    fv::test::TempDir temp;
    MapEnvReader bare{{"OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(temp.path() / "data")}};
    const auto loose = Paths::resolve(identity(), bare);
    CHECK_FALSE(loose.components_root().is_absolute()); // "~/..." below the working directory

    MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(temp.path() / "data")}};
    fv::pin_per_user_folders(env, temp.path() / "per-user");
    const auto paths = Paths::resolve(identity(), env);

    const auto inside = [&](const std::filesystem::path& path) {
        const auto relative = path.lexically_relative(temp.path());
        return path.is_absolute() && !relative.empty() && *relative.begin() != "..";
    };
    CHECK(paths.data_root() == temp.path() / "data");
    CHECK(inside(paths.local_app_dir()));
    CHECK(inside(paths.components_root()));
    CHECK(inside(paths.speech_models_root()));
    REQUIRE_FALSE(paths.openwhisper_roots().empty());
    for (const auto& root : paths.openwhisper_roots()) {
        CHECK(inside(root));
    }
    // Moved by neither data-folder variable, as OpenWhisper's are not.
    env.set("OPENWHISPER_DATA_DIR", fv::path_to_utf8(temp.path() / "ow-data"));
    const auto moved = Paths::resolve(identity(), env);
    CHECK(moved.components_root() == paths.components_root());
    CHECK(moved.speech_models_root() == paths.speech_models_root());
    CHECK(moved.openwhisper_roots().front() == temp.path() / "ow-data");
}
