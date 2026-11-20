#include "update_native/manifest.h"

#include "update_native/detail/file_ops.h"

#include <algorithm>
#include <array>
#include <set>
#include <vector>

#include "base/identity.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "update/release_identity.h"
#include "update/update_contract.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

bool reserved(std::string_view part) {
    const auto stem = py::casefold(part.substr(0, part.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" ||
        stem == "conout$") {
        return true;
    }
    if (!stem.starts_with("com") && !stem.starts_with("lpt")) {
        return false;
    }
    const auto digit = std::string_view(stem).substr(3);
    return (digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') || digit == "¹" ||
           digit == "²" || digit == "³";
}

std::uint64_t unsigned_value(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    }
    throw UpdateError("The update manifest has invalid size metadata.");
}

void required_files(const FileTable& files) {
    for (std::string_view name :
         {std::string_view(update::release_identity().app_exe), updater_name}) {
        if (!files.contains(std::string(name))) {
            throw UpdateError("The update package is missing " + std::string(name) + ".");
        }
    }
}

FileTable read_tree(const fs::path& root) {
    if (detail::unsafe_node(root) || !fs::is_directory(root)) {
        throw UpdateError("The update tree must be an ordinary directory.");
    }
    FileTable result;
    std::uint64_t total = 0;
    std::set<std::string> seen;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto& path = entry.path();
        if (detail::unsafe_node(path)) {
            throw UpdateError("The update tree contains a link: " + path_to_utf8(path));
        }
        const auto bytes = path.lexically_relative(root).generic_u8string();
        const std::string name(bytes.begin(), bytes.end());
        member_name(name);
        if (!seen.insert(py::casefold(name)).second || seen.size() > max_members) {
            throw UpdateError("The update tree has duplicate paths or too many entries.");
        }
        const bool directory = entry.is_directory();
        check_topology(name, directory);
        if (directory) {
            continue;
        }
        if (!entry.is_regular_file()) {
            throw UpdateError("The update tree contains a non-regular file: " + name);
        }
        if (name == update::manifest_name() || name == update::sentinel_name()) {
            continue;
        }
        const auto size = entry.file_size();
        if (size > max_unpacked_bytes - total) {
            throw UpdateError("The update tree is larger than allowed.");
        }
        total += size;
        result.emplace(name, FileRecord{size, download::sha256_file(path, true)});
    }
    required_files(result);
    return result;
}

} // namespace

std::string member_name(std::string_view raw, bool directory) {
    if (directory && raw.ends_with('/')) {
        raw.remove_suffix(1);
    }
    if (raw.empty() || raw.size() > 960 || py::to_code_points(raw).size() > 240 ||
        std::ranges::count(raw, '/') >= 20 ||
        raw.find_first_of("\\:\"<>|?*") != std::string_view::npos ||
        std::ranges::any_of(raw, [](unsigned char ch) { return ch < 32; })) {
        throw UpdateError("Invalid path in the update package: " + std::string(raw));
    }
    std::size_t begin = 0;
    while (true) {
        auto end = raw.find('/', begin);
        const auto part = raw.substr(begin, end == raw.npos ? raw.npos : end - begin);
        if (part.empty() || part.ends_with('.') || part.ends_with(' ') || reserved(part)) {
            throw UpdateError("Invalid path in the update package: " + std::string(raw));
        }
        if (end == raw.npos) {
            break;
        }
        begin = end + 1;
    }
    return std::string(raw);
}

void check_topology(std::string_view name, bool directory) {
    static constexpr std::array<std::string_view, 9> roots{
        "platforms",          "styles",  "imageformats", "iconengines", "tls",
        "networkinformation", "generic", "licenses",     "webui"};
    const auto slash = name.find('/');
    if (directory || slash != name.npos) {
        if (std::ranges::find(roots, name.substr(0, slash)) != roots.end()) {
            return;
        }
    } else {
        const auto& release = update::release_identity();
        if (name == release.app_exe || name == release.cli_exe || name == updater_name ||
            name == "THIRD_PARTY_NOTICES.md" || name == update::manifest_name() ||
            name == update::sentinel_name() || py::lower(name).ends_with(".dll")) {
            return;
        }
    }
    throw RequiresSetup("The update contains an unmanaged path: " + std::string(name));
}

