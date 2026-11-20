#pragma once

// Helpers shared by the update tests: OpenWhisper_CPP's names in place of OpenWhisper's in recorded
// payloads, results as the golden vectors spell them, and fixed times.

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "golden/golden.h"
#include "pyport/datetime.h"
#include "update/check.h"
#include "update/schedule.h"

namespace fv::update::test {

// rebrand(), then OpenWhisper's native archive suffix as OpenWhisper_CPP's (architecture amendment
// 22).
inline std::string rebrand_text(std::string_view text) {
    auto out = golden::rebrand(text);
    static constexpr std::string_view kFrom = "-win64.tar.xz";
    static constexpr std::string_view kTo = "-win64.zip";
    for (auto at = out.find(kFrom); at != std::string::npos; at = out.find(kFrom, at)) {
        out.replace(at, kFrom.size(), kTo);
        at += kTo.size();
    }
    return out;
}

// Every string (and object key) of a JSON value, rebranded.
inline Json rebrand_json(const Json& value) {
    if (value.is_string()) {
        return rebrand_text(value.get_ref<const std::string&>());
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(rebrand_json(item));
        }
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[rebrand_text(key)] = rebrand_json(item);
        }
        return out;
    }
    return value;
}

inline Json optional_text(const std::optional<std::string>& text) {
    return text ? Json(*text) : Json(nullptr);
}

inline Json asset_json(const std::optional<ReleaseAsset>& asset) {
    if (!asset) {
        return nullptr;
    }
    Json out = Json::object();
    out["url"] = asset->url;
    out["name"] = asset->name;
    out["size_bytes"] = asset->size_bytes;
    out["sha256"] = optional_text(asset->sha256);
    out["github_asset_id"] = asset->github_asset_id ? Json(*asset->github_asset_id) : Json(nullptr);
    return out;
}

inline Json release_json(const ReleaseInfo& release) {
    Json out = Json::object();
    out["version"] = release.version;
    out["tag_name"] = release.tag_name;
    out["html_url"] = release.html_url;
    out["notes"] = release.notes;
    out["prerelease"] = release.prerelease;
    out["setup_asset"] = asset_json(release.setup_asset);
    out["native_asset"] = asset_json(release.native_asset);
    return out;
}

// An ISO time ("2026-08-21T12:00:00+00:00") as a system clock time.
inline std::chrono::system_clock::time_point at(std::string_view iso) {
    return to_sys_time(py::fromisoformat(iso));
}

inline std::string iso(std::chrono::system_clock::time_point time) {
    return utc_datetime(time).isoformat();
}

} // namespace fv::update::test
