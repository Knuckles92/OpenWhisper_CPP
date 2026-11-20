#include "cli/installed_check.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>

#include "base/identity.h"
#include "base/utf8.h"
#include "cli/pe_imports.h"

namespace fv::cli {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

std::filesystem::path relative_path(std::string_view relative) {
    return path_from_utf8(relative).make_preferred();
}

std::string display(std::string_view relative) {
    return path_to_utf8(relative_path(relative));
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// The files directly in the folder by their lower-case names, spelled as on disk: the loader
// finds a DLL beside the program whatever the case of the name it imports.
std::map<std::string, std::string> files_by_folded_name(const std::filesystem::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        std::error_code ignored;
        if (entry.is_regular_file(ignored)) {
            const std::string name = path_to_utf8(entry.path().filename());
            files.emplace(lower(name), name);
        }
    }
    return files;
}

} // namespace

std::vector<MissingFile>
missing_installed_files(const std::filesystem::path& folder, const InstalledLayout& layout,
                        const std::function<bool(std::string_view)>& is_system) {
    std::vector<MissingFile> missing;
    std::set<std::string> reported; // lower-case relative paths
    const auto report = [&](std::string_view relative, std::string imported_by) {
        if (reported.insert(lower(display(relative))).second) {
            missing.push_back(MissingFile{display(relative), std::move(imported_by)});
        }
    };

    for (const std::string& relative : layout.required) {
        if (!is_file(folder / relative_path(relative))) {
            report(relative, "");
        }
    }

    const auto local_only = [&](std::string_view name) {
        const std::string folded = lower(name);
        return std::ranges::any_of(layout.local_only_prefixes, [&](const std::string& prefix) {
            return folded.starts_with(prefix);
        });
    };
    const std::map<std::string, std::string> beside = files_by_folded_name(folder);
    std::deque<std::string> pending(layout.import_roots.begin(), layout.import_roots.end());
    std::set<std::string> walked;
    while (!pending.empty()) {
        const std::string relative = std::move(pending.front());
        pending.pop_front();
        if (!walked.insert(lower(display(relative))).second) {
            continue;
        }
        const std::filesystem::path file = folder / relative_path(relative);
        if (!is_file(file)) {
            continue; // reported above when the layout requires it
        }
        const std::optional<std::vector<std::string>> imports = pe_imports(file);
        if (!imports) {
            report(relative, "(not a Windows executable)");
            continue;
        }
        const std::string importer = path_to_utf8(relative_path(relative).filename());
        for (const std::string& name : *imports) {
            if (const auto found = beside.find(lower(name)); found != beside.end()) {
                pending.push_back(found->second);
            } else if (local_only(name) || !is_system(name)) {
                report(name, importer);
            }
        }
    }
    return missing;
}

int run_installed_self_test() {
    try {
        const std::filesystem::path folder = executable_folder();
        const InstalledLayout layout = installed_layout();
        const std::vector<MissingFile> missing =
            missing_installed_files(folder, layout, is_system_library);
        if (missing.empty()) {
            std::printf("%s installed self-test passed\n", std::string(identity().product).c_str());
            return 0;
        }
        std::fprintf(stderr, "fiorivoce-cli: installed self-test failed: %zu %s missing from %s:\n",
                     missing.size(), missing.size() == 1 ? "file is" : "files are",
                     path_to_utf8(folder).c_str());
        for (const MissingFile& file : missing) {
            if (file.imported_by.empty()) {
                std::fprintf(stderr, "  %s\n", file.path.c_str());
            } else if (file.imported_by.starts_with("(")) {
                std::fprintf(stderr, "  %s %s\n", file.path.c_str(), file.imported_by.c_str());
            } else {
                std::fprintf(stderr, "  %s (imported by %s)\n", file.path.c_str(),
                             file.imported_by.c_str());
            }
        }
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fiorivoce-cli: installed self-test failed: %s\n", error.what());
        return 1;
    }
}

} // namespace fv::cli
