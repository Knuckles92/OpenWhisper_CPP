#include "engine/platform.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace fv {

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

#ifdef _WIN32

namespace {

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                        nullptr, nullptr);
    return text;
}

} // namespace

std::optional<std::string> env_utf8(const char* name) {
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    value.resize(written);
    if (value.empty()) {
        return std::nullopt;
    }
    return narrow(value);
}

std::vector<std::string> utf8_args(int, char**) {
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return args;
    }
    for (int i = 0; i < count; ++i) {
        args.push_back(narrow(wide[i]));
    }
    LocalFree(wide);
    return args;
}

void use_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
}

void add_dll_directory(const std::filesystem::path& folder) {
    // A folder already added is added again harmlessly; a failure leaves the library's own
    // folder search (SharedLibrary) to find what it can.
    AddDllDirectory(std::filesystem::absolute(folder).make_preferred().c_str());
}

#else

std::optional<std::string> env_utf8(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::vector<std::string> utf8_args(int argc, char** argv) {
    return std::vector<std::string>(argv, argv + argc);
}

void use_utf8_console() {}

void add_dll_directory(const std::filesystem::path&) {}

#endif

} // namespace fv
