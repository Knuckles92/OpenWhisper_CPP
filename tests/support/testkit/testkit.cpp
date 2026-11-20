#include "testkit/testkit.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fv::testkit {
namespace {

std::filesystem::path from_utf8(std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

#ifdef _WIN32
std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("testkit: invalid UTF-8 in an environment name or value");
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
    return wide;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}
#endif

std::string random_hex(std::size_t digits) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
    std::string text;
    for (std::size_t i = 0; i < digits; ++i) {
        text.push_back(kDigits[engine() & 0xF]);
    }
    return text;
}

} // namespace

std::filesystem::path source_root() {
    return from_utf8(FV_TESTKIT_SOURCE_DIR);
}

std::filesystem::path golden_path() {
    return source_root() / "tests" / "golden";
}

std::filesystem::path golden_path(std::string_view module) {
    return golden_path() / from_utf8(std::string(module) + ".json");
}

std::filesystem::path reference_path() {
    return source_root() / "tests" / "reference";
}

std::filesystem::path parity_reference_file() {
    return source_root() / "tools" / "parity" / "reference.json";
}

std::filesystem::path artifact_path() {
    const auto path = from_utf8(FV_TESTKIT_BINARY_DIR) / "test-artifacts";
    std::filesystem::create_directories(path);
    return path;
}

std::optional<std::string> get_env(std::string_view name) {
#ifdef _WIN32
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, widen(name).c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    return narrow(value);
#else
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

void set_env(std::string_view name, const std::optional<std::string>& value) {
#ifdef _WIN32
    // An empty value removes the variable, which is also how a set-but-empty one would read.
    if (_wputenv_s(widen(name).c_str(), value ? widen(*value).c_str() : L"") != 0) {
        throw std::runtime_error("testkit: cannot set " + std::string(name));
    }
#else
    const std::string key(name);
    const int result = value ? setenv(key.c_str(), value->c_str(), 1) : unsetenv(key.c_str());
    if (result != 0) {
        throw std::runtime_error("testkit: cannot set " + key);
    }
#endif
}

TempDataRoot::TempDataRoot() : previous_(get_env("OPENWHISPER_CPP_DATA_DIR")) {
    // The long form: on Windows %TEMP% may hold 8.3 short names (C:\Users\FIRSTN~1\...), and code
    // that resolves its folders (weakly_canonical, as Python's resolve()) gets the long ones, so
    // paths it stores would not sit under a data root spelled the short way.
    std::error_code error;
    auto base = std::filesystem::temp_directory_path();
    if (auto resolved = std::filesystem::weakly_canonical(base, error); !error) {
        base = std::move(resolved);
    }
    for (int attempt = 0; attempt < 16 && path_.empty(); ++attempt) {
        auto candidate = base / ("fv-test-" + random_hex(16));
        if (std::filesystem::create_directory(candidate)) {
            path_ = std::filesystem::absolute(candidate);
        }
    }
    if (path_.empty()) {
        throw std::runtime_error("TempDataRoot: cannot create a temporary data folder");
    }
    const auto utf8 = path_.u8string();
    set_env("OPENWHISPER_CPP_DATA_DIR", std::string(utf8.begin(), utf8.end()));
}

TempDataRoot::~TempDataRoot() {
    try {
        set_env("OPENWHISPER_CPP_DATA_DIR", previous_);
    } catch (const std::exception&) {
        // A destructor must not throw; the next test's fixture sets its own value anyway.
    }
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

const std::filesystem::path& TempDataRoot::path() const {
    return path_;
}

} // namespace fv::testkit
