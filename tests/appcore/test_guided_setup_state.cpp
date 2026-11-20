// Guided setup eligibility (services/guided_setup.py): OpenWhisper's eligibility tests of
// tests/test_guided_setup.py and the golden vectors of services.guided_setup over folder
// fixtures.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "appcore/guided_setup_state.h"
#include "base/identity.h"
#include "base/json.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::SettingsStore;
namespace guided_setup = fv::appcore::guided_setup_state;

namespace {

constexpr const char* kModule = "services.guided_setup";

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The fixture of a vector: a trailing "/" makes an empty folder, anything else an empty file;
// OpenWhisper's database name is FioriVoce's here.
void make_files(const std::filesystem::path& root, const std::vector<std::string>& files) {
    std::filesystem::create_directories(root);
    for (std::string name : files) {
        const std::string legacy(fv::legacy_identity().database_file);
        if (name.starts_with(legacy)) {
            name = std::string(fv::identity().database_file) + name.substr(legacy.size());
        }
        if (name.ends_with('/')) {
            std::filesystem::create_directories(root / name);
        } else {
            write_text(root / name, "");
        }
    }
}

} // namespace

// from
// tests/test_guided_setup.py::test_new_user_invitation_survives_first_launch_writes_and_restart
TEST_CASE("new user invitation survives first launch writes and restart", "[test_guided_setup]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "settings.json";
    const bool other_writes_first = GENERATE(false, true);
    SettingsStore store(path);
    if (other_writes_first) {
        store.save_setting("app_style_tones", Json{{"chat", "casual"}});
    }
    guided_setup::initialize(store);
    CHECK(guided_setup::should_invite(store));
    store.save_setting("selected_model", "parakeet");
    SettingsStore reopened(path);
    guided_setup::initialize(reopened);
    CHECK(guided_setup::should_invite(reopened));
    CHECK(reopened.value("selected_model") == "parakeet");
    if (other_writes_first) {
        CHECK(reopened.value("app_style_tones") == Json{{"chat", "casual"}});
    }
}

// from tests/test_guided_setup.py::test_established_user_with_default_settings_is_not_new
TEST_CASE("established user with default settings is not new", "[test_guided_setup]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "settings.json";
    write_text(path, "{}");
    SettingsStore store(path);
    guided_setup::initialize(store);
    CHECK(store.value("guided_setup_state") == std::string(guided_setup::kExisting));
    CHECK_FALSE(guided_setup::should_invite(store));
}

// from tests/test_guided_setup.py::test_missing_settings_with_prior_data_does_not_invite
TEST_CASE("missing settings with prior data does not invite", "[test_guided_setup]") {
    const fv::testkit::TempDataRoot root;
    const std::string artifact =
        GENERATE(std::string(fv::identity().database_file),
                 std::string("transcription_history.json"), std::string("scratchpad.txt"),
                 std::string("recordings/old.wav"), std::string("meetings/old.json"));
    const auto path = root.path() / artifact;
    write_text(path, "existing data");
    SettingsStore store(root.path() / "settings.json");
    guided_setup::initialize(store);
    CHECK_FALSE(guided_setup::should_invite(store));
    CHECK(read_text(path) == "existing data");
}

// from
// tests/test_guided_setup.py::test_corrupt_existing_settings_are_neither_overwritten_nor_invited
TEST_CASE("corrupt existing settings are neither overwritten nor invited", "[test_guided_setup]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "settings.json";
    write_text(path, "broken JSON");
    SettingsStore store(path);
    guided_setup::initialize(store);
    CHECK_FALSE(guided_setup::should_invite(store));
    CHECK(read_text(path) == "broken JSON");
}

// Path.exists() raises for a path it cannot check (only a missing one reads as absent), and
// initialize then leaves the state unset: data that cannot be seen is not taken for none.
TEST_CASE("prior data that cannot be checked is not taken for none", "[guided_setup_state]") {
#ifdef _WIN32
    SKIP("Needs a folder whose entries cannot be checked (POSIX permissions)");
#else
    const fv::testkit::TempDataRoot root;
    const auto locked = root.path() / "locked";
    std::filesystem::create_directories(locked);
    CHECK_FALSE(guided_setup::has_prior_data(locked));
    std::filesystem::permissions(locked, std::filesystem::perms::none);
    std::error_code probe;
    std::filesystem::exists(locked / "probe", probe);
    if (!probe) {
        std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
        SKIP("Permissions do not apply to this user");
    }
    CHECK_THROWS_AS(guided_setup::has_prior_data(locked), std::filesystem::filesystem_error);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
#endif
}

TEST_CASE("only the five states are saved", "[guided_setup_state]") {
    const fv::testkit::TempDataRoot root;
    SettingsStore store(root.path() / "settings.json");
    guided_setup::save_state(guided_setup::kCompleted, store);
    CHECK(guided_setup::is_completed(store));
    CHECK_FALSE(guided_setup::should_invite(store));
    try {
        guided_setup::save_state("done", store);
        FAIL("an unknown state was saved");
    } catch (const fv::py::ValueError& error) {
        CHECK(std::string(error.what()) == "Unknown guided setup state: done");
    }
    CHECK(store.value("guided_setup_state") == "completed");
}

TEST_CASE("guided setup replays the golden vectors", "[golden][guided_setup_state]") {
    const fv::testkit::TempDataRoot root;
    int counter = 0;
    const auto fresh = [&] {
        return root.path() / ("case-" + std::to_string(counter++));
    };

    fv::golden::for_each(kModule, "_has_prior_data", [&](const fv::golden::Case& c) {
        const auto folder = fresh();
        make_files(folder, c.arg<std::vector<std::string>>(0));
        if (c.raises()) {
            CHECK(c.raises_type() == "NotADirectoryError");
            CHECK_THROWS_AS(guided_setup::has_prior_data(folder),
                            std::filesystem::filesystem_error);
            return;
        }
        CHECK(guided_setup::has_prior_data(folder) == c.result<bool>());
    });

    fv::golden::for_each(kModule, "initialize", [&](const fv::golden::Case& c) {
        const auto folder = fresh();
        make_files(folder, c.arg<std::vector<std::string>>(1));
        const auto path = folder / "settings.json";
        if (const auto text = c.arg<std::optional<std::string>>(0)) {
            write_text(path, *text);
        }
        {
            SettingsStore store(path);
            guided_setup::initialize(store);
        }
        const SettingsStore reopened(path);
        const Json expected = c.result<Json>();
        CHECK(guided_setup::should_invite(reopened) == expected.at("invite").get<bool>());
        CHECK(guided_setup::is_completed(reopened) == expected.at("completed").get<bool>());
        CHECK(std::filesystem::exists(path) == expected.at("exists").get<bool>());
        if (!expected.at("settings").is_null()) {
            CHECK(fv::golden::equal(expected.at("settings"), fv::py::loads(read_text(path))));
        }
        if (!expected.at("raw").is_null()) {
            CHECK(read_text(path) == expected.at("raw").get<std::string>());
        }
    });

    fv::golden::for_each(kModule, "save_state", [&](const fv::golden::Case& c) {
        const auto folder = fresh();
        std::filesystem::create_directories(folder);
        SettingsStore store(folder / "settings.json");
        const std::string state = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(guided_setup::save_state(state, store), fv::py::ValueError);
            CHECK_FALSE(std::filesystem::exists(store.path()));
            return;
        }
        guided_setup::save_state(state, store);
        CHECK(fv::golden::equal(c.result<Json>(), *store.snapshot()));
    });
}
