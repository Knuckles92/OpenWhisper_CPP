#include "meeting_intel/context_folder.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <system_error>
#include <tuple>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "meeting_intel/detail/office_text.h"
#include "meeting_intel/detail/paths.h"
#include "meeting_intel/detail/text.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/pdf_text.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger logger("meeting.context_folder");

constexpr std::array<std::string_view, 12> kSkipDirNames = {
    ".obsidian", ".git",    ".svn",  ".hg",  ".trash",      ".smart-env",
    ".idea",     ".vscode", ".venv", "venv", "__pycache__", "node_modules",
};
constexpr std::array<std::string_view, 4> kDocExts = {".pdf", ".docx", ".pptx", ".xlsx"};
constexpr std::array<std::string_view, 44> kSkipExts = {
    ".png", ".jpg", ".jpeg", ".gif",   ".webp", ".bmp", ".ico", ".tif", ".tiff", ".heic",  ".avif",
    ".mp3", ".wav", ".m4a",  ".flac",  ".ogg",  ".aac", ".mp4", ".mov", ".mkv",  ".avi",   ".webm",
    ".zip", ".tar", ".gz",   ".bz2",   ".7z",   ".rar", ".exe", ".dll", ".so",   ".dylib", ".bin",
    ".o",   ".obj", ".pyc",  ".class", ".wasm", ".doc", ".ppt", ".xls", ".odt",  ".odp",   ".ods",
};

// Bytes of a plain file looked at for a NUL, and for the share of printable characters.
constexpr std::size_t kBinaryProbeBytes = 8192;
constexpr std::size_t kPrintableProbeBytes = 4096;
constexpr double kMinPrintableShare = 0.85;

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& items, std::string_view item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}

Json result(bool ok, std::string text, Json hits = Json::array()) {
    Json out = Json::object();
    out["ok"] = ok;
    out["text"] = std::move(text);
    out["hits"] = std::move(hits);
    return out;
}

Json disabled(std::string_view text) {
    Json out = Json::object();
    out["ok"] = false;
    out["disabled"] = true;
    out["text"] = std::string(text);
    out["hits"] = Json::array();
    return out;
}

std::string utf8(const fs::path& path) {
    return base_utf8::path_to_utf8(path);
}

bool skip_dir(std::string_view name) {
    const std::string lowered = py::lower(name);
    return lowered.starts_with('.') || contains(kSkipDirNames, lowered);
}

// Path.suffix, lower-cased.
std::string suffix_of(const fs::path& path) {
    const std::string name = utf8(path.filename());
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) {
        return "";
    }
    return py::lower(name.substr(dot));
}

std::string hit_title(const std::string& relpath) {
    const auto slash = relpath.rfind('/');
    return slash == std::string::npos ? relpath : relpath.substr(slash + 1);
}

std::string opaque_ref(const std::string& relpath, std::size_t index) {
    return "file:" + relpath + ":" + std::to_string(index);
}

// Python's strict UTF-8 decoder: no overlong forms, surrogates or code points past U+10FFFF.
bool valid_utf8(std::string_view bytes) {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[index]);
        std::size_t length = 0;
        char32_t min = 0;
        char32_t cp = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
            min = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
            min = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        if (index + length > bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[index + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        index += length;
    }
    return true;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::io_error), "could not open a file");
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The text cache: insertion order, not use order, decides what is dropped.
struct TextCache {
    using Key = std::tuple<std::string, std::string, std::uintmax_t, std::int64_t>;
    std::mutex mutex;
    std::map<Key, std::string> texts;
    std::vector<Key> order;
};

TextCache& cache() {
    static TextCache instance;
    return instance;
}

std::string cached_text(const fs::path& root, const fs::path& path, const std::string& relpath) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        return "";
    }
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return "";
    }
    TextCache::Key key{utf8(root), relpath, size,
                       static_cast<std::int64_t>(modified.time_since_epoch().count())};
    TextCache& shared = cache();
    {
        std::lock_guard lock(shared.mutex);
        if (const auto found = shared.texts.find(key); found != shared.texts.end()) {
            return found->second;
        }
    }
    std::string text = detail::extract_text(path);
    if (text.empty()) {
        return "";
    }
    std::lock_guard lock(shared.mutex);
    shared.texts[key] = text;
    shared.order.push_back(std::move(key));
    while (shared.order.size() > kFolderMaxCacheEntries) {
        shared.texts.erase(shared.order.front());
        shared.order.erase(shared.order.begin());
    }
    return text;
}

struct FolderFile {
    fs::path path;
    std::string relpath;
};

