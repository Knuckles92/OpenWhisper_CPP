#include "models/catalog.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "base/identity.h"
#include "base/platform_info.h"
#include "models/data/speech_catalog.json.h"
#include "models/runtime_resolver.h"
#include "pyport/json.h"

namespace fv::models {

namespace {

// An all-zero digest marks an archive that is not published yet.
constexpr std::string_view kPlaceholderSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

std::string lower_stripped_ascii(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Python's int(x or 0) for the numbers a catalog holds: null, false, 0 and "" are 0.
std::int64_t int_or_zero(const Json* value) {
    if (value == nullptr || value->is_null()) {
        return 0;
    }
    if (value->is_boolean()) {
        return value->get<bool>() ? 1 : 0;
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>();
    }
    if (value->is_number_float()) {
        return static_cast<std::int64_t>(value->get<double>());
    }
    if (value->is_string()) {
        const auto& text = value->get_ref<const std::string&>();
        if (text.empty()) {
            return 0;
        }
        try {
            std::size_t used = 0;
            const auto parsed = std::stoll(text, &used);
            if (used == text.size()) {
                return parsed;
            }
        } catch (const std::exception&) {
        }
    }
    // int() of anything else raises in Python; the entry is then not usable either.
    return 0;
}

const Json* member(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Python's truthiness of a JSON value.
bool truthy(const Json* value) {
    if (value == nullptr || value->is_null()) {
        return false;
    }
    if (value->is_boolean()) {
        return value->get<bool>();
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>() != 0;
    }
    if (value->is_number_unsigned()) {
        return value->get<std::uint64_t>() != 0;
    }
    if (value->is_number_float()) {
        return value->get<double>() != 0.0;
    }
    if (value->is_string()) {
        return !value->get_ref<const std::string&>().empty();
    }
    if (value->is_array() || value->is_object()) {
        return !value->empty();
    }
    return true;
}

// str() of a JSON value as an f-string writes it: a string as itself, else Python's repr-like
// JSON text.
std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_null()) {
        return "None";
    }
    return py::dumps(value);
}

std::optional<std::vector<int>> parse_version(std::string_view text) {
    std::vector<int> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find('.', start);
        const auto piece = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (piece.empty() ||
            !std::ranges::all_of(piece, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        parts.push_back(std::stoi(std::string(piece)));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

// _below_macos_minimum: the macos_min a runtime needs when this Mac is older, else nullopt.
std::optional<std::string> below_macos_minimum(const Json& manifest, const Host& host) {
    const Json* minimum = member(manifest, "macos_min");
    if (minimum == nullptr || !minimum->is_string() ||
        minimum->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    const auto required = parse_version(minimum->get_ref<const std::string&>());
    if (!required || !host.macos_version || *host.macos_version >= *required) {
        return std::nullopt;
    }
    return minimum->get<std::string>();
}

std::vector<std::string> available_for(std::string_view tag, const Host& host, bool this_host) {
    std::vector<std::string_view> candidates;
    if (tag == platform::kWinAmd64) {
        candidates = {component_id::kAsrNvidiaCpu, component_id::kAsrNvidiaCuda,
                      component_id::kAsrNvidiaVulkan, component_id::kAsrNvidiaMetal};
    } else if (tag == platform::kLinuxX86_64) {
        candidates = {component_id::kAsrNvidiaCpu, component_id::kAsrNvidiaCuda};
        if (host.installed(component_id::kAsrNvidiaVulkan) ||
            nvidia_gpu_runtime(host) == component_id::kAsrNvidiaVulkan) {
            candidates.push_back(component_id::kAsrNvidiaVulkan);
        }
    } else if (tag == platform::kDarwinArm64) {
        candidates = {component_id::kAsrNvidiaCpu, component_id::kAsrNvidiaMetal};
    } else if (tag == platform::kDarwinX86_64) {
        candidates = {component_id::kAsrNvidiaCpu};
    } else {
        return {};
    }
    std::vector<std::string> ids;
    for (const std::string_view id : candidates) {
        if (!component_is_published(id, tag)) {
            continue;
        }
        if (this_host) {
            // A runtime built for a newer macOS than this one cannot load here.
            const auto entry = catalog_entry(id, tag);
            if (below_macos_minimum(entry.value_or(Json::object()), host)) {
                continue;
            }
        }
        ids.emplace_back(id);
    }
    return ids;
}

} // namespace

std::string_view state_name(ComponentState state) {
    switch (state) {
    case ComponentState::NotInstalled:
        return "not_installed";
    case ComponentState::External:
        return "external";
    case ComponentState::Installed:
        return "installed";
    case ComponentState::UpdateAvailable:
        return "update_available";
    case ComponentState::Incompatible:
        return "incompatible";
    case ComponentState::Broken:
        return "broken";
    }
    return "not_installed";
}

Host Host::current(std::function<bool(std::string_view)> is_installed) {
    Host host;
    host.platform = platform_tag();
    host.is_installed = std::move(is_installed);
    host.gpu = [] {
        return nvidia_gpu();
    };
    return host;
}

std::optional<std::string> platform_tag(std::string_view sys_platform, std::string_view machine) {
    const std::string arch = lower_stripped_ascii(machine);
    const auto is = [&arch](std::initializer_list<std::string_view> names) {
        return std::ranges::find(names, std::string_view(arch)) != names.end();
    };
    if (sys_platform == "darwin" && is({"arm64", "aarch64"})) {
        return std::string(platform::kDarwinArm64);
    }
    if (sys_platform == "darwin" && is({"x86_64", "amd64"})) {
        return std::string(platform::kDarwinX86_64);
    }
    if (sys_platform.starts_with("win")) {
        if (is({"amd64", "x86_64", "x64"})) {
            return std::string(platform::kWinAmd64);
        }
        return std::nullopt;
    }
    if (sys_platform.starts_with("linux")) {
        if (is({"amd64", "x86_64", "x64"})) {
            return std::string(platform::kLinuxX86_64);
        }
        if (is({"aarch64", "arm64"})) {
            return std::string(platform::kLinuxAarch64);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> platform_tag() {
    static const std::optional<std::string> tag = platform_tag(python_platform(), host_machine());
    return tag;
}

const Json& builtin_catalog() {
    static const Json catalog = [] {
        const Json data = Json::parse(embedded::models_speech_catalog);
        Json built = Json::object();
        built["schema"] = 2;
        built["components"] = data.at("components");
        return built;
    }();
    return catalog;
}

std::optional<Json> catalog_entry(std::string_view component_id, std::string_view tag,
                                  const Json& catalog) {
    const Json* components = member(catalog, "components");
    const Json* root = components == nullptr ? nullptr : member(*components, component_id);
    if (root == nullptr || !root->is_object()) {
        return std::nullopt;
    }
    const Json* platforms = member(*root, "platforms");
    if (platforms == nullptr || !platforms->is_object() || tag.empty()) {
        return std::nullopt;
    }
    const Json* entry = member(*platforms, tag);
    if (entry == nullptr || !entry->is_object()) {
        return std::nullopt;
    }
    return std::optional<Json>(std::in_place, *entry);
}

std::optional<Json> catalog_entry(std::string_view component_id, const Host& host,
                                  const Json& catalog) {
    return catalog_entry(component_id, host.platform.value_or(std::string()), catalog);
}

bool component_is_published(const Json& entry) {
    if (!entry.is_object()) {
        return false;
    }
    if (const Json* published = member(entry, "published");
        published != nullptr && published->is_boolean() && !published->get<bool>()) {
        return false;
    }
    const Json* archives = member(entry, "archives");
    if (!truthy(archives) || int_or_zero(member(entry, "install_bytes")) <= 0) {
        return false;
    }
    if (!archives->is_array()) {
        return false;
    }
    for (const Json& archive : *archives) {
        const Json* sha = member(archive, "sha256");
        std::string digest = truthy(sha) ? py_str(*sha) : std::string();
        std::ranges::transform(digest, digest.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const Json* url = member(archive, "url");
        const std::string link = truthy(url) ? py_str(*url) : std::string();
        if (digest.size() != 64 || digest == kPlaceholderSha256 || !link.starts_with("https://") ||
            int_or_zero(member(archive, "size_bytes")) <= 0) {
            return false;
        }
    }
    return true;
}

bool component_is_published(std::string_view component_id, std::string_view tag,
                            const Json& catalog) {
    const auto entry = catalog_entry(component_id, tag, catalog);
    return entry && component_is_published(*entry);
}

std::vector<std::string> available_component_ids(const Host& host) {
    if (!host.platform) {
        return {};
    }
    return available_for(*host.platform, host, true);
}

std::vector<std::string> available_component_ids(std::string_view tag, const Host& host) {
    return available_for(tag, host, false);
}

std::optional<std::string> check_compatibility(const Json& manifest, const Host& host) {
    if (const Json* api = member(manifest, "component_api");
        api != nullptr && !api->is_null() && !py::py_equal(*api, Json(kComponentApi))) {
        return std::format(
            "Built for a different version of {} (component API {}, this app uses {})",
            identity().product, py_str(*api), kComponentApi);
    }
    if (const Json* tag = member(manifest, "platform"); truthy(tag)) {
        if (!host.platform || !tag->is_string() ||
            tag->get_ref<const std::string&>() != *host.platform) {
            return std::format("Built for {}, which this app cannot use", py_str(*tag));
        }
    }
    if (const auto minimum = below_macos_minimum(manifest, host)) {
        return std::format("Requires macOS {} or later", *minimum);
    }
    return std::nullopt;
}

} // namespace fv::models
