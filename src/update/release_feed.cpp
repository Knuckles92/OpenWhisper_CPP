#include "update/release_feed.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <tuple>

#include "base/identity.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "update/schedule.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update");

using Micros = std::chrono::microseconds;
using SysMicros = std::chrono::sys_time<Micros>;

constexpr std::string_view kUnreadable =
    "The update server returned a response that could not be read.";
constexpr std::string_view kNoVersion = "The update server did not report a release version.";

// Python's truth value of a JSON value.
bool truthy(const Json* value) {
    if (value == nullptr || value->is_null()) {
        return false;
    }
    if (value->is_boolean()) {
        return value->get<bool>();
    }
    if (value->is_number_unsigned()) {
        return value->get<std::uint64_t>() != 0;
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>() != 0;
    }
    if (value->is_number_float()) {
        return value->get<double>() != 0.0;
    }
    if (value->is_string()) {
        return !value->get_ref<const std::string&>().empty();
    }
    return !value->empty();
}

// dict.get(key): nullptr when missing (or when `object` is no dict).
const Json* get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &*found;
}

const std::string* get_string(const Json& object, std::string_view key) {
    const auto* value = get(object, key);
    return value != nullptr && value->is_string() ? &value->get_ref<const std::string&>() : nullptr;
}

// str.rsplit("/", 1)[-1].
std::string_view last_segment(std::string_view url) {
    const auto slash = url.rfind('/');
    return slash == std::string_view::npos ? url : url.substr(slash + 1);
}

// Whether int(text, 16) accepts `text` (already stripped): a sign, a "0x" prefix with one
// underscore after it, digits (any Unicode decimal digit counts, as in Python) with single
// underscores between them.
bool python_hex_int(std::u32string_view text) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == U'+' || text[i] == U'-')) {
        ++i;
    }
    if (i + 1 < text.size() && text[i] == U'0' && (text[i + 1] == U'x' || text[i + 1] == U'X')) {
        i += 2;
        if (i < text.size() && text[i] == U'_') {
            ++i;
        }
    }
    if (i >= text.size() || text[i] == U'_') {
        return false;
    }
    char32_t previous = 0;
    for (; i < text.size(); ++i) {
        const char32_t c = text[i];
        if (c == U'_') {
            if (previous == U'_') {
                return false;
            }
        } else {
            const bool hex_letter = (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
            if (!hex_letter && !py::decimal_value(c)) {
                return false;
            }
        }
        previous = c;
    }
    return previous != U'_';
}