// _iter_files: os.walk top-down without following links, directories and files sorted, hidden
// and skipped directories pruned; dot files, links and non-files skipped; at most 200 accepted
// files, counted only when accepted.
std::vector<FolderFile> iter_files(const fs::path& root) {
    std::vector<FolderFile> files;
    std::vector<fs::path> stack{root};
    while (!stack.empty()) {
        const fs::path top = stack.back();
        stack.pop_back();
        if (detail::islink(top)) {
            continue;
        }
        std::error_code error;
        fs::directory_iterator entries(top, error);
        if (error) {
            continue;
        }
        std::vector<std::string> dirs;
        std::vector<std::string> nondirs;
        for (fs::directory_iterator end; entries != end; entries.increment(error)) {
            if (error) {
                break;
            }
            std::error_code status_error;
            const bool is_dir = entries->is_directory(status_error) && !status_error;
            (is_dir ? dirs : nondirs).push_back(utf8(entries->path().filename()));
        }
        if (error) {
            continue;
        }
        std::erase_if(dirs, [](const std::string& name) { return skip_dir(name); });
        std::sort(dirs.begin(), dirs.end());
        std::sort(nondirs.begin(), nondirs.end());
        for (const auto& name : nondirs) {
            if (files.size() >= kFolderMaxFilesScanned) {
                return files;
            }
            if (name.starts_with('.')) {
                continue;
            }
            const fs::path full = top / base_utf8::path_from_utf8(name);
            std::error_code file_error;
            if (detail::islink(full) || !fs::is_regular_file(full, file_error)) {
                continue;
            }
            auto relpath = detail::relative_display(root, full);
            if (!relpath) {
                continue;
            }
            files.push_back({full, std::move(*relpath)});
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            const fs::path next = top / base_utf8::path_from_utf8(*it);
            if (!detail::islink(next)) {
                stack.push_back(next);
            }
        }
    }
    return files;
}

std::optional<fs::path> configured_root(const std::function<Json()>& settings) {
    std::string raw;
    try {
        raw = resolve_meeting_context_folder_path(settings());
    } catch (const std::exception& error) {
        logger.warning("Could not read the knowledge-folder path setting ({})",
                       detail::type_name(error));
        return std::nullopt;
    }
    const std::string cleaned = py::strip(raw);
    if (cleaned.empty()) {
        return std::nullopt;
    }
    const auto root = detail::resolve_strict(base_utf8::path_from_utf8(cleaned));
    if (!root) {
        return std::nullopt;
    }
    std::error_code error;
    if (!fs::is_directory(*root, error) || detail::islink(*root)) {
        return std::nullopt;
    }
    return root;
}

Json search_folder(const fs::path& root, std::string_view query, std::int64_t limit) {
    const std::vector<std::string> tokens = detail::query_tokens(query);
    if (tokens.empty()) {
        return result(false, "Provide a search query or a relative file path.");
    }
    struct Scored {
        int score;
        std::string relpath;
        std::string lowered_path;
        std::string text;
    };
    std::vector<Scored> scored;
    std::size_t scanned = 0;
    for (const auto& file : iter_files(root)) {
        ++scanned;
        std::string text = cached_text(root, file.path, file.relpath);
        if (text.empty()) {
            continue;
        }
        const int score = detail::score(file.relpath, text, tokens);
        if (score <= 0) {
            continue;
        }
        scored.push_back({score, file.relpath, py::lower(file.relpath), std::move(text)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.lowered_path < b.lowered_path;
    });
    Json hits = Json::array();
    std::string text;
    auto budget = static_cast<std::int64_t>(kFolderMaxTotalChars);
    const std::size_t count = std::min(scored.size(), static_cast<std::size_t>(limit));
    for (std::size_t index = 0; index < count; ++index) {
        const Scored& item = scored[index];
        const std::string snippet = detail::snippet(item.text, tokens, kFolderMaxSnippetChars);
        if (snippet.empty()) {
            continue;
        }
        const std::string ref = opaque_ref(item.relpath, index + 1);
        const std::string line = ref + "  " + item.relpath + " — " + snippet;
        const auto length = static_cast<std::int64_t>(py::len(line));
        if (length > budget) {
            break;
        }
        text += (hits.empty() ? "" : "\n") + line;
        budget -= length + 1;
        Json hit = Json::object();
        hit["ref"] = ref;
        hit["path"] = item.relpath;
        hit["title"] = hit_title(item.relpath);
        hit["snippet"] = snippet;
        hits.push_back(std::move(hit));
    }
    if (hits.empty()) {
        const std::string extra = scanned >= kFolderMaxFilesScanned ? " Scan limit reached." : "";
        return result(true, py::rstrip("No knowledge-folder matches." + extra));
    }
    return result(true, std::move(text), std::move(hits));
}

Json slice_file(const fs::path& root, std::string_view relative_path, std::string_view query) {
    const auto path = detail::safe_join(root, relative_path);
    if (!path) {
        return result(false, "That file is outside the knowledge folder.");
    }
    std::error_code error;
    if (!fs::is_regular_file(*path, error)) {
        return result(false, "Unknown knowledge-folder file.");
    }
    const auto relpath = detail::relative_display(root, *path);
    if (!relpath) {
        return result(false, "That file is outside the knowledge folder.");
    }
    const std::string text = cached_text(root, *path, *relpath);
    if (text.empty()) {
        return result(true, "No extractable text in " + *relpath + ".");
    }
    const std::vector<std::string> tokens = detail::query_tokens(query);
    const std::string passage = tokens.empty()
                                    ? detail::clip(text, kFolderMaxPassageChars)
                                    : detail::snippet(text, tokens, kFolderMaxPassageChars);
    if (passage.empty()) {
        return result(true, "No extractable text in " + *relpath + ".");
    }
    const std::string ref = opaque_ref(*relpath, 1);
    Json hit = Json::object();
    hit["ref"] = ref;
    hit["path"] = *relpath;
    hit["title"] = hit_title(*relpath);
    hit["snippet"] = passage;
    Json hits = Json::array();
    hits.push_back(std::move(hit));
    return result(true, detail::clip(ref + "  " + *relpath + "\n" + passage, kFolderMaxTotalChars),
                  std::move(hits));
}

} // namespace

