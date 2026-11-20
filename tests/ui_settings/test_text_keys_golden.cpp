// What the text-model endpoints add to Settings, replayed from OpenWhisper's own answers
// (tools/parity/cases/ui_qt.dialogs.settings_api_keys.py and ui_qt.dialogs.settings_dialog.py):
// the credential owners and rows of the API keys page with text endpoints in the settings, and the
// AI cleanup and text-near-cursor items of Home's where strip.
//
// OpenWhisper lists TypeSafe's key last, and so do the owners and rows here.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <QStringList>

#include "appcore/api_keys_model.h"
#include "golden/golden.h"
#include "home_support.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/home_page.h"

namespace golden = fv::golden;
using fv::Json;

namespace {

constexpr const char* kKeys = "ui_qt.dialogs.settings_api_keys";

fv::CredentialSource sourceOf(const std::string& name) {
    if (name == "stored") {
        return fv::CredentialSource::Store;
    }
    if (name == "environment") {
        return fv::CredentialSource::Environment;
    }
    if (name == "dotenv") {
        return fv::CredentialSource::DotEnv;
    }
    return fv::CredentialSource::None;
}

std::vector<std::vector<std::string>> rowsOf(const Json& rows) {
    std::vector<std::vector<std::string>> all;
    for (const Json& row : rows) {
        all.push_back(row.get<std::vector<std::string>>());
    }
    return all;
}

} // namespace

TEST_CASE("golden ui_qt.dialogs.settings_api_keys key_entries with text-model owners",
          "[golden][widget]") {
    golden::for_each(kKeys, "key_entries", [](const golden::Case& c) {
        const Json settings = c.arg<Json>(0);
        const Json expected = c.result<Json>();

        std::vector<std::vector<std::string>> owners;
        for (const auto& owner : fv::credentialOwners(settings)) {
            owners.push_back({owner.env_name, owner.owner, owner.use});
        }
        CHECK(owners == rowsOf(expected.at("owners")));

        std::vector<std::vector<std::string>> entries;
        for (const auto& entry : fv::credentialEntries(settings)) {
            entries.push_back({entry.env_name, entry.label});
        }
        CHECK(entries == rowsOf(expected.at("entries")));
    });
}

TEST_CASE(
    "golden ui_qt.dialogs.settings_api_keys rail_value counts the text-model and TypeSafe rows",
    "[golden][widget]") {
    golden::for_each(kKeys, "rail_value", [](const golden::Case& c) {
        const Json settings = c.arg<Json>(0);
        std::map<std::string, std::string> sources;
        const auto merged_profiles = c.arg<Json>(1);
        for (const auto& [name, source] : merged_profiles.items()) {
            sources[name] = source.get<std::string>();
        }
        const std::string expected = c.result<Json>().at("value").get<std::string>();

        const auto entries = fv::credentialEntries(settings);
        CHECK(fv::appcore::api_keys_rail_value(entries, [&sources](std::string_view name) {
                  const auto found = sources.find(std::string(name));
                  return found == sources.end() ? fv::CredentialSource::None
                                                : sourceOf(found->second);
              }) == expected);
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_dialog where strip: AI cleanup and text near the cursor",
          "[golden][widget]") {
    fvtest::HomeRig rig;
    // The vectors were recorded where text near the cursor can be read (Windows).
    rig.context().service<fv::HomeSources>().textReadingSupported = [] {
        return true;
    };
    fv::HomePage& home = rig.home();
    const auto mine = [](const QString& item) {
        return item.startsWith(QStringLiteral("AI cleanup")) ||
               item.startsWith(QStringLiteral("Text near the cursor"));
    };
    const auto keep = [&mine](const Json& items) {
        QStringList kept;
        for (const auto& item : items) {
            const QString text = QString::fromStdString(item.get<std::string>());
            if (mine(text)) {
                kept.append(text);
            }
        }
        return kept;
    };
    golden::for_each("ui_qt.dialogs.settings_dialog", "SettingsDialog._refresh_home",
                     [&](const golden::Case& c) {
                         const Json settings = c.arg<Json>(0);
                         const Json expected = c.result<Json>();
                         const fv::HomeSummary summary = home.composeSummary(settings);
                         QStringList local;
                         QStringList cloud;
                         for (const QString& item : summary.localItems) {
                             if (mine(item)) {
                                 local.append(item);
                             }
                         }
                         for (const QString& item : summary.cloudItems) {
                             if (mine(item)) {
                                 cloud.append(item);
                             }
                         }
                         CHECK(local == keep(expected.at("local")));
                         CHECK(cloud == keep(expected.at("cloud")));
                     });
}
