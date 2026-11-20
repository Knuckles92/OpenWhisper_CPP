#include "bench/provenance.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "base/hash.h"
#include "base/platform_info.h"
#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::bench {

namespace {

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// HEAD's commit without running git: a detached HEAD holds it, a branch names a ref file or a
// line of packed-refs.
std::optional<std::string> head_commit(const std::filesystem::path& root) {
    const std::filesystem::path git = root / ".git";
    const auto head = read_text(git / "HEAD");
    if (!head) {
        return std::nullopt;
    }
    const std::string line = py::strip(*head);
    if (!line.starts_with("ref: ")) {
        return line.empty() ? std::nullopt : std::optional(line);
    }
    const std::string ref = line.substr(5);
    if (const auto value = read_text(git / path_from_utf8(ref))) {
        return py::strip(*value);
    }
    if (const auto packed = read_text(git / "packed-refs")) {
        for (const std::string& entry : py::split_on(*packed, "\n")) {
            if (entry.ends_with(" " + ref)) {
                return entry.substr(0, entry.find(' '));
            }
        }
    }
    return std::nullopt;
}

Json file_size_or_null(const std::optional<std::filesystem::path>& path) {
    if (!path) {
        return nullptr;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(*path, error);
    Json out{{"path", path_to_utf8(*path)}};
    out["bytes"] = error ? Json(nullptr) : Json(size);
    return out;
}

unsigned cpu_count() {
    return std::max(1U, std::thread::hardware_concurrency());
}

} // namespace

std::string file_identity(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read " + path_to_utf8(path) + ".");
    }
    Sha256 digest;
    std::string block(1024 * 1024, '\0');
    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        digest.update(std::string_view(block.data(), static_cast<std::size_t>(in.gcount())));
    }
    return digest.hexdigest();
}

const std::vector<std::string>& provenance_source_dirs() {
    static const std::vector<std::string> dirs = {"src/bench", "src/meeting_capture", "src/speech",
                                                  "src/engine", "src/dsp"};
    return dirs;
}

const std::vector<std::string>& provenance_source_files() {
    static const std::vector<std::string> files = {"src/settings/keys.inc"};
    return files;
}

Json identity(const IdentityRequest& request) {
    Json source = Json::object();
    if (request.source_root) {
        const std::filesystem::path& root = *request.source_root;
        for (const std::string& dir : provenance_source_dirs()) {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(root / dir, error);
                 !error && it != std::filesystem::recursive_directory_iterator(); ++it) {
                const auto extension = it->path().extension();
                if (it->is_regular_file() &&
                    (extension == ".h" || extension == ".cpp" || extension == ".inc")) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                std::string relative = path_to_utf8(file.lexically_relative(root));
                std::replace(relative.begin(), relative.end(), '\\', '/');
                source[relative] = file_identity(file);
            }
        }
        for (const std::string& file : provenance_source_files()) {
            source[file] = file_identity(root / path_from_utf8(file));
        }
    }
    Json inputs = Json::object();
    for (const auto& input : request.inputs) {
        inputs[path_to_utf8(std::filesystem::absolute(input).lexically_normal())] =
            file_identity(input);
    }
    return Json{{"schema", 1},
                {"source", std::move(source)},
                {"cpu_count", cpu_count()},
                {"inputs", std::move(inputs)},
                {"settings", request.settings.is_null() ? Json::object() : request.settings},
                {"model", request.model},
                {"platform", std::string(python_platform())},
                {"machine", host_machine()}};
}

bool reusable(const Json& result, const Json& expected) {
    return result.is_object() && result.contains("provenance") &&
           result.at("provenance") == expected && !result.contains("error");
}

Json report_provenance(const std::optional<std::filesystem::path>& source_root,
                       const std::optional<std::filesystem::path>& runtime_dir,
                       const std::optional<std::filesystem::path>& model_path) {
    std::optional<std::string> commit;
    if (source_root) {
        commit = head_commit(*source_root);
    }
    Json runtime = nullptr;
    if (runtime_dir) {
        runtime = Json{{"path", path_to_utf8(*runtime_dir)}};
    }
    return Json{{"commit", commit ? Json(*commit) : Json(nullptr)},
                {"runtime", std::move(runtime)},
                {"model", file_size_or_null(model_path)},
                {"machine", Json{{"platform", std::string(python_platform())},
                                 {"machine", host_machine()},
                                 {"cpu_count", cpu_count()}}}};
}

Json make_report(std::string kind, Json metrics, Json provenance) {
    return Json{{"schema", 1},
                {"kind", std::move(kind)},
                {"metrics", std::move(metrics)},
                {"provenance", std::move(provenance)}};
}

void write_json(const std::filesystem::path& path, const Json& value) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!out) {
        throw std::runtime_error("Cannot write " + path_to_utf8(path) + ".");
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    auto text = read_text(path);
    if (!text) {
        throw std::runtime_error("Cannot read " + path_to_utf8(path) + ".");
    }
    if (text->starts_with("\xEF\xBB\xBF")) {
        text->erase(0, 3);
    }
    return std::move(*text);
}

Json read_json(const std::filesystem::path& path) {
    return Json::parse(read_text_file(path));
}

std::optional<std::filesystem::path> find_source_root(const std::filesystem::path& start) {
    if (const auto configured = env_utf8("FIORIVOCE_SOURCE_ROOT")) {
        return path_from_utf8(*configured);
    }
    std::error_code error;
    auto at = std::filesystem::absolute(start, error);
    while (!error && !at.empty()) {
        if (std::filesystem::exists(at / "CMakePresets.json") &&
            std::filesystem::is_directory(at / "src" / "bench")) {
            return at;
        }
        if (at == at.parent_path()) {
            break;
        }
        at = at.parent_path();
    }
    return std::nullopt;
}

} // namespace fv::bench