namespace detail {

std::vector<std::string> query_tokens(std::string_view query) {
    static const py::Regex token(R"([A-Za-z0-9_]{2,})");
    std::vector<std::string> tokens;
    for (const auto& found : token.findall(query)) {
        tokens.push_back(py::lower(found));
    }
    return tokens;
}

int score(std::string_view relpath, std::string_view text, const std::vector<std::string>& tokens) {
    const std::string name = py::lower(relpath);
    const std::string haystack = py::lower(text);
    int total = 0;
    int matched = 0;
    for (const auto& token : tokens) {
        const bool in_name = name.find(token) != std::string::npos;
        int count = 0;
        for (std::size_t at = haystack.find(token); at != std::string::npos;
             at = haystack.find(token, at + token.size())) {
            ++count;
        }
        if (in_name) {
            total += 8;
            ++matched;
        }
        if (count > 0) {
            total += std::min(count, 10);
            ++matched;
        }
    }
    if (matched == 0) {
        return 0;
    }
    if (std::all_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
            return name.find(token) != std::string::npos;
        })) {
        total += 12;
    }
    return total;
}

std::string snippet(std::string_view text, const std::vector<std::string>& tokens,
                    std::size_t max_chars) {
    const std::string cleaned = sanitize(text);
    const std::string lowered = py::lower(cleaned);
    std::int64_t start = 0;
    for (const auto& token : tokens) {
        const auto at = lowered.find(token);
        if (at != std::string::npos) {
            const auto index =
                static_cast<std::int64_t>(py::len(std::string_view(lowered).substr(0, at)));
            const auto before = static_cast<std::int64_t>(std::max<std::size_t>(24, max_chars / 5));
            start = std::max<std::int64_t>(0, index - before);
            break;
        }
    }
    const std::string window =
        py::slice(cleaned, start, start + static_cast<std::int64_t>(max_chars) + 40);
    return clip(window, max_chars);
}

std::optional<fs::path> safe_join(const fs::path& root, std::string_view relative_path) {
    std::string raw = py::strip([&] {
        std::string replaced(relative_path);
        std::replace(replaced.begin(), replaced.end(), '\\', '/');
        return replaced;
    }());
    if (raw.empty() || raw.starts_with('/') || raw.starts_with('~')) {
        return std::nullopt;
    }
    // Path(raw).parts: a drive (with its root) first, then the names; "." and empty names drop.
    std::vector<std::string> parts;
    std::string rest = raw;
#if defined(_WIN32)
    if (rest.size() > 1 && rest[1] == ':') {
        const bool rooted = rest.size() > 2 && rest[2] == '/';
        parts.push_back(rest.substr(0, 2) + (rooted ? "\\" : ""));
        rest = rest.substr(rooted ? 3 : 2);
    }
#endif
    for (auto& part : py::split_on(rest, "/")) {
        if (part.empty() || part == ".") {
            continue;
        }
        parts.push_back(std::move(part));
    }
    if (parts.empty()) {
        return std::nullopt;
    }
    for (const auto& part : parts) {
        if (part == ".." || part.starts_with('.')) {
            return std::nullopt;
        }
    }
    fs::path candidate = root;
    for (const auto& part : parts) {
        candidate /= base_utf8::path_from_utf8(part);
    }
    if (islink(candidate)) {
        return std::nullopt;
    }
    const auto resolved = resolve_lax(candidate);
    if (!resolved || !relative_to(*resolved, root)) {
        return std::nullopt;
    }
    return resolved;
}

