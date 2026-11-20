#include "update_native/pinned_tree.h"
#include "base/json.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "update/release_identity.h"
#include "update/update_contract.h"
#include "update_native/manifest.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace fv::update_native {
namespace fs = std::filesystem;
PinnedTree::PinnedTree(const fs::path& root) : root_(root) {
    directories_.push_back(std::make_unique<GuardedDirectory>(root));
    std::map<fs::path, GuardedDirectory*> parents{{root, directories_.back().get()}};
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (++count > max_members)
            throw UpdateError("The update tree has too many entries.");
        const auto path = entry.path();
        const auto parent = parents.find(path.parent_path());
        if (parent == parents.end())
            throw UpdateError("The update tree changed during verification.");
        const auto status = entry.symlink_status();
        if (fs::is_directory(status)) {
            auto directory = std::make_unique<GuardedDirectory>(path);
            parents.emplace(path, directory.get());
            directories_.push_back(std::move(directory));
            auto name = path_to_utf8(path.lexically_relative(root));
            std::replace(name.begin(), name.end(), '\\', '/');
            observed_directories_.insert(std::move(name));
        } else if (fs::is_regular_file(status)) {
            auto file = parent->second->pin_file(path.filename());
            if (!file)
                throw UpdateError("An update file disappeared during verification.");
            files_.push_back(std::move(file));
            auto name = path_to_utf8(path.lexically_relative(root));
            std::replace(name.begin(), name.end(), '\\', '/');
            if (!observed_.emplace(name, path).second)
                throw UpdateError("The update tree contains duplicate names.");
        } else {
            throw UpdateError("The update tree contains a link or special file.");
        }
    }
}
void PinnedTree::verify_old(std::string_view hash) const {
    if (hash.size() != 64 ||
        download::sha256_file(root_ / path_from_utf8(update::release_identity().app_exe), true) !=
            hash)
        throw UpdateError("The installation executable failed verification.");
}
void PinnedTree::verify_release(std::string_view version, std::string_view hash,
                                const Json& compatibility, bool marker_required) const {
    const auto bytes = directories_.front()->read_file(path_from_utf8(update::manifest_name()),
                                                       max_manifest_bytes);
    const Json manifest = Json::parse(bytes);
    auto expected = validate_manifest(manifest, version);
    if (!compatibility.is_object())
        throw UpdateError("The update compatibility metadata is invalid.");
    for (auto it = compatibility.begin(); it != compatibility.end(); ++it) {
        const auto& record = it.value();
        if (!record.is_object() || !record.contains("size") ||
            !record.at("size").is_number_integer() ||
            (!record.at("size").is_number_unsigned() &&
             record.at("size").get<std::int64_t>() < 0) ||
            !record.contains("sha256") || !record.at("sha256").is_string() ||
            !expected
                 .emplace(it.key(), FileRecord{record.at("size").get<std::uint64_t>(),
                                               record.at("sha256").get<std::string>()})
                 .second)
            throw UpdateError("The update compatibility metadata is invalid.");
    }
    if ((marker_required || observed_.contains(update::sentinel_name())) &&
        directories_.front()->read_file(path_from_utf8(update::sentinel_name()), 256) != version)
        throw UpdateError("The update completion marker is invalid.");
    auto actual = observed_;
    actual.erase(update::manifest_name());
    actual.erase(update::sentinel_name());
    if (actual.size() != expected.size())
        throw UpdateError("The update tree contains unexpected files.");
    std::set<std::string> expected_directories;
    for (const auto& [name, record] : expected) {
        for (auto slash = name.find('/'); slash != std::string::npos;
             slash = name.find('/', slash + 1))
            expected_directories.insert(name.substr(0, slash));
        const auto found = actual.find(name);
        if (found == actual.end() || fs::file_size(found->second) != record.size ||
            download::sha256_file(found->second, true) != record.sha256)
            throw UpdateError("An update file failed verification: " + name);
    }
    if (observed_directories_ != expected_directories)
        throw UpdateError("The update tree contains unexpected directories.");
    verify_old(hash);
}
} // namespace fv::update_native
