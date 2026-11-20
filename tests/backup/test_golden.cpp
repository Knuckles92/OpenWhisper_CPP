// Golden vectors of services/backup.py's pure half and of the actions loader it relies on
// (tools/parity/cases/services.backup.py, services.actions.model.py).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>

#include "backup/backup.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/action_rows.h"
#include "backup/members.h"
#include "backup/names.h"
#include "backup/sanitize.h"
#include "base/identity.h"
#include "base/platform_info.h"
#include "base/utf8.h"
#include "golden/golden.h"

namespace fs = std::filesystem;
using fv::Json;
using fv::backup::BackupError;
using fv::golden::Case;

namespace {

const fv::backup::Names& ow_names() {
    return fv::backup::names_for(fv::legacy_identity());
}

// {"error": message} as the case wrappers record a BackupError.
Json error(const BackupError& failure) {
    return Json{{"error", failure.what()}};
}

bool on_vector_platform() {
    return fv::golden::load("services.actions.model")
               .at("pins")
               .at("platform")
               .get<std::string>() == fv::python_platform();
}

} // namespace

TEST_CASE("_scrub matches OpenWhisper", "[golden]") {
    fv::golden::for_each("services.backup", "_scrub", [](const Case& c) {
        const bool keep = c.arg_count() > 1 && c.arg<bool>(1);
        CHECK(fv::golden::equal(c.result<Json>(), fv::backup::scrub(c.arg<Json>(0), keep)));
    });
}

TEST_CASE("_sanitize_settings matches OpenWhisper", "[golden]") {
    fv::golden::for_each("services.backup", "_sanitize_settings", [](const Case& c) {
        const Json actual = fv::backup::sanitize_settings(c.arg<Json>(0));
        // Key order is part of the file the bundle writes.
        CHECK(c.result<Json>().dump() == actual.dump());
    });
}

TEST_CASE("_member_parts matches OpenWhisper", "[golden]") {
    fv::golden::for_each("services.backup", "_member_parts", [](const Case& c) {
        Json actual;
        try {
            actual = fv::backup::member_parts(c.arg<Json>(0));
        } catch (const BackupError& failure) {
            actual = error(failure);
        }
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("_safe_member matches OpenWhisper with its own names", "[golden]") {
    fv::golden::for_each("services.backup", "_safe_member", [](const Case& c) {
        Json actual;
        try {
            actual = fv::backup::safe_member(c.arg<Json>(0), ow_names());
        } catch (const BackupError& failure) {
            actual = error(failure);
        }
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("_safe_member accepts FioriVoce's core files where OpenWhisper accepts its own",
          "[golden]") {
    const auto& names = fv::backup::names_for(fv::identity());
    fv::golden::for_each("services.backup", "_safe_member", [&](const Case& c) {
        // A name already spelled the FioriVoce way is foreign to OpenWhisper, not to FioriVoce.
        if (!c.args()[0].is_string() ||
            c.arg<std::string>(0).find("fiorivoce") != std::string::npos) {
            return;
        }
        const std::string name = c.arg<std::string>(0);
        Json actual;
        try {
            actual = fv::backup::safe_member(fv::golden::rebrand(name), names);
        } catch (const BackupError& failure) {
            actual = error(failure);
        }
        Json expected = c.result<Json>();
        if (expected.is_string()) {
            expected = fv::golden::rebrand(expected.get<std::string>());
        }
        CHECK(fv::golden::equal(expected, actual));
    });
}

TEST_CASE("_relative_under matches OpenWhisper on Windows paths", "[golden]") {
#ifndef _WIN32
    SKIP("the vectors hold Windows paths");
#endif
    fv::golden::for_each("services.backup", "_relative_under", [](const Case& c) {
        Json actual;
        try {
            const auto value = fv::backup::relative_under(c.arg<std::string>(0),
                                                          fv::path_from_utf8(c.arg<std::string>(1)),
                                                          c.arg<std::string>(2), ow_names());
            actual = value ? Json(*value) : Json();
        } catch (const BackupError& failure) {
            actual = error(failure);
        }
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("_meeting_session_updates matches OpenWhisper", "[golden]") {
    fv::golden::for_each("services.backup", "_meeting_session_updates", [](const Case& c) {
        std::set<std::string> columns;
        for (const auto& column : c.arg<std::vector<std::string>>(0)) {
            columns.insert(column);
        }
        Json actual = Json::array();
        for (const auto& [column, value] :
             fv::backup::meeting_session_updates(columns, c.arg<std::string>(1))) {
            Json shown;
            if (const auto* text = std::get_if<std::string>(&value)) {
                const bool token = column == "host_token" || column == "guest_token";
                shown = token ? Json("<token:" + std::to_string(text->size()) + ">") : Json(*text);
            }
            actual.push_back(Json::array({column, shown}));
        }
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("load_actions matches OpenWhisper", "[golden]") {
    // Shortcuts normalize through the platform's key vocabulary; elsewhere the field is left out.
    const bool same_platform = on_vector_platform();
    fv::golden::for_each("services.actions.model", "load_actions", [&](const Case& c) {
        Json actual = Json::array();
        for (const auto& row : fv::backup::detail::load_action_rows(c.arg<Json>(0))) {
            actual.push_back(row.to_json());
        }
        Json expected = c.result<Json>();
        if (!same_platform) {
            for (auto& row : expected) {
                row.erase("hotkey");
            }
            for (auto& row : actual) {
                row.erase("hotkey");
            }
        }
        CHECK(expected.dump() == actual.dump());
    });
}