FileTable validate_manifest(const Json& doc, std::string_view expected_version,
                            std::string_view updater_version) {
    if (!doc.is_object() || doc.value("schema_version", Json{}) != update::kManifestSchemaVersion) {
        throw RequiresSetup("This update uses an unsupported manifest schema.");
    }
    if (doc.value("product", std::string{}) != identity().product ||
        doc.value("app_id", Json{}) != update::release_identity().app_id ||
        doc.value("architecture", std::string{}) != update::kArchitecture) {
        throw UpdateError("The update package is for another product or architecture.");
    }
    if (doc.value("topology_revision", Json{}) != update::kTopologyRevision) {
        throw RequiresSetup("This update changes the install layout and must use the installer.");
    }
    const auto version = doc.value("version", std::string{});
    update::parse_strict_version(version);
    if (!expected_version.empty() && version != expected_version) {
        throw UpdateError("The update package version does not match the release.");
    }
    const auto minimum =
        doc.value("minimum_updater_version", std::string(update::kMinimumUpdaterVersion));
    const auto floor = update::parse_strict_version(minimum);
    if (!updater_version.empty() && floor > update::parse_strict_version(updater_version)) {
        throw RequiresSetup("This update needs a newer updater and must use the installer.");
    }
    if (!doc.contains("files") || !doc.at("files").is_object() || doc.at("files").empty()) {
        throw UpdateError("The update manifest lists no files.");
    }
    const auto& entries = doc.at("files");
    const auto count = unsigned_value(doc.value("member_count", Json{}));
    const auto total = unsigned_value(doc.value("unpacked_bytes", Json{}));
    if (count > max_members || count != entries.size() + 1 || total > max_unpacked_bytes) {
        throw UpdateError("The update manifest has invalid size metadata.");
    }
    FileTable result;
    std::set<std::string> file_names;
    std::map<std::string, std::string> directories;
    std::uint64_t size_sum = 0;
    for (const auto& [name, record] : entries.items()) {
        member_name(name);
        check_topology(name, false);
        const auto key = py::casefold(name);
        if (!file_names.insert(key).second || directories.contains(key) ||
            key == py::casefold(update::manifest_name()) ||
            key == py::casefold(update::sentinel_name())) {
            throw UpdateError("The update manifest contains a conflicting path: " + name);
        }
        for (auto slash = name.find('/'); slash != name.npos; slash = name.find('/', slash + 1)) {
            const auto parent = name.substr(0, slash);
            const auto folded = py::casefold(parent);
            const auto [where, inserted] = directories.emplace(folded, parent);
            if (file_names.contains(folded) || (!inserted && where->second != parent)) {
                throw UpdateError("The update manifest contains a conflicting directory: " +
                                  parent);
            }
        }
        if (!record.is_object()) {
            throw UpdateError("The update manifest has an invalid file entry.");
        }
        const auto size = unsigned_value(record.value("size", Json{}));
        const auto hash = record.value("sha256", std::string{});
        if (size > total - size_sum || hash.size() != 64 ||
            hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
            throw UpdateError("The update manifest has an invalid file entry.");
        }
        size_sum += size;
        result.emplace(name, FileRecord{size, hash});
    }
    if (size_sum != total) {
        throw UpdateError("The update manifest size does not match its files.");
    }
    required_files(result);
    return result;
}

Json build_manifest(const fs::path& tree, std::string_view version) {
    const auto files = read_tree(tree);
    Json records = Json::object();
    std::uint64_t total = 0;
    for (const auto& [name, file] : files) {
        total += file.size;
        records[name] = {{"size", file.size}, {"sha256", file.sha256}};
    }
    Json doc{{"schema_version", update::kManifestSchemaVersion},
             {"product", identity().product},
             {"app_id", update::release_identity().app_id},
             {"version", version},
             {"architecture", update::kArchitecture},
             {"topology_revision", update::kTopologyRevision},
             {"minimum_updater_version", update::kMinimumUpdaterVersion},
             {"unpacked_bytes", total},
             {"member_count", files.size() + 1},
             {"files", std::move(records)}};
    validate_manifest(doc);
    return doc;
}

void verify_tree(const fs::path& tree, const Json& manifest) {
    const auto expected = validate_manifest(manifest);
    const auto actual = read_tree(tree);
    if (actual.size() != expected.size()) {
        throw UpdateError("The update tree does not match its manifest.");
    }
    for (const auto& [name, record] : expected) {
        const auto it = actual.find(name);
        if (it == actual.end() || it->second.size != record.size ||
            it->second.sha256 != record.sha256) {
            throw UpdateError("A staged file failed verification: " + name);
        }
    }
}

} // namespace fv::update_native
