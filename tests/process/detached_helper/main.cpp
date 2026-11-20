// Writes "<working folder>\n<argument>\n..." (every argument after the first) to the file the
// first argument names, through a temporary file renamed into place, so a reader never sees half
// of it. The text is UTF-8 everywhere: on Windows the arguments are read as UTF-16 (wmain), since
// the narrow argv holds the ANSI code page, which cannot spell every folder name.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::filesystem::path& text) {
    const std::u8string bytes = text.u8string();
    return {bytes.begin(), bytes.end()};
}

// `args` holds every argument after the program, each as a path only to carry its encoding.
int run(const std::vector<std::filesystem::path>& args) {
    if (args.empty()) {
        return 2;
    }
    const std::filesystem::path& target = args.front();
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << utf8(std::filesystem::current_path()) << '\n';
        for (std::size_t i = 1; i < args.size(); ++i) {
            out << utf8(args[i]) << '\n';
        }
        if (!out) {
            return 1;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return error ? 1 : 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run(std::vector<std::filesystem::path>(argv + (argc > 0 ? 1 : 0), argv + argc));
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::filesystem::path>(argv + (argc > 0 ? 1 : 0), argv + argc));
}
#endif
