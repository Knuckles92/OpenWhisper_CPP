// Golden vectors of services/update_contract.py, services/app_update.py and the update dialog's
// text methods, replayed against fv_update (tests/golden/*.json, tools/parity/cases/*.py).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "pyport/unicode.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "update/check.h"
#include "update/release_feed.h"
#include "update/schedule.h"
#include "update/trust.h"
#include "update/update_contract.h"
#include "update/update_copy.h"
#include "update_support.h"

using fv::Json;
using fv::golden::Case;
namespace update = fv::update;
using namespace fv::update::test;

namespace {

constexpr std::string_view kModule = "services.app_update";

std::string pinned_repo() {
    return fv::golden::load(kModule).at("pins").at("github_repo").get<std::string>();
}

// The configuration a vector's call ran under: the repository (rebranded with the payload when
// `rebranded`) and the feed variable, which the C++ side validates as Python does.
update::FeedConfig feed_for(const std::optional<std::string>& feed, bool rebranded) {
    update::FeedConfig config;
    config.source.kind = update::UpdateSource::Kind::GitHub;
    config.source.repo = rebranded ? rebrand_text(pinned_repo()) : pinned_repo();
    if (feed && update::is_loopback_feed_url(*feed)) {
        config.override_url = *feed;
    }
    return config;
}

update::UpdateStatus status_from(std::string_view name) {
    if (name == "update_available") {
        return update::UpdateStatus::UpdateAvailable;
    }
    return name == "development" ? update::UpdateStatus::Development
                                 : update::UpdateStatus::UpToDate;
}

update::ApplyMode mode_from(std::string_view name) {
    if (name == "native") {
        return update::ApplyMode::Native;
    }
    return name == "setup" ? update::ApplyMode::Setup : update::ApplyMode::NotifyOnly;
}

update::DownloadPhase phase_from(std::string_view name) {
    using update::DownloadPhase;
    for (const auto phase :
         {DownloadPhase::Downloading, DownloadPhase::Verifying, DownloadPhase::Extracting,
          DownloadPhase::Opening, DownloadPhase::Restarting, DownloadPhase::RollingBack}) {
        if (update::phase_name(phase) == name) {
            return phase;
        }
    }
    FAIL("unknown phase " << name);
    return DownloadPhase::Downloading;
}

// A case file's byte spec: a str of latin-1 bytes, or ["repeat", text, n].
std::string bytes_of(const Json& spec) {
    std::string text;
    std::size_t times = 1;
    if (spec.is_array()) {
        text = spec.at(1).get<std::string>();
        times = spec.at(2).get<std::size_t>();
    } else {
        text = spec.get<std::string>();
    }
    std::string once;
    for (const char32_t cp : fv::py::to_code_points(text)) {
        once += static_cast<char>(static_cast<unsigned char>(cp));
    }
    std::string out;
    for (std::size_t i = 0; i < times; ++i) {
        out += once;
    }
    return out;
}

fv::http::Headers headers_of(const Json& pairs) {
    fv::http::Headers headers;
    for (const auto& pair : pairs) {
        headers.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
    }
    return headers;
}

} // namespace

