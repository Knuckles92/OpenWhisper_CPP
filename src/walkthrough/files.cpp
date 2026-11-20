#include "walkthrough/files.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <system_error>
#include <vector>

#include "base/atomic_file.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "walkthrough/detail/text_io.h"

namespace fv::walkthrough {

namespace {

// json's allow_nan=False, raised at the first out-of-range float in the order dump meets it.
void check_finite(const Json& value) {
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            throw py::ValueError("Out of range float values are not JSON compliant: " +
                                 py::float_repr(number));
        }
    } else if (value.is_structured()) {
        for (const auto& item : value) {
            check_finite(item);
        }
    }
}

bool is_link(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

} // namespace

void atomic_json(const std::filesystem::path& path, const Json& value) {
    check_finite(value);
    py::DumpOptions options;
    options.indent = 2;
    options.ensure_ascii = false;
    write_file_atomic(path, py::dumps(value, options));
}

void publish_files(const std::filesystem::path& stage, const std::filesystem::path& folder,
                   std::span<const std::string> names, std::span<const std::string> remove) {
    const auto previous = stage / ".previous";
    if (!std::filesystem::create_directory(previous)) {
        throw std::filesystem::filesystem_error("Could not create", previous,
                                                std::make_error_code(std::errc::file_exists));
    }
    std::set<std::string> existed;
    std::vector<std::string> all(names.begin(), names.end());
    all.insert(all.end(), remove.begin(), remove.end());
    for (const auto& name : all) {
        const auto target = folder / path_from_utf8(name);
        std::error_code error;
        if (std::filesystem::exists(target, error) || is_link(target)) {
            const auto source = local_file(folder, name);
            std::filesystem::copy_file(source, previous / path_from_utf8(name),
                                       std::filesystem::copy_options::overwrite_existing);
            existed.insert(name);
        }
    }
    std::vector<std::string> published;
    std::vector<std::string> removed;
    try {
        for (const auto& name : remove) {
            if (existed.contains(name)) {
                const auto target = folder / path_from_utf8(name);
                if (!std::filesystem::remove(target)) {
                    throw std::filesystem::filesystem_error(
                        "Could not remove", target,
                        std::make_error_code(std::errc::no_such_file_or_directory));
                }
                removed.push_back(name);
            }
        }
        for (const auto& name : names) {
            std::filesystem::rename(stage / path_from_utf8(name), folder / path_from_utf8(name));
            published.push_back(name);
        }
    } catch (const std::system_error&) {
        std::vector<std::string> done = published;
        done.insert(done.end(), removed.begin(), removed.end());
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const auto target = folder / path_from_utf8(*it);
            if (existed.contains(*it)) {
                std::filesystem::rename(previous / path_from_utf8(*it), target);
            } else {
                std::error_code ignored;
                std::filesystem::remove(target, ignored);
            }
        }
        throw;
    }
}

std::filesystem::path local_file(const std::filesystem::path& folder, std::string_view name) {
    // Path(name).name != name: a separator, "." or (on Windows) a drive such as "c:" or "c:x".
    bool invalid = name.find('/') != std::string_view::npos ||
                   name.find('\\') != std::string_view::npos || name == ".";
#ifdef _WIN32
    invalid = invalid || (name.size() >= 2 && name[1] == ':');
#endif
    if (invalid) {
        throw py::ValueError("Invalid walkthrough filename");
    }
    const auto path = folder / path_from_utf8(name);
    std::error_code error;
    if (is_link(folder) || is_link(path) || !std::filesystem::is_regular_file(path, error)) {
        throw py::ValueError("Missing or unsafe walkthrough file: " + std::string(name));
    }
    return path;
}

Json read_json(const std::filesystem::path& path) {
    if (is_link(path) || std::filesystem::file_size(path) > MAX_METADATA_BYTES) {
        throw py::ValueError("Unsafe or oversized walkthrough metadata");
    }
    Json value = py::loads(detail::read_text(path));
    if (!value.is_object()) {
        throw py::ValueError("Walkthrough metadata must be an object");
    }
    return value;
}

} // namespace fv::walkthrough
