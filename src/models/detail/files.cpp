#include "models/detail/files.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "base/hash.h"
#include "base/utf8.h"
#include "models/catalog.h"
#include "models/detail/fs_native.h"
#include "pyport/json.h"

namespace fv::models::detail {

namespace fs = std::filesystem;

void remove_tree(const fs::path& path) {
    std::error_code error;
    if (path.empty() || !fs::is_directory(path, error)) {
        return;
    }
    // Depth first; an entry that cannot be removed is left and the rest still go.
    std::vector<fs::path> children;
    for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        children.push_back(it->path());
    }
    for (const auto& child : children) {
        std::error_code status_error;
        // A junction or symbolic link to a directory is removed itself, never entered.
        if (fs::symlink_status(child, status_error).type() == fs::file_type::directory) {
            remove_tree(child);
        } else {
            std::error_code removed;
            fs::remove(child, removed);
        }
    }
    std::error_code removed;
    fs::remove(path, removed);
}

void write_text(const fs::path& path, std::string_view text) {
    const std::string bytes = text_mode(text);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw fs::filesystem_error("could not open the file for writing", path,
                                   std::make_error_code(std::errc::permission_denied));
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw fs::filesystem_error("could not write the file", path,
                                   std::make_error_code(std::errc::io_error));
    }
}

std::optional<std::string> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> url_hostname(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view netloc = url.substr(scheme + 3);
    netloc = netloc.substr(0, netloc.find_first_of("/?#"));
    if (const auto at = netloc.rfind('@'); at != std::string_view::npos) {
        netloc.remove_prefix(at + 1);
    }
    std::string_view host;
    if (netloc.starts_with('[')) {
        const auto close = netloc.find(']');
        host =
            netloc.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
    } else {
        host = netloc.substr(0, netloc.find(':'));
    }
    if (host.empty()) {
        return std::nullopt;
    }
    std::string lowered(host);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

std::int64_t to_int(const Json& value) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1 : 0;
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        return static_cast<std::int64_t>(value.get<double>());
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::size_t used = 0;
        try {
            const auto parsed = std::stoll(text, &used);
            if (used == text.size()) {
                return parsed;
            }
        } catch (const std::exception&) {
        }
        throw std::invalid_argument("invalid literal for int() with base 10: '" + text + "'");
    }
    throw std::invalid_argument("int() argument must be a string or a real number");
}

const Json* member(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

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

std::string sha256_cancellable(const std::filesystem::path& file, const CancelToken& cancel) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                fv::base_utf8::path_to_utf8(file));
    }
    Sha256 hash;
    std::vector<char> block(std::size_t{1} << 20);
    while (in) {
        if (cancel.cancelled()) {
            throw ComponentCanceled();
        }
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        hash.update(std::string_view(block.data(), static_cast<std::size_t>(in.gcount())));
    }
    if (in.bad()) {
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                fv::base_utf8::path_to_utf8(file));
    }
    return hash.hexdigest();
}

void copy_file_cancellable(const std::filesystem::path& from, const std::filesystem::path& to,
                           const CancelToken& cancel) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                fv::base_utf8::path_to_utf8(from));
    }
    bool done = false;
    try {
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::system_error(std::make_error_code(std::errc::permission_denied),
                                    fv::base_utf8::path_to_utf8(to));
        }
        std::vector<char> block(std::size_t{4} << 20);
        while (in) {
            if (cancel.cancelled()) {
                throw ComponentCanceled();
            }
            in.read(block.data(), static_cast<std::streamsize>(block.size()));
            out.write(block.data(), in.gcount());
            if (!out) {
                throw std::system_error(std::make_error_code(std::errc::io_error),
                                        fv::base_utf8::path_to_utf8(to));
            }
        }
        if (in.bad()) {
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    fv::base_utf8::path_to_utf8(from));
        }
        out.close();
        if (!out) {
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    fv::base_utf8::path_to_utf8(to));
        }
        done = true;
    } catch (...) {
        if (!done) {
            std::error_code ignored;
            std::filesystem::remove(to, ignored);
        }
        throw;
    }
}

} // namespace fv::models::detail
