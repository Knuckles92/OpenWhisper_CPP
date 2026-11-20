// Compiled into every test executable by fv_test (FioriVoceModules.cmake). Before main() it
// points the per-user folders of this process at a private temporary folder, and removes the
// variables that lead to the user's real data, installed tools and API keys. A test that
// resolves fv::Paths from the process environment, or starts a child process, then cannot reach
// the real %LOCALAPPDATA%\FioriVoce or OpenWhisper folders by accident (lead amendment 25).
//
// It does not cover an environment a test pins itself (fv::MapEnvReader never reads the
// process), the credential store or the registry: those are injected or sandboxed by the test.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace fv::testkit {
// A name for the linker to keep; nothing reads it.
extern const int private_home_guard;
const int private_home_guard = 1;
} // namespace fv::testkit

namespace {

namespace fs = std::filesystem;

// Set by the first guarded process to the private home, so that a child that is itself a test
// executable (fv_recorder_tests starts itself) shares it and leaves its removal to the parent.
constexpr const char* kMarker = "FV_TEST_HOME";

#ifdef _WIN32

std::wstring wide(const char* ascii) {
    return std::wstring(ascii, ascii + std::char_traits<char>::length(ascii));
}

std::optional<fs::path> read(const char* name) {
    wchar_t* raw = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&raw, &size, wide(name).c_str()) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    fs::path value(raw);
    std::free(raw);
    return value.empty() ? std::nullopt : std::optional<fs::path>(std::move(value));
}

// _wputenv_s changes the C runtime's copy and the Win32 block: code under test reads both.
void write(const char* name, const fs::path& value) {
    _wputenv_s(wide(name).c_str(), value.c_str());
}

// An empty value removes the variable.
void erase(const char* name) {
    _wputenv_s(wide(name).c_str(), L"");
}

int process_id() {
    return _getpid();
}

#else

std::optional<fs::path> read(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<fs::path>(value);
}

void write(const char* name, const fs::path& value) {
    ::setenv(name, value.c_str(), 1);
}

void erase(const char* name) {
    ::unsetenv(name);
}

int process_id() {
    return static_cast<int>(::getpid());
}

#endif

// Removed by exact name, never by prefix: FIORIVOCE_DATA_DIR is CTest's folder for the test
// case, and the FIORIVOCE_TEST_* variables name what a test was explicitly given.
constexpr std::array kRemoved = {
    // Another data folder, speech runtime or model than the test builds for itself.
    "OPENWHISPER_DATA_DIR",
    "FIORIVOCE_RUNTIME_DIR",
    "FIORIVOCE_MODEL",
    "FIORIVOCE_SPEAKER_MODEL",
    "OPENWHISPER_SPEAKER_MODEL",
    "FIORIVOCE_ONNXRUNTIME",
    // Settings of the user's session that change what the app does.
    "FIORIVOCE_UI",
    "FIORIVOCE_UPDATE_FEED_URL",
    "OMARCHY_PATH",
    // Where the user's coding agents keep their sign-in and configuration.
    "CLAUDE_CONFIG_DIR",
    "CODEX_HOME",
    "NPM_CONFIG_PREFIX",
    // The user's keys and tokens: no test may spend credit or act as the user.
    "OPENAI_API_KEY",
    "OPENROUTER_API_KEY",
    "GROQ_API_KEY",
    "OPENCODE_GO_API_KEY",
    "OPENCODE_ZEN_API_KEY",
    "LMSTUDIO_API_KEY",
    "TYPESAFE_API_KEY",
    "ANTHROPIC_API_KEY",
    "HF_TOKEN",
    "HUGGING_FACE_HUB_TOKEN",
    "FIORIVOCE_API_TOKEN",
    "FIORIVOCE_MCP_TOKEN",
    "FIORIVOCE_REMOTE_ENGINE_TOKEN",
    "FIORIVOCE_TOKEN",
    "OPENWHISPER_API_TOKEN",
    "OPENWHISPER_MCP_TOKEN",
    "OPENWHISPER_REMOTE_ENGINE_TOKEN",
    "OPENWHISPER_TOKEN",
#ifdef _WIN32
    // ntpath.expanduser's second choice after USERPROFILE.
    "HOMEDRIVE",
    "HOMEPATH",
#else
    // Read before HOME by the agent lookup on every platform.
    "USERPROFILE",
#endif
};

struct PrivateHome {
    PrivateHome() {
        if (read(kMarker)) {
            return;
        }
        std::error_code error;
        const fs::path temp = fs::temp_directory_path(error);
        if (error || temp.empty()) {
            std::fputs("fv_test: no temporary folder for the private per-user folders\n", stderr);
            std::fflush(stderr);
            std::_Exit(2);
        }
        home = temp / ("fv-test-home-" + std::to_string(process_id()));
        // Left by an earlier process of the same id that did not exit normally.
        fs::remove_all(home, error);

        // A compiled-kernel cache and the desktop's caches are the machine's, not the app's:
        // moved with the home they would be rebuilt by every test case.
#ifdef _WIN32
        if (const auto roaming = read("APPDATA"); roaming && !read("CUDA_CACHE_PATH")) {
            write("CUDA_CACHE_PATH", *roaming / "NVIDIA" / "ComputeCache");
        }
        write("LOCALAPPDATA", home / "Local");
        write("APPDATA", home / "Roaming");
        write("USERPROFILE", home);
#else
        if (const auto real = read("HOME")) {
            if (!read("CUDA_CACHE_PATH")) {
                write("CUDA_CACHE_PATH", *real / ".nv" / "ComputeCache");
            }
            if (!read("XDG_CACHE_HOME")) {
                write("XDG_CACHE_HOME", *real / ".cache");
            }
        }
        // Tests written on Windows set these two; they are harmless elsewhere.
        write("LOCALAPPDATA", home / ".local" / "share");
        write("APPDATA", home / ".config");
#endif
        write("HOME", home);
        write("XDG_DATA_HOME", home / ".local" / "share");
        write("XDG_CONFIG_HOME", home / ".config");
        write("XDG_STATE_HOME", home / ".local" / "state");
        for (const char* name : kRemoved) {
            erase(name);
        }
        write(kMarker, home);
        owner = true;
    }

    ~PrivateHome() {
        if (owner) {
            std::error_code ignored;
            fs::remove_all(home, ignored);
        }
    }

    PrivateHome(const PrivateHome&) = delete;
    PrivateHome& operator=(const PrivateHome&) = delete;

    fs::path home;
    bool owner = false;
};

const PrivateHome private_home;

} // namespace