TEST_CASE("update contract vectors", "[golden][update_contract]") {
    fv::golden::for_each("services.update_contract", "normalize_version", [](const Case& c) {
        CHECK(update::normalize_version(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("services.update_contract", "parse_strict_version", [](const Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(update::parse_strict_version(c.arg<std::string>(0)),
                            fv::py::ValueError);
            return;
        }
        const auto parsed = update::parse_strict_version(c.arg<std::string>(0));
        CHECK(std::vector<long long>(parsed.begin(), parsed.end()) ==
              c.result<std::vector<long long>>());
    });
    fv::golden::for_each("services.update_contract", "is_newer_version", [](const Case& c) {
        if (c.raises()) {
            CHECK_THROWS_AS(update::is_newer_version(c.arg<std::string>(0), c.arg<std::string>(1)),
                            fv::py::ValueError);
            return;
        }
        CHECK(update::is_newer_version(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<bool>());
    });
    fv::golden::for_each("services.update_contract", "setup_asset_name", [](const Case& c) {
        CHECK(fv::golden::equal_rebranded(c.result<std::string>(),
                                          update::setup_asset_name(c.arg<std::string>(0))));
    });
}

TEST_CASE("version and schedule vectors", "[golden][app_update]") {
    fv::golden::for_each(kModule, "parse_version", [](const Case& c) {
        const auto parsed = update::parse_version(c.arg<std::string>(0));
        CHECK(std::vector<long long>(parsed.begin(), parsed.end()) ==
              c.result<std::vector<long long>>());
    });
    fv::golden::for_each(kModule, "compare_versions", [](const Case& c) {
        CHECK(update::status_name(update::compare_versions(
                  c.arg<std::string>(0), c.arg<std::string>(1))) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "seconds_until_auto_check", [](const Case& c) {
        const auto channel = c.arg<std::string>(3);
        const auto wait = update::seconds_until_auto_check(
            c.arg<Json>(0), at(c.arg<std::string>(1)), std::chrono::seconds{c.arg<int>(2)},
            channel == "installer" ? update::InstallChannel::Installer
                                   : update::InstallChannel::Source,
            channel != "git");
        CHECK(wait == c.result<std::optional<double>>());
    });
    fv::golden::for_each(kModule, "parse_last_check_at", [](const Case& c) {
        const auto parsed = update::parse_last_check_at(c.arg<Json>(0));
        CHECK((parsed ? std::optional(parsed->isoformat()) : std::nullopt) ==
              c.result<std::optional<std::string>>());
    });
    fv::golden::for_each(kModule, "shows_update_notice", [](const Case& c) {
        CHECK(update::shows_update_notice(status_from(c.arg<std::string>(0)), c.arg<Json>(1)) ==
              c.result<bool>());
    });
}

TEST_CASE("release payload vectors", "[golden][app_update]") {
    fv::golden::for_each(kModule, "parse_release_payload", [](const Case& c) {
        const auto feed = feed_for(c.arg<std::optional<std::string>>(2), true);
        const auto payload = rebrand_json(c.arg<Json>(0));
        if (c.raises()) {
            CHECK(c.raises_type() == "AppUpdateError");
            CHECK_THROWS_AS(update::parse_release_payload(feed, payload, c.arg<bool>(1)),
                            update::AppUpdateError);
            return;
        }
        const auto release = update::parse_release_payload(feed, payload, c.arg<bool>(1));
        CHECK(fv::golden::equal(rebrand_json(c.result<Json>()), release_json(release)));
    });
    fv::golden::for_each(kModule, "select_newest_release", [](const Case& c) {
        const auto feed = feed_for(c.arg<std::optional<std::string>>(1), true);
        const auto payload = rebrand_json(c.arg<Json>(0));
        if (c.raises()) {
            CHECK(c.raises_type() == "AppUpdateError");
            CHECK_THROWS_AS(update::select_newest_release(feed, payload), update::AppUpdateError);
            return;
        }
        CHECK(fv::golden::equal(rebrand_json(c.result<Json>()),
                                release_json(update::select_newest_release(feed, payload))));
    });
    fv::golden::for_each(kModule, "_parse_named_asset", [](const Case& c) {
        const auto feed = feed_for(c.arg<std::optional<std::string>>(3), true);
        const auto asset =
            update::parse_named_asset(feed, rebrand_json(c.arg<Json>(0)),
                                      rebrand_text(c.arg<std::string>(1)), c.arg<std::string>(2));
        CHECK(fv::golden::equal(rebrand_json(c.result<Json>()), asset_json(asset)));
    });
    fv::golden::for_each(kModule, "_digest_to_sha256", [](const Case& c) {
        CHECK(update::digest_to_sha256(c.arg<Json>(0)) == c.result<std::optional<std::string>>());
    });
    fv::golden::for_each(kModule, "_asset_url_allowed", [](const Case& c) {
        const auto feed = feed_for(c.arg<std::optional<std::string>>(3), true);
        CHECK(update::asset_url_allowed(feed, rebrand_text(c.arg<std::string>(0)),
                                        c.arg<std::string>(1),
                                        rebrand_text(c.arg<std::string>(2))) == c.result<bool>());
    });
    fv::golden::for_each(kModule, "_fetch_release_checksums", [](const Case& c) {
        const auto raw = bytes_of(c.arg<Json>(0));
        if (c.raises()) {
            CHECK_THROWS_AS(update::parse_release_checksums(raw), std::invalid_argument);
            return;
        }
        Json actual = Json::object();
        for (const auto& [name, digest] : update::parse_release_checksums(raw)) {
            actual[name] = digest;
        }
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("trust rule vectors", "[golden][app_update]") {
    const auto repo = pinned_repo();
    fv::golden::for_each(kModule, "_redirect_url_allowed", [&](const Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(
                update::redirect_url_allowed(repo, c.arg<std::string>(0), c.arg<std::string>(1)),
                fv::py::ValueError);
            return;
        }
        CHECK(update::redirect_url_allowed(repo, c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<bool>());
    });
    fv::golden::for_each(kModule, "_parsed_url_is_well_formed", [](const Case& c) {
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(update::url_well_formed(c.arg<std::string>(0)), fv::py::ValueError);
            return;
        }
        CHECK(update::url_well_formed(c.arg<std::string>(0)) == c.result<bool>());
    });
    fv::golden::for_each(kModule, "_is_loopback_feed_url", [](const Case& c) {
        CHECK(update::is_loopback_feed_url(c.arg<std::string>(0)) == c.result<bool>());
    });
    fv::golden::for_each(kModule, "_rate_limit", [](const Case& c) {
        update::FetchFailure failure;
        failure.failure.kind = fv::download::NetworkFailure::Kind::HttpStatus;
        failure.failure.code = c.arg<int>(0);
        failure.failure.reason = "Forbidden";
        failure.headers = headers_of(c.arg<Json>(1));
        fv::MemorySettingsStore settings;
        fv::FakeClock clock(at(c.arg<std::string>(2)));
        const bool limited = update::is_rate_limited(failure);
        update::remember_api_rate_limit(failure, settings, clock);
        const Json actual =
            Json::array({limited, settings.value(fv::keys::update_api_retry_at.key())});
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
    fv::golden::for_each(kModule, "_describe_update_error", [&](const Case& c) {
        using Kind = fv::download::NetworkFailure::Kind;
        const auto kind = c.arg<std::string>(0);
        update::FetchFailure failure;
        if (kind == "http") {
            const auto detail = c.arg<Json>(1);
            failure.failure = {Kind::HttpStatus, detail.at(0).get<int>(),
                               detail.at(1).get<std::string>()};
        } else {
            failure.failure.kind = kind == "certificate" ? Kind::Certificate
                                   : kind == "url"       ? Kind::Url
                                                         : Kind::Other;
            failure.failure.reason = c.arg<std::string>(1);
        }
        failure.headers = headers_of(c.arg<Json>(2));
        CHECK(update::describe_update_error(failure, repo) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "update_check_failure_status", [](const Case& c) {
        CHECK(update::update_check_failure_status(c.arg<std::string>(0)) ==
              c.result<std::string>());
    });
}

TEST_CASE("update dialog text vectors", "[golden][app_update_dialog]") {
    fv::golden::for_each("ui_qt.dialogs.app_update_dialog", "texts", [](const Case& c) {
        const auto spec = c.arg<Json>(0);
        std::optional<update::UpdateCheckResult> result;
        if (!spec.is_null()) {
            update::UpdateCheckResult made;
            made.status = status_from(spec.at("status").get<std::string>());
            made.current_version = spec.at("current_version").get<std::string>();
            made.channel = spec.at("channel").get<std::string>() == "installer"
                               ? update::InstallChannel::Installer
                               : update::InstallChannel::Source;
            made.apply_mode = mode_from(spec.at("apply_mode").get<std::string>());
            made.can_apply = spec.at("can_apply").get<bool>();
            if (const auto& r = spec.at("release"); !r.is_null()) {
                update::ReleaseInfo release;
                release.version = r.at("version").get<std::string>();
                release.tag_name = "v" + release.version;
                release.prerelease = r.at("prerelease").get<bool>();
                const auto asset = [&](const Json& size, std::string name) {
                    std::optional<update::ReleaseAsset> made_asset;
                    if (!size.is_null()) {
                        made_asset = update::ReleaseAsset{"https://example/" + name, name,
                                                          size.get<std::int64_t>(),
                                                          std::string(64, 'a'), std::nullopt};
                    }
                    return made_asset;
                };
                release.setup_asset = asset(r.at("setup_size"), "setup.exe");
                release.native_asset = asset(r.at("native_size"), "native.zip");
                made.release = release;
            }
            result = made;
        }
        const auto* view = result ? &*result : nullptr;
        const auto error = c.arg<std::string>(1);
        Json progress = Json::array();
        const auto expected = c.result<Json>();
        for (const auto& row : expected.at("progress")) {
            const auto phase = phase_from(row.at(0).get<std::string>());
            const auto done = row.at(1).get<std::int64_t>();
            const auto total = row.at(2).get<std::int64_t>();
            progress.push_back(Json::array(
                {row.at(0), done, total, update::progress_detail_text(view, phase, done, total)}));
        }
        Json phases = Json::object();
        for (const auto& [name, text] : expected.at("phase_text").items()) {
            const auto line = update::phase_text(phase_from(name));
            phases[name] = line ? Json(*line) : Json(nullptr);
        }
        Json actual = Json::object();
        actual["title"] = update::title_text(view);
        actual["body"] = update::body_text(view, error, "win32");
        actual["size"] = update::download_size_bytes(view);
        actual["hint"] = update::hint_text(view);
        actual["primary"] = update::primary_label(view);
        actual["later"] = update::later_label(view);
        actual["progress"] = progress;
        actual["phase_text"] = phases;
        CHECK(fv::golden::equal(rebrand_json(c.result<Json>()), actual));
    });
}