// bytes.decode("utf-8") succeeds: no stray, overlong or truncated sequence, no surrogate, nothing
// past U+10FFFF.
bool valid_utf8(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        std::size_t length = 0;
        char32_t cp = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
        } else {
            return false;
        }
        if (i + length > bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((length == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (length == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        i += length;
    }
    return true;
}

// str.splitlines() of valid UTF-8 text.
std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    bool open = false;
    while (i < text.size()) {
        const auto start = i;
        const char32_t cp = py::next_code_point(text, i);
        const bool breaks = cp == U'\n' || cp == U'\r' || cp == U'\v' || cp == U'\f' ||
                            cp == 0x1C || cp == 0x1D || cp == 0x1E || cp == 0x85 || cp == 0x2028 ||
                            cp == 0x2029;
        if (breaks) {
            if (cp == U'\r' && i < text.size() && text[i] == '\n') {
                ++i;
            }
            lines.push_back(std::move(line));
            line.clear();
            open = false;
        } else {
            line.append(text.substr(start, i - start));
            open = true;
        }
    }
    if (open) {
        lines.push_back(std::move(line));
    }
    return lines;
}

bool is_hex_ascii(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// The range datetime.fromtimestamp(seconds, timezone.utc) accepts on the win32 Python the
// vectors pin (its gmtime_s), measured; anything outside raises OSError there.
constexpr std::int64_t kMinUtcTimestamp = -43200;
constexpr std::int64_t kMaxUtcTimestamp = 32536850399;

// datetime's own range, 0001-01-01 to 9999-12-31T23:59:59.999999 (in Unix microseconds).
constexpr std::int64_t kMinDatetimeMicros = -62135596800LL * 1'000'000;
constexpr std::int64_t kMaxDatetimeMicros = 253402300800LL * 1'000'000 - 1;

std::string fetch_failure_text(const FetchFailure& failure, std::string_view repo) {
    return describe_update_error(failure, repo);
}

// _latest_public_release_tag: the stable tag and release page GitHub's latest redirect names.
std::pair<std::string, std::string>
latest_public_release_tag(http::Client& client, const FeedConfig& feed, const CancelToken& cancel) {
    const auto repo = feed_repo(feed);
    const auto urls = github_urls(repo);
    auto opened =
        open_checked(client, update_request(feed, urls.latest_release_page, "HEAD"), repo, cancel);
    if (!opened) {
        throw AppUpdateError(fetch_failure_text(opened.error(), repo));
    }
    const auto& release_url = opened->effective_url;
    const py::Regex tag_pattern(py::regex_escape(urls.releases_page + "/tag/") +
                                R"((v?\d+\.\d+\.\d+))");
    const auto match = tag_pattern.fullmatch(release_url);
    if (!match) {
        throw AppUpdateError("GitHub did not report a stable release version.");
    }
    return {match->group(1), release_url};
}

// _fetch_release_checksums.
std::map<std::string, std::string> fetch_release_checksums(http::Client& client,
                                                           const FeedConfig& feed,
                                                           std::string_view tag,
                                                           const CancelToken& cancel) {
    const auto repo = feed_repo(feed);
    auto request = update_request(feed, *expected_asset_url(repo, tag, "SHA256SUMS.txt"));
    // Python reads at most one byte past the limit; a longer body is refused all the same.
    request.max_body_bytes = kMaxChecksumBytes + 1;
    auto opened = open_checked(client, std::move(request), repo, cancel);
    if (!opened) {
        throw std::runtime_error(fetch_failure_text(opened.error(), repo));
    }
    return parse_release_checksums(opened->body);
}

// _release_asset_from_checksum: an API-style asset entry, or nullopt when the HEAD fails or
// names no usable size.
std::optional<Json> release_asset_from_checksum(http::Client& client, const FeedConfig& feed,
                                                std::string_view tag, std::string_view name,
                                                std::string_view digest,
                                                const CancelToken& cancel) {
    const auto repo = feed_repo(feed);
    const auto url = *expected_asset_url(repo, tag, name);
    auto opened = open_checked(client, update_request(feed, url, "HEAD"), repo, cancel);
    if (!opened) {
        kLog.info("Public release asset metadata unavailable: {}",
                  fetch_failure_text(opened.error(), repo));
        return std::nullopt;
    }
    const auto header = http::find_header(opened->headers, "Content-Length");
    const auto text = header && !header->empty() ? *header : std::string("0");
    const auto size = py::py_int(text);
    if (!size) {
        kLog.info("Public release asset metadata unavailable: invalid literal for int() with "
                  "base 10: {}",
                  py::repr(text));
        return std::nullopt;
    }
    if (opened->status != 200 || *size <= 0 || *size > kMaxAssetBytes) {
        return std::nullopt;
    }
    Json asset = Json::object();
    asset["name"] = std::string(name);
    asset["size"] = *size;
    asset["digest"] = "sha256:" + std::string(digest);
    asset["browser_download_url"] = url;
    return asset;
}

// _public_release_assets: entries for the checksummed assets this app can install. A missing
// or unreadable checksum file leaves the release notify-only.
Json public_release_assets(http::Client& client, const FeedConfig& feed, std::string_view tag,
                           std::string_view version, const CancelToken& cancel) {
    Json assets = Json::array();
    try {
        const auto hashes = fetch_release_checksums(client, feed, tag, cancel);
        for (const auto& name : {setup_asset_name(version), archive_asset_name(version)}) {
            const auto found = hashes.find(name);
            if (found == hashes.end()) {
                continue;
            }
            if (auto asset =
                    release_asset_from_checksum(client, feed, tag, name, found->second, cancel)) {
                assets.push_back(std::move(*asset));
            }
        }
    } catch (const std::exception& error) {
        kLog.info("Public release checksums unavailable: {}", error.what());
    }
    return assets;
}

// _fetch_latest_release_from_web: GitHub's API-free endpoints; missing hashes permit notifying
// only.
ReleaseInfo fetch_latest_release_from_web(http::Client& client, const FeedConfig& feed,
                                          const CancelToken& cancel) {
    const auto [tag, release_url] = latest_public_release_tag(client, feed, cancel);
    const auto version = normalize_version(tag);
    Json payload = Json::object();
    payload["tag_name"] = tag;
    payload["html_url"] = release_url;
    payload["body"] = "[Read the release notes on GitHub](" + release_url + ").";
    payload["assets"] = public_release_assets(client, feed, tag, version, cancel);
    return parse_release_payload(feed, payload);
}

} // namespace

std::string update_feed_env() {
    return std::string(identity().env_prefix) + "UPDATE_FEED_URL";
}

FeedConfig feed_config(const EnvReader& env) {
    return feed_config(env, release_identity().source);
}

FeedConfig feed_config(const EnvReader& env, UpdateSource source) {
    FeedConfig config;
    config.source = std::move(source);
    const auto name = update_feed_env();
    const auto value = py::strip(env.get(name).value_or(""));
    if (value.empty()) {
        return config;
    }
    if (is_loopback_feed_url(value)) {
        config.override_url = value;
    } else {
        kLog.warning("Ignoring unsafe {} value", name);
    }
    return config;
}

GitHubUrls github_urls(std::string_view repo) {
    const std::string name(repo);
    GitHubUrls urls;
    urls.latest_release = "https://api.github.com/repos/" + name + "/releases/latest";
    urls.release_list = "https://api.github.com/repos/" + name + "/releases?per_page=30";
    urls.releases_page = "https://github.com/" + name + "/releases";
    urls.latest_release_page = urls.releases_page + "/latest";
    return urls;
}

std::string_view feed_repo(const FeedConfig& feed) {
    return feed.source.kind == UpdateSource::Kind::GitHub ? std::string_view(feed.source.repo)
                                                          : std::string_view();
}

bool has_update_source(const FeedConfig& feed) {
    return feed.override_url.has_value() || !feed_repo(feed).empty();
}

std::string no_update_source_message() {
    return "This build of " + std::string(identity().product) + " has no update source.";
}

std::string latest_release_url(const FeedConfig& feed, ReleaseTrack track) {
    if (feed.override_url) {
        kLog.warning("Update feed overridden by {}: {}", update_feed_env(), *feed.override_url);
        return *feed.override_url;
    }
    const auto repo = feed_repo(feed);
    if (repo.empty()) {
        throw AppUpdateError(no_update_source_message());
    }
    const auto urls = github_urls(repo);
    return track == ReleaseTrack::Beta ? urls.release_list : urls.latest_release;
}

std::optional<std::string> expected_asset_url(std::string_view repo, std::string_view tag_name,
                                              std::string_view filename) {
    if (repo.empty()) {
        return std::nullopt;
    }
    return "https://github.com/" + std::string(repo) + "/releases/download/" +
           std::string(tag_name) + "/" + std::string(filename);
}

bool asset_url_allowed(const FeedConfig& feed, std::string_view url, std::string_view tag_name,
                       std::string_view expected_name) {
    if (const auto expected = expected_asset_url(feed_repo(feed), tag_name, expected_name);
        expected && url == *expected) {
        return true;
    }
    if (!feed.override_url) {
        return false;
    }
    // The last segment of the expected URL, which ends ".../<tag>/<name>".
    const auto expected_tail = "/" + std::string(tag_name) + "/" + std::string(expected_name);
    const bool same_file = last_segment(url) == last_segment(expected_tail);
    return same_file && url_origin(url) == url_origin(*feed.override_url);
}

std::optional<std::string> digest_to_sha256(const Json& raw) {
    if (!raw.is_string()) {
        return std::nullopt;
    }
    const auto digest = py::strip(raw.get_ref<const std::string&>());
    static constexpr std::string_view kPrefix = "sha256:";
    if (!py::lower(digest).starts_with(kPrefix)) {
        return std::nullopt;
    }
    // The prefix matched in lower case, so it is seven ASCII characters of `digest` too.
    const auto hex_digest = py::lower(py::strip(std::string_view(digest).substr(kPrefix.size())));
    const auto code_points = py::to_code_points(hex_digest);
    if (code_points.size() != 64 || !python_hex_int(code_points)) {
        return std::nullopt;
    }
    return hex_digest;
}

std::optional<ReleaseAsset> parse_named_asset(const FeedConfig& feed, const Json& assets,
                                              std::string_view expected_name,
                                              std::string_view tag_name) {
    if (!assets.is_array()) {
        return std::nullopt;
    }
    const auto expected_url = expected_asset_url(feed_repo(feed), tag_name, expected_name);
    std::vector<ReleaseAsset> matches;
    for (const auto& item : assets) {
        if (!item.is_object()) {
            continue;
        }
        const auto* name = get_string(item, "name");
        if (name == nullptr || *name != expected_name) {
            continue;
        }
        const auto* state = get(item, "state");
        if (state != nullptr && !state->is_null() &&
            !(state->is_string() && state->get_ref<const std::string&>() == "uploaded")) {
            continue;
        }
        const auto* url = get_string(item, "browser_download_url");
        if (url == nullptr || !asset_url_allowed(feed, *url, tag_name, expected_name)) {
            continue;
        }
        const auto* size_value = get(item, "size");
        const std::int64_t size_bytes =
            truthy(size_value) ? py::py_int(*size_value).value_or(0) : 0;
        if (size_bytes <= 0 || size_bytes > kMaxAssetBytes) {
            continue;
        }
        ReleaseAsset asset;
        asset.url = *url;
        asset.name = *name;
        asset.size_bytes = size_bytes;
        const auto* digest = get(item, "digest");
        asset.sha256 = digest != nullptr ? digest_to_sha256(*digest) : std::nullopt;
        const auto* id = get(item, "id");
        if (expected_url && *url == *expected_url && id != nullptr && id->is_number_integer() &&
            !id->is_boolean()) {
            if (id->is_number_unsigned()) {
                const auto value = id->get<std::uint64_t>();
                if (value > 0 && value <= static_cast<std::uint64_t>(INT64_MAX)) {
                    asset.github_asset_id = static_cast<std::int64_t>(value);
                }
            } else if (const auto value = id->get<std::int64_t>(); value > 0) {
                asset.github_asset_id = value;
            }
        }
        matches.push_back(std::move(asset));
    }
    if (matches.size() != 1) {
        return std::nullopt;
    }
    return matches.front();
}

ReleaseInfo parse_release_payload(const FeedConfig& feed, const Json& payload,
                                  bool allow_prerelease) {
    const auto* tag_name = get_string(payload, "tag_name");
    if (tag_name == nullptr || normalize_version(*tag_name).empty()) {
        throw AppUpdateError(std::string(kNoVersion));
    }
    ReleaseInfo release;
    release.version = normalize_version(*tag_name);
    release.tag_name = *tag_name;
    if (const auto* html_url = get_string(payload, "html_url");
        html_url != nullptr && !html_url->empty()) {
        release.html_url = *html_url;
    } else if (const auto repo = feed_repo(feed); !repo.empty()) {
        release.html_url = "https://github.com/" + std::string(repo) + "/releases/tag/" + *tag_name;
    }
    const auto* body = get_string(payload, "body");
    release.notes = body != nullptr ? py::strip(*body) : std::string();
    const auto* assets_value = get(payload, "assets");
    const Json assets = truthy(assets_value) ? *assets_value : Json::array();
    release.prerelease = truthy(get(payload, "prerelease"));
    const bool installable =
        !truthy(get(payload, "draft")) && (allow_prerelease || !release.prerelease);
    if (installable) {
        release.setup_asset =
            parse_named_asset(feed, assets, setup_asset_name(release.version), *tag_name);
        release.native_asset =
            parse_named_asset(feed, assets, archive_asset_name(release.version), *tag_name);
    }
    return release;
}

ReleaseInfo select_newest_release(const FeedConfig& feed, const Json& payload) {
    if (!payload.is_array()) {
        throw AppUpdateError(std::string(kUnreadable));
    }
    static const py::Regex kReleaseTag(R"([vV]?\d+\.\d+\.\d+)");
    const Json* best = nullptr;
    std::tuple<std::array<long long, 3>, bool> best_key;
    for (const auto& item : payload) {
        if (!item.is_object() || truthy(get(item, "draft"))) {
            continue;
        }
        const auto* tag_name = get_string(item, "tag_name");
        if (tag_name == nullptr || !kReleaseTag.fullmatch(py::strip(*tag_name))) {
            continue;
        }
        const auto key =
            std::make_tuple(parse_version(*tag_name), !truthy(get(item, "prerelease")));
        if (best == nullptr || key > best_key) {
            best = &item;
            best_key = key;
        }
    }
    if (best == nullptr) {
        throw AppUpdateError(std::string(kNoVersion));
    }
    return parse_release_payload(feed, *best, true);
}

std::map<std::string, std::string> parse_release_checksums(std::string_view raw) {
    if (raw.size() > kMaxChecksumBytes) {
        throw std::invalid_argument("Release checksums exceed the size limit");
    }
    if (!valid_utf8(raw)) {
        throw std::invalid_argument("Release checksums are not UTF-8 text");
    }
    std::map<std::string, std::string> hashes;
    for (const auto& line : splitlines(raw)) {
        // ([a-fA-F0-9]{64}) [ *]([A-Za-z0-9_.-]+), whole line.
        const bool shaped = line.size() >= 67 &&
                            std::all_of(line.begin(), line.begin() + 64, is_hex_ascii) &&
                            line[64] == ' ' && (line[65] == ' ' || line[65] == '*') &&
                            std::all_of(line.begin() + 66, line.end(), is_name_char);
        if (!shaped) {
            throw std::invalid_argument("Release checksums are invalid or duplicated");
        }
        auto name = line.substr(66);
        if (hashes.contains(name)) {
            throw std::invalid_argument("Release checksums are invalid or duplicated");
        }
        hashes.emplace(std::move(name), lower_ascii(std::string_view(line).substr(0, 64)));
    }
    return hashes;
}

bool api_retry_pending(const SettingsStore& settings, const Clock& clock) {
    try {
        const auto snapshot = settings.snapshot();
        const auto* raw = get(*snapshot, keys::update_api_retry_at.key());
        const auto retry_at = raw != nullptr ? parse_last_check_at(*raw) : std::nullopt;
        return retry_at && to_sys_time(*retry_at) > std::chrono::floor<Micros>(clock.utc());
    } catch (const std::exception& error) {
        kLog.debug("Could not load GitHub API cooldown: {}", error.what());
        return false;
    }
}

void remember_api_rate_limit(const FetchFailure& failure, SettingsStore& settings,
                             const Clock& clock) {
    const auto now = std::chrono::floor<Micros>(clock.utc());
    auto retry_at = now + std::chrono::seconds{60};
    // Honor both GitHub's primary reset and its secondary Retry-After; a bad header must not
    // keep the public-release fallback from running.
    for (const auto& [name, relative] :
         {std::pair<std::string_view, bool>{"X-RateLimit-Reset", false},
          std::pair<std::string_view, bool>{"Retry-After", true}}) {
        const auto header = http::find_header(failure.headers, name);
        const auto seconds = py::py_int(header.value_or(""));
        if (!seconds) {
            continue;
        }
        std::optional<SysMicros> candidate;
        if (relative) {
            // timedelta(seconds=...) and the sum must stay within datetime's years 1 to 9999.
            constexpr std::int64_t kTenThousandYears = 315'537'897'600;
            if (*seconds > -kTenThousandYears && *seconds < kTenThousandYears) {
                const auto micros = now.time_since_epoch().count() + *seconds * 1'000'000;
                if (micros >= kMinDatetimeMicros && micros <= kMaxDatetimeMicros) {
                    candidate = SysMicros{Micros{micros}};
                }
            }
        } else if (*seconds >= kMinUtcTimestamp && *seconds <= kMaxUtcTimestamp) {
            candidate = SysMicros{std::chrono::seconds{*seconds}};
        }
        if (candidate) {
            retry_at = std::max(retry_at, *candidate);
        }
    }
    save_check_timestamp(settings, keys::update_api_retry_at.key(), retry_at);
}

ReleaseInfo fetch_latest_release(http::Client& client, const FeedConfig& feed,
                                 SettingsStore& settings, Clock& clock, const CancelToken& cancel,
                                 std::optional<ReleaseTrack> track) {
    const auto chosen = track.value_or(release_track(settings));
    const auto url = latest_release_url(feed, chosen);
    const auto repo = feed_repo(feed);
    const auto urls = github_urls(repo);
    const bool github_api =
        !feed.override_url && (url == urls.latest_release || url == urls.release_list);
    if (github_api && api_retry_pending(settings, clock)) {
        return fetch_latest_release_from_web(client, feed, cancel);
    }
    auto request = update_request(feed, url);
    request.headers.emplace_back("Accept", "application/vnd.github+json");
    auto opened = open_checked(client, std::move(request), repo, cancel);
    if (!opened) {
        if (github_api && is_rate_limited(opened.error())) {
            remember_api_rate_limit(opened.error(), settings, clock);
            kLog.info("GitHub API rate-limited; checking the public release instead");
            return fetch_latest_release_from_web(client, feed, cancel);
        }
        throw AppUpdateError(describe_update_error(opened.error(), repo));
    }
    if (!valid_utf8(opened->body)) {
        throw AppUpdateError(std::string(kUnreadable));
    }
    Json payload;
    try {
        payload = py::loads(opened->body);
    } catch (const py::ValueError&) {
        throw AppUpdateError(std::string(kUnreadable));
    }
    if (url == urls.release_list && github_api) {
        return select_newest_release(feed, payload);
    }
    if (!payload.is_object()) {
        throw AppUpdateError(std::string(kUnreadable));
    }
    // The loopback soak feed serves one release; on the beta track its prerelease assets are
    // installable, as they would be from GitHub.
    return parse_release_payload(feed, payload, chosen == ReleaseTrack::Beta);
}

} // namespace fv::update
