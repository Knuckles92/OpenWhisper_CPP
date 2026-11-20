// main.py _show_startup_error (map row A01-71): "<product>: <message>" on stderr always, and the
// message box (every FioriVoce build, where OpenWhisper showed it only frozen).
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define fv_dup _dup
#define fv_dup2 _dup2
#define fv_close _close
#define fv_fileno _fileno
#else
#include <unistd.h>
#define fv_dup dup
#define fv_dup2 dup2
#define fv_close close
#define fv_fileno fileno
#endif

#include "testkit/testkit.h"
#include "ui_shell/bootstrap.h"

namespace {

// What `write` printed on stderr, read back from a file stderr pointed at meanwhile.
template <typename Write> std::string stderrOf(const std::filesystem::path& file, Write write) {
    std::fflush(stderr);
    const int saved = fv_dup(fv_fileno(stderr));
    REQUIRE(saved >= 0);
    std::FILE* sink = nullptr;
#if defined(_WIN32)
    // The wide name: the narrow one is in the ANSI code page. fopen is deprecated under MSVC.
    REQUIRE(_wfopen_s(&sink, file.c_str(), L"wb") == 0);
#else
    sink = std::fopen(file.c_str(), "wb");
#endif
    REQUIRE(sink != nullptr);
    fv_dup2(fv_fileno(sink), fv_fileno(stderr));
    write();
    std::fflush(stderr);
    fv_dup2(saved, fv_fileno(stderr));
    fv_close(saved);
    std::fclose(sink);
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("a startup error goes to stderr with the product's name and to the error box", "[unit]") {
    fv::testkit::TempDataRoot root;
    std::vector<std::string> boxes;
    const std::string printed = stderrOf(root.path() / "stderr.txt", [&] {
        fv::showStartupError("Could not safely open application data: busy.",
                             [&](const std::string& message) { boxes.push_back(message); });
    });
    std::string text;
    for (const char c : printed) {
        if (c != '\r') {
            text += c;
        }
    }
    CHECK(text == "FioriVoce: Could not safely open application data: busy.\n");
    CHECK(boxes == std::vector<std::string>{"Could not safely open application data: busy."});

    // Without a box (Qt cannot start), the stderr line still stands.
    const std::string alone =
        stderrOf(root.path() / "alone.txt", [] { fv::showStartupError("No box.", {}); });
    CHECK(alone.starts_with("FioriVoce: No box."));
}
