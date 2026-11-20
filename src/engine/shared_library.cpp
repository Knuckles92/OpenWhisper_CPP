#include "engine/shared_library.h"

#include <string>
#include <utility>

#include "engine/error.h"
#include "engine/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fv {

namespace {

#ifdef _WIN32
std::string last_error_text() {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message(buffer ? buffer : L"", length);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ' || message.back() == L'.')) {
        message.pop_back();
    }
    std::string text = path_to_utf8(std::filesystem::path(message));
    return text.empty() ? "error " + std::to_string(code) : text;
}
#endif

} // namespace

SharedLibrary::SharedLibrary(const std::filesystem::path& path)
    : path_(std::filesystem::absolute(path).make_preferred()) {
#ifdef _WIN32
    // Search the library's own folder for its dependencies (the runtime ships
    // ggml and, for the GPU build, CUDA beside it), then the app folder and
    // System32. Never the working directory or PATH.
    //
    // Windows otherwise answers a damaged or wrong-architecture DLL with a
    // "Bad Image" dialog of its own and blocks until it is dismissed; the
    // error mode makes the load simply fail, and the caller reports it.
    DWORD previous_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_mode);
    handle_ = LoadLibraryExW(path_.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const std::string reason = handle_ == nullptr ? last_error_text() : std::string();
    SetThreadErrorMode(previous_mode, nullptr);
    if (handle_ == nullptr) {
        throw EngineError("Could not load " + path_to_utf8(path_) + ": " + reason + ".");
    }
#else
    handle_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char* reason = dlerror();
        throw EngineError("Could not load " + path_to_utf8(path_) + ": " +
                          (reason ? reason : "unknown error") + ".");
    }
#endif
}

SharedLibrary::~SharedLibrary() {
    close();
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void* SharedLibrary::symbol(const char* name) const {
    if (handle_ == nullptr) {
        return nullptr;
    }
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

void SharedLibrary::close() {
    if (handle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

} // namespace fv
