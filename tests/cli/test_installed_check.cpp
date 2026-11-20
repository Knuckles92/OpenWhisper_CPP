// fiorivoce-cli --self-test --installed: the install folder holds every file its programs need.
// No OpenWhisper test: the frozen build's self-test imported modules instead (a34 6.7 item 6).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli/installed_check.h"
#include "cli/pe_imports.h"
#include "cli_process.h"
#include "testkit/testkit.h"

template <> struct Catch::StringMaker<fv::cli::MissingFile> {
    static std::string convert(const fv::cli::MissingFile& file) {
        return "{" + file.path + ", " + file.imported_by + "}";
    }
};

using Catch::Matchers::ContainsSubstring;
using fv::cli::InstalledLayout;
using fv::cli::missing_installed_files;
using fv::cli::MissingFile;
using fv::cli::pe_imports;
using fv::clitest::run_cli;
using fv::clitest::write_text;

namespace {

namespace fs = std::filesystem;

void put16(std::string& image, std::size_t at, std::uint32_t value) {
    image[at] = static_cast<char>(value & 0xFF);
    image[at + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void put32(std::string& image, std::size_t at, std::uint32_t value) {
    put16(image, at, value & 0xFFFF);
    put16(image, at + 2, value >> 16);
}

// The smallest PE32+ image whose import table names `imports`: the headers in the first 0x200
// bytes, then one section at RVA 0x1000 holding the import descriptors and the names. With
// `directory` false the image has no import directory at all.
std::string make_pe(const std::vector<std::string>& imports, bool directory = true) {
    constexpr std::size_t kPe = 0x40;
    constexpr std::size_t kOptional = kPe + 4 + 20;
    constexpr std::size_t kOptionalSize = 240;
    constexpr std::size_t kSectionTable = kOptional + kOptionalSize;
    constexpr std::uint32_t kSectionRva = 0x1000;
    constexpr std::size_t kSectionOffset = 0x200;

    std::string section((imports.size() + 1) * 20, '\0');
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const auto name_rva = static_cast<std::uint32_t>(kSectionRva + section.size());
        section += imports[i];
        section += '\0';
        put32(section, i * 20 + 12, name_rva);
        put32(section, i * 20 + 16, kSectionRva); // a first thunk; never followed
    }
    section.resize((section.size() + 0x1FF) / 0x200 * 0x200, '\0');

    std::string image(kSectionOffset, '\0');
    image[0] = 'M';
    image[1] = 'Z';
    put32(image, 0x3C, kPe);
    image.replace(kPe, 4, std::string("PE\0\0", 4));
    put16(image, kPe + 4, 0x8664);                // AMD64
    put16(image, kPe + 4 + 2, 1);                 // one section
    put16(image, kPe + 4 + 16, kOptionalSize);    // optional header size
    put16(image, kOptional, 0x20B);               // PE32+
    put32(image, kOptional + 60, kSectionOffset); // size of headers
    put32(image, kOptional + 108, 16);            // data directories
    if (directory) {
        put32(image, kOptional + 112 + 8, kSectionRva); // import directory
        put32(image, kOptional + 112 + 12, static_cast<std::uint32_t>((imports.size() + 1) * 20));
    }
    image.replace(kSectionTable, 6, ".idata");
    put32(image, kSectionTable + 8, static_cast<std::uint32_t>(section.size()));
    put32(image, kSectionTable + 12, kSectionRva);
    put32(image, kSectionTable + 16, static_cast<std::uint32_t>(section.size()));
    put32(image, kSectionTable + 20, kSectionOffset);
    return image + section;
}

std::optional<std::vector<std::string>> imports_of(const std::string& bytes) {
    return pe_imports(std::as_bytes(std::span<const char>(bytes)));
}

bool only_kernel32(std::string_view name) {
    std::string folded(name);
    for (char& c : folded) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return folded == "kernel32.dll";
}

std::string native(std::string_view relative) {
    return fs::path(relative).make_preferred().string();
}

} // namespace

TEST_CASE("pe imports reads the names of the import table", "[installed_check]") {
    CHECK(imports_of(make_pe({"KERNEL32.dll", "libz.dll", "Qt6Core.dll"})) ==
          std::vector<std::string>{"KERNEL32.dll", "libz.dll", "Qt6Core.dll"});
    CHECK(imports_of(make_pe({})) == std::vector<std::string>{});
    CHECK(imports_of(make_pe({"x.dll"}, false)) == std::vector<std::string>{});
}

TEST_CASE("pe imports refuses bytes that are not an image", "[installed_check]") {
    CHECK_FALSE(imports_of("").has_value());
    CHECK_FALSE(imports_of("not an executable at all, just text").has_value());
    std::string no_signature = make_pe({"a.dll"});
    no_signature[0x40] = 'X';
    CHECK_FALSE(imports_of(no_signature).has_value());
    // The table runs past the end of the file.
    const std::string whole = make_pe({"a.dll"});
    CHECK_FALSE(imports_of(whole.substr(0, 0x200 + 10)).has_value());
}

TEST_CASE("an install folder with every file passes", "[installed_check]") {
    fv::testkit::TempDataRoot root;
    const fs::path folder = root.path();
    fs::create_directories(folder / "plugins");
    const InstalledLayout layout{{"app.exe", "plugins/look.dll", "NOTICES.md"},
                                 {"app.exe", "plugins/look.dll"},
                                 {"msvcp140"}};
    write_text(folder / "app.exe",
               make_pe({"KERNEL32.dll", "LIBZ.DLL", "msvcp140.dll", "MSVCP140_1.dll"}));
    write_text(folder / "plugins" / "look.dll", make_pe({"kernel32.dll", "libpng.dll"}));
    write_text(folder / "libz.dll", make_pe({"KERNEL32.dll", "libmissing.dll"}));

    SECTION("each missing file once: the layout's first, then the imports as the walk meets "
            "them") {
        const std::vector<MissingFile> missing =
            missing_installed_files(folder, layout, only_kernel32);
        CHECK(missing == std::vector<MissingFile>{{"NOTICES.md", ""},
                                                  {"msvcp140.dll", "app.exe"},
                                                  {"MSVCP140_1.dll", "app.exe"},
                                                  {"libpng.dll", "look.dll"},
                                                  {"libmissing.dll", "libz.dll"}});
    }

    SECTION("nothing is missing once they are there") {
        write_text(folder / "NOTICES.md", "notices");
        for (const char* name :
             {"msvcp140.dll", "msvcp140_1.dll", "libpng.dll", "libmissing.dll"}) {
            write_text(folder / name, make_pe({"KERNEL32.dll"}));
        }
        CHECK(missing_installed_files(folder, layout, only_kernel32).empty());
    }

    SECTION("the C++ runtime must be beside the programs even when the system has it") {
        const auto everything_is_system = [](std::string_view) {
            return true;
        };
        const std::vector<MissingFile> missing =
            missing_installed_files(folder, layout, everything_is_system);
        CHECK(missing == std::vector<MissingFile>{{"NOTICES.md", ""},
                                                  {"msvcp140.dll", "app.exe"},
                                                  {"MSVCP140_1.dll", "app.exe"}});
    }

    SECTION("a file the layout names but that is not an image is reported") {
        write_text(folder / "plugins" / "look.dll", "text, not a library");
        const std::vector<MissingFile> missing =
            missing_installed_files(folder, layout, only_kernel32);
        CHECK(std::ranges::find(
                  missing, MissingFile{native("plugins/look.dll"), "(not a Windows executable)"}) !=
              missing.end());
        // Its imports are not followed: libpng.dll is not reported.
        CHECK(std::ranges::find(missing, MissingFile{"libpng.dll", "look.dll"}) == missing.end());
    }

    SECTION("a missing program is reported once") {
        fs::remove(folder / "app.exe");
        const std::vector<MissingFile> missing =
            missing_installed_files(folder, layout, only_kernel32);
        CHECK(missing == std::vector<MissingFile>{
                             {"app.exe", ""}, {"NOTICES.md", ""}, {"libpng.dll", "look.dll"}});
    }
}

#if defined(_WIN32)

TEST_CASE("the windows layout lists the programs, qt, the runtime and the notices",
          "[installed_check]") {
    fv::testkit::TempDataRoot root;
    std::vector<std::string> paths;
    for (const MissingFile& file : missing_installed_files(root.path(), fv::cli::installed_layout(),
                                                           fv::cli::is_system_library)) {
        CHECK(file.imported_by.empty());
        paths.push_back(file.path);
    }
    CHECK(paths == std::vector<std::string>{
                       "fiorivoce.exe", "fiorivoce-cli.exe", "fiorivoce-updater.exe",
                       "webui\\dist\\index.html", "platforms\\qwindows.dll",
                       "platforms\\qoffscreen.dll", "styles\\qmodernwindowsstyle.dll",
                       "imageformats\\qsvg.dll", "iconengines\\qsvgicon.dll",
                       "tls\\qschannelbackend.dll", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                       "Qt6Svg.dll", "Qt6Network.dll", "vcruntime140.dll", "vcruntime140_1.dll",
                       "msvcp140.dll", "THIRD_PARTY_NOTICES.md"});
}

TEST_CASE("system libraries are found in the system folder", "[platform][installed_check]") {
    CHECK(fv::cli::is_system_library("KERNEL32.dll"));
    CHECK(fv::cli::is_system_library("api-ms-win-crt-runtime-l1-1-0.dll"));
    CHECK_FALSE(fv::cli::is_system_library("libz.dll"));
    CHECK_FALSE(fv::cli::is_system_library("Qt6Core.dll"));
}

TEST_CASE("only libraries windows itself ships count as the system's", "[installed_check]") {
    // A system folder where a third-party installer left a zlib and an OpenSSL beside Windows'
    // own kernel32: the install must still hold its own zlib and OpenSSL.
    fv::testkit::TempDataRoot root;
    const fs::path system = root.path() / "System32";
    fs::create_directories(system);
    for (const char* name : {"kernel32.dll", "zlib1.dll", "libcrypto-3-x64.dll", "Qt6Core.dll"}) {
        write_text(system / name, "MZ");
    }
    CHECK(fv::cli::is_system_library_in("KERNEL32.dll", system));
    CHECK_FALSE(fv::cli::is_system_library_in("zlib1.dll", system));
    CHECK_FALSE(fv::cli::is_system_library_in("libcrypto-3-x64.dll", system));
    CHECK_FALSE(fv::cli::is_system_library_in("Qt6Core.dll", system));
    // A Windows library that is not there is not found.
    CHECK_FALSE(fv::cli::is_system_library_in("user32.dll", system));
    // API sets are the loader's, wherever the folder is.
    CHECK(fv::cli::is_system_library_in("api-ms-win-core-synch-l1-2-0.dll", system));
    CHECK(fv::cli::is_system_library_in("ext-ms-win-ntuser-window-l1-1-0.dll", fs::path()));
}

TEST_CASE("every library the built programs import is beside them in the build folder",
          "[platform][installed_check]") {
    // The build folder is no install (no notices, the C++ runtime comes from the system), but
    // the vcpkg and Qt libraries are copied beside the programs, so following the imports of the
    // real executables must find each of them.
    const fs::path folder = fv::clitest::cli_executable().parent_path();
    InstalledLayout layout;
    layout.import_roots = {"fiorivoce-cli.exe"};
    if (fs::exists(folder / "fiorivoce.exe")) {
        layout.import_roots.emplace_back("fiorivoce.exe");
    }
    const std::optional<std::vector<std::string>> imports =
        pe_imports(fv::clitest::cli_executable());
    REQUIRE(imports.has_value());
    CHECK_FALSE(imports->empty());
    // A Debug build imports the Visual C++ debug runtime, which Visual Studio puts in System32.
    // is_system_library leaves it out on purpose: no install may depend on it.
    const auto system_or_debug_runtime = [](std::string_view name) {
        std::string folded(name);
        std::ranges::transform(folded, folded.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr std::string_view kDebugRuntime[] = {"concrt140d.dll",
                                                      "msvcp140d.dll",
                                                      "msvcp140d_atomic_wait.dll",
                                                      "msvcp140_1d.dll",
                                                      "msvcp140_2d.dll",
                                                      "msvcp140d_codecvt_ids.dll",
                                                      "ucrtbased.dll",
                                                      "vcruntime140d.dll",
                                                      "vcruntime140_1d.dll",
                                                      "vcruntime140_threadsd.dll"};
        return fv::cli::is_system_library(name) ||
               std::ranges::find(kDebugRuntime, folded) != std::end(kDebugRuntime);
    };
    const std::vector<MissingFile> missing =
        missing_installed_files(folder, layout, system_or_debug_runtime);
    for (const MissingFile& file : missing) {
        INFO(file.path << " imported by " << file.imported_by);
        CHECK(false);
    }
}

#endif

TEST_CASE("the installed self-test lists what the folder lacks and fails",
          "[platform][installed_check]") {
    // The build folder lacks the notices (and, outside Windows, the check is not supported).
    const fv::clitest::CliResult result = run_cli({"--self-test", "--installed"}, {});
    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, ContainsSubstring("installed self-test failed"));
#if defined(_WIN32)
    CHECK_THAT(result.err, ContainsSubstring("THIRD_PARTY_NOTICES.md"));
#endif
    CHECK_THAT(result.out, !ContainsSubstring("passed"));

    const fv::clitest::CliResult extra = run_cli({"--self-test", "--installed", "x"}, {});
    CHECK(extra.exit_code == 2);
    const fv::clitest::CliResult other = run_cli({"--self-test", "--other"}, {});
    CHECK(other.exit_code == 2);
}
