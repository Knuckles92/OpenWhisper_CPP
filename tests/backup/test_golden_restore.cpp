// Golden vectors of the restore and schedule rules (tools/parity/cases/services.backup.py's
// _check_staged_file, services.backup_manager.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "backup/backup.h"
#include "backup/detail/staged.h"
#include "backup/names.h"
#include "backup/preferences.h"
#include "backup/testing/sample_data.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using fv::Json;
using fv::backup::BackupError;
using fv::backup::BackupPreferences;
using fv::golden::Case;

namespace {

// A fresh folder below the test's data root, resolved as Path.resolve() gives it.
fs::path scratch(const fv::testkit::TempDataRoot& temp) {
    const fs::path folder = temp.path() / fv::path_from_utf8(fv::random_hex(12));
    fs::create_directories(folder);
    return fs::weakly_canonical(folder);
}

std::vector<std::string> sorted(const std::set<std::string>& values) {
    return {values.begin(), values.end()};
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

} // namespace

TEST_CASE("_check_staged_file matches OpenWhisper", "[golden]") {
    // The vectors use OpenWhisper's file names, read here through legacy_identity().
    const auto& names = fv::backup::names_for(fv::legacy_identity());
    fv::testkit::TempDataRoot temp;
    fv::golden::for_each("services.backup", "_check_staged_file", [&](const Case& c) {
        const fs::path stage = scratch(temp) / ("stage-" + std::string(32, '0'));
        fs::create_directories(stage / "data");
        const auto files = c.arg<Json>(2);
        for (const auto& [rel, text] : files.items()) {
            fv::backup::testing::write_bytes(stage / "data" / fv::path_from_utf8(rel),
                                             text.get<std::string>());
        }
        std::set<std::string> listed;
        for (const auto& name : c.arg<std::vector<std::string>>(3)) {
            listed.insert(name);
        }
        Json actual = Json::object();
        try {
            fv::backup::detail::check_staged_file(stage, c.arg<Json>(0), c.arg<Json>(1), listed,
                                                  names);
        } catch (const BackupError& failure) {
            actual["error"] = failure.what();
        }
        actual["listed"] = sorted(listed);
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("next_backup_at matches OpenWhisper's snapshot", "[golden]") {
    fv::golden::for_each("services.backup_manager", "next_backup_at", [](const Case& c) {
        const auto actual = fv::backup::next_backup_at(BackupPreferences::from_json(c.arg<Json>(0)),
                                                       c.arg<double>(1), c.arg<double>(2));
        CHECK(fv::golden::equal(c.result<Json>(), actual ? Json(*actual) : Json()));
    });
}

TEST_CASE("_load_preferences matches OpenWhisper", "[golden]") {
    fv::testkit::TempDataRoot temp;
    const auto check = [&](const Case& c, const std::optional<std::string>& bytes) {
        const fs::path folder = scratch(temp);
        if (bytes) {
            fv::backup::testing::write_bytes(folder / fv::backup::kPreferencesFile, *bytes);
        }
        const BackupPreferences loaded = fv::backup::load_preferences(folder, c.arg<double>(1));
        const Json actual = loaded.to_json();
        CHECK(fv::golden::equal(c.result<Json>(), actual));
        // The kind a later save writes back is Python's too (an int stays an int).
        const auto kind = [](const Json& value) {
            return value.is_boolean()          ? 'b'
                   : value.is_number_integer() ? 'i'
                   : value.is_number_float()   ? 'f'
                                               : '?';
        };
        CHECK(kind(c.result<Json>().at("last_auto_at")) == kind(actual.at("last_auto_at")));
    };
    fv::golden::for_each("services.backup_manager", "_load_preferences", [&](const Case& c) {
        const Json text = c.arg<Json>(0);
        check(c, text.is_null() ? std::nullopt : std::optional(text.get<std::string>()));
    });
    fv::golden::for_each("services.backup_manager", "_load_preferences_bytes", [&](const Case& c) {
        const std::string hex = c.arg<std::string>(0);
        std::string bytes;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            bytes += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
        }
        check(c, bytes);
    });
}

TEST_CASE("_prune_automatic_backups matches OpenWhisper on Windows paths", "[golden]") {
#ifndef _WIN32
    SKIP("the vectors hold Windows paths");
#endif
    fv::testkit::TempDataRoot temp;
    fv::golden::for_each("services.backup_manager", "_prune_automatic_backups", [&](const Case& c) {
        const fs::path root = scratch(temp);
        const std::string root_text = fv::path_to_utf8(root);
        fs::create_directories(root / "backups");
        std::vector<std::string> made;
        for (const auto& rel : c.arg<std::vector<std::string>>(1)) {
            if (rel.ends_with("/")) {
                fs::create_directories(root / fv::path_from_utf8(rel));
                continue;
            }
            fv::backup::testing::write_bytes(root / fv::path_from_utf8(rel), "x");
            made.push_back(rel);
        }
        BackupPreferences preferences;
        preferences.destination = fv::path_to_utf8(root / "backups");
        preferences.retention_count = c.arg<int>(2);
        for (const auto& entry : c.arg<std::vector<std::string>>(0)) {
            preferences.scheduled_archives.push_back(
                entry.starts_with("@")
                    ? root_text + "/" + entry.substr(1)
                    : fv::path_to_utf8((root / fv::path_from_utf8(entry)).make_preferred()));
        }
        if (c.raises()) {
            CHECK(c.raises_type() == "FileNotFoundError");
            CHECK_THROWS_AS(fv::backup::prune_automatic_backups(preferences, fv::legacy_identity()),
                            fs::filesystem_error);
            return;
        }
        fv::backup::prune_automatic_backups(preferences, fv::legacy_identity());
        std::vector<std::string> removed;
        for (const auto& rel : made) {
            if (!fs::exists(root / fv::path_from_utf8(rel))) {
                removed.push_back(rel);
            }
        }
        std::ranges::sort(removed);
        Json left = Json::array();
        for (std::string entry : preferences.scheduled_archives) {
            replace_all(entry, root_text, "<root>");
            left.push_back(entry);
        }
        CHECK(fv::golden::equal(c.result<Json>(),
                                Json{{"removed", removed}, {"scheduled_archives", left}}));
    });
}

TEST_CASE("_AUTO_NAME matches OpenWhisper's names", "[golden]") {
    fv::golden::for_each("services.backup_manager", "_auto_name", [](const Case& c) {
        CHECK(fv::backup::is_auto_backup_name(c.arg<std::string>(0), fv::legacy_identity()) ==
              c.result<bool>());
    });
}

TEST_CASE("automatic archive names carry FioriVoce's product and extension", "[backup]") {
    const auto at = std::chrono::sys_days{std::chrono::year{2026} / 11 / 15} +
                    std::chrono::hours(13) + std::chrono::minutes(4) +
                    std::chrono::milliseconds(5'999);
    const std::string name = fv::backup::auto_backup_name(fv::identity(), at, "0123456789ab");
    CHECK(name == "FioriVoce-auto-20261115T130405Z-0123456789ab.fvbackup");
    CHECK(fv::backup::is_auto_backup_name(name, fv::identity()));
    CHECK_FALSE(fv::backup::is_auto_backup_name(name, fv::legacy_identity()));
}
