#include "update_native/archive.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <vector>

#include "archive/zip_writer.h"
#include "base/atomic_file.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "update/update_contract.h"
#include "update_native/detail/file_ops.h"
#include "update_native/manifest.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

void validate_entry(const archive::EntryInfo& entry) {
    // High bits encode Unix file type; the DOS reparse attribute is independent of it.
    const auto type = (entry.external_attr >> 16) & 0170000U;
    const auto expected = entry.directory ? 0040000U : 0100000U;
    if ((type != 0 && type != expected) || (entry.external_attr & 0x400U) != 0 ||
        (entry.directory && entry.size != 0) || entry.method == archive::Method::Other) {
        throw UpdateError("Unsupported release archive entry: " + entry.name);
    }
    check_topology(member_name(entry.name, entry.directory), entry.directory);
}

Json read_manifest(archive::ZipReader& zip, const CancelToken& cancel) {
    std::set<std::string> seen;
    bool found = false;
    const auto manifest = update::manifest_name();
    for (const auto& entry : zip.infolist()) {
        validate_entry(entry);
        if (!seen.insert(py::casefold(member_name(entry.name, entry.directory))).second) {
            throw UpdateError("The update archive contains duplicate paths.");
        }
        if (entry.name == manifest) {
            found = true;
            if (entry.directory || entry.size > max_manifest_bytes) {
                throw UpdateError("Invalid update manifest.");
            }
        }
    }
    if (!found) {
        throw UpdateError("The update package is missing its manifest.");
    }
    std::string bytes;
    zip.read_entry(
        manifest,
        [&](std::span<const std::byte> chunk) {
            if (chunk.size() > max_manifest_bytes - bytes.size()) {
                throw UpdateError("The update manifest is too large.");
            }
            bytes.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        },
        cancel);
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 8) {
            throw UpdateError("The update manifest is nested too deeply.");
        }
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key &&
                   !keys.back().insert(parsed.get<std::string>()).second) {
            throw UpdateError("The update manifest repeats a field.");
        }
        return true;
    });
}

Json read_payload(const fs::path& archive, std::string_view expected_version,
                  const CancelToken& cancel, const fs::path* staging) {
    archive::ZipReader zip(archive, {max_members, 960, true});
    auto manifest = read_manifest(zip, cancel);
    auto files = validate_manifest(manifest, expected_version);
    std::set<std::string> allowed_dirs;
    for (const auto& [name, record] : files) {
        for (auto slash = name.find('/'); slash != name.npos; slash = name.find('/', slash + 1)) {
            allowed_dirs.insert(name.substr(0, slash));
        }
    }
    for (const auto& entry : zip.infolist()) {
        if (entry.directory) {
            if (!allowed_dirs.contains(member_name(entry.name, true))) {
                throw UpdateError("The update contains an undeclared directory: " + entry.name);
            }
            continue;
        }
        if (entry.name == update::manifest_name()) {
            continue;
        }
        const auto it = files.find(entry.name);
        if (it == files.end() || it->second.size != entry.size) {
            throw UpdateError("The update manifest does not authorize " + entry.name);
        }
        std::optional<std::ofstream> file;
        if (staging) {
            const auto path = *staging / path_from_utf8(entry.name);
            fs::create_directories(path.parent_path());
            file.emplace();
            file->exceptions(std::ios::badbit | std::ios::failbit);
            file->open(path, std::ios::binary | std::ios::noreplace);
        }
        Sha256 digest;
        std::uint64_t count = 0;
        zip.read_entry(
            entry.name,
            [&](std::span<const std::byte> chunk) {
                if (chunk.size() > entry.size - count) {
                    throw UpdateError("A file exceeds its declared size: " + entry.name);
                }
                count += chunk.size();
                digest.update(chunk);
                if (file) {
                    file->write(reinterpret_cast<const char*>(chunk.data()),
                                static_cast<std::streamsize>(chunk.size()));
                }
            },
            cancel);
        if (count != entry.size || digest.hexdigest() != it->second.sha256) {
            throw UpdateError("An update file failed verification: " + entry.name);
        }
        if (file) {
            file->close();
            detail::flush_file(*staging / path_from_utf8(entry.name));
        }
        files.erase(it);
    }
    if (!files.empty()) {
        throw UpdateError("The update archive is missing " + files.begin()->first);
    }
    return manifest;
}

} // namespace

Json verify_archive(const fs::path& archive, std::string_view expected_version,
                    const CancelToken& cancel) {
    return read_payload(archive, expected_version, cancel, nullptr);
}

Json extract_archive(const fs::path& archive, const fs::path& destination,
                     std::string_view expected_version, const CancelToken& cancel) {
    const auto parent = fs::canonical(fs::absolute(destination).parent_path());
    const auto candidate = parent / destination.filename();
    if (fs::exists(fs::symlink_status(candidate))) {
        throw UpdateError("The update candidate directory already exists.");
    }
    // Only this operation knows this staging name, and only a fully verified tree is published.
    const auto work = parent / (".fiorivoce-candidate-" + uuid4_hex());
    if (!fs::create_directory(work)) {
        throw UpdateError("Could not create the update candidate directory.");
    }
    try {
        auto manifest = read_payload(archive, expected_version, cancel, &work);
        write_file_atomic(work / update::manifest_name(), manifest.dump(2) + "\n");
        if (cancel.cancelled()) {
            throw Canceled("The update was canceled.");
        }
        detail::publish_new(work, candidate);
        return manifest;
    } catch (...) {
        std::error_code error;
        fs::remove_all(work, error);
        throw;
    }
}

void write_manifest(const fs::path& tree, std::string_view version) {
    write_file_atomic(tree / update::manifest_name(), build_manifest(tree, version).dump(2) + "\n");
}

void pack_archive(const fs::path& tree, std::string_view version, const fs::path& output) {
    const auto parent = fs::canonical(fs::absolute(output).parent_path());
    const auto root = fs::canonical(tree);
    for (auto ancestor = parent;; ancestor = ancestor.parent_path()) {
        if (fs::equivalent(root, ancestor)) {
            throw UpdateError("Choose an archive destination outside the staged tree.");
        }
        if (ancestor == ancestor.root_path()) {
            break;
        }
    }
    if (fs::exists(fs::symlink_status(output))) {
        throw UpdateError("The archive destination already exists.");
    }
    const auto manifest = build_manifest(tree, version);
    const auto bytes = manifest.dump(2) + "\n";
    const auto temporary = parent / (".fiorivoce-pack-" + uuid4_hex());
    if (!fs::create_directory(temporary)) {
        throw UpdateError("Could not create a staging directory for the archive.");
    }
    const auto staged = temporary / "release.zip";
    struct Cleanup {
        fs::path file;
        ~Cleanup() {
            std::error_code error;
            fs::remove(file, error);
            fs::remove(file.parent_path(), error);
        }
    } cleanup{staged};
    auto writer = archive::ZipWriter::create(staged);
    constexpr std::uint32_t attrs = 0100644U << 16;
    writer.add_bytes(update::manifest_name(), std::as_bytes(std::span(bytes)),
                     archive::Method::Deflated, attrs);
    for (const auto& [name, meta] : manifest.at("files").items()) {
        writer.add_file(name, tree / path_from_utf8(name), archive::Method::Deflated, attrs, {});
    }
    writer.close();
    verify_archive(staged, version);
    // A concurrent producer must never replace the winner's verified release.
    detail::publish_new(staged, output);
}

} // namespace fv::update_native