std::optional<std::string> relative_display(const fs::path& root, const fs::path& path) {
    const auto resolved = resolve_strict(path);
    if (!resolved) {
        return std::nullopt;
    }
    const auto relative = relative_to(*resolved, root);
    if (!relative) {
        return std::nullopt;
    }
    std::string display;
    for (const auto& part : *relative) {
        const std::string name = base_utf8::path_to_utf8(part);
        if (name.starts_with('.') || contains(kSkipDirNames, py::lower(name))) {
            return std::nullopt;
        }
        display += (display.empty() ? "" : "/") + name;
    }
    return display;
}

std::string extract_plain(std::string_view bytes) {
    if (bytes.substr(0, kBinaryProbeBytes).find('\0') != std::string_view::npos) {
        return "";
    }
    if (valid_utf8(bytes)) {
        constexpr std::string_view bom = "\xEF\xBB\xBF";
        return std::string(bytes.starts_with(bom) ? bytes.substr(bom.size()) : bytes);
    }
    const std::string_view sample = bytes.substr(0, kPrintableProbeBytes);
    std::size_t printable = 0;
    for (const char c : sample) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 32 && byte <= 126) || byte == 9 || byte == 10 || byte == 13) {
            ++printable;
        }
    }
    if (!sample.empty() &&
        static_cast<double>(printable) / static_cast<double>(sample.size()) < kMinPrintableShare) {
        return "";
    }
    std::string latin1;
    latin1.reserve(bytes.size());
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            latin1 += c;
        } else {
            latin1 += static_cast<char>(0xC0 | (byte >> 6));
            latin1 += static_cast<char>(0x80 | (byte & 0x3F));
        }
    }
    return latin1;
}

std::string extract_text(const fs::path& path) {
    const std::string suffix = suffix_of(path);
    if (contains(kSkipExts, suffix)) {
        return "";
    }
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        return "";
    }
    const std::uintmax_t limit =
        contains(kDocExts, suffix) ? kFolderMaxDocBytes : kFolderMaxFileBytes;
    if (size == 0 || size > limit) {
        return "";
    }
    std::string extracted;
    try {
        if (suffix == ".pdf") {
            extracted = pdf_text(path, kFolderMaxPdfPages).value_or("");
        } else if (suffix == ".docx") {
            extracted = extract_docx(path);
        } else if (suffix == ".pptx") {
            extracted = extract_pptx(path);
        } else if (suffix == ".xlsx") {
            extracted = extract_xlsx(path, kFolderMaxXlsxRows, kFolderMaxXlsxCols);
        } else {
            extracted = extract_plain(read_bytes(path));
        }
    } catch (const std::exception& failure) {
        logger.debug("Knowledge-folder extract failed for {} ({})", utf8(path), type_name(failure));
        return "";
    }
    return clip(sanitize(extracted), kFolderMaxExtractedChars);
}

} // namespace detail

bool context_folder_enabled(const std::function<Json()>& settings) {
    try {
        return resolve_meeting_context_folder_enabled(settings());
    } catch (const std::exception& error) {
        logger.warning("Could not read the knowledge-folder setting; search stays off ({})",
                       detail::type_name(error));
        return false;
    }
}

void clear_context_folder_cache() {
    TextCache& shared = cache();
    std::lock_guard lock(shared.mutex);
    shared.texts.clear();
    shared.order.clear();
}

Json search_context_files(const std::function<Json()>& settings, std::string_view query,
                          std::string_view relative_path, const Json& limit) {
    if (!context_folder_enabled(settings)) {
        return disabled("Knowledge-folder search is disabled.");
    }
    const auto root = configured_root(settings);
    if (!root) {
        return disabled("Knowledge-folder search is not available.");
    }
    const std::int64_t hit_limit =
        detail::clamp_limit(limit, kFolderDefaultHitLimit, kFolderMaxHitLimit);
    const std::string target = py::strip(relative_path);
    try {
        if (!target.empty()) {
            return slice_file(*root, target, query);
        }
        return search_folder(*root, query, hit_limit);
    } catch (const std::exception& error) {
        logger.error_with(error, "Knowledge-folder search failed");
        return result(false, "Knowledge-folder search failed.");
    }
}

} // namespace fv::meeting_intel
