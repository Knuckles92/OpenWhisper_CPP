#include "diarize/detail/ort_loader.h"

#include <string>

#include "base/utf8.h"

#include <windows.h>

namespace fv::diarize::detail {

namespace {

std::string last_error_text(DWORD code) {
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

} // namespace

std::expected<void*, std::string> load_ort_get_api_base(const std::filesystem::path& absolute) {
    // Dependencies come from the runtime's own folder and System32 only: never the working
    // directory or PATH, where another onnxruntime.dll (System32 has an older one) could win.
    // The error mode stops Windows from answering a damaged or wrong-architecture DLL with a
    // "Bad Image" dialog of its own; the load simply fails and the caller reports it.
    DWORD previous_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_mode);
    HMODULE module = LoadLibraryExW(
        absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD code = module == nullptr ? GetLastError() : 0;
    SetThreadErrorMode(previous_mode, nullptr);
    if (module == nullptr) {
        return std::unexpected("Could not load ONNX Runtime from " + path_to_utf8(absolute) + ": " +
                               last_error_text(code) + ".");
    }
    // Never freed: a loaded runtime stays loaded until the process ends.
    FARPROC symbol = GetProcAddress(module, "OrtGetApiBase");
    if (symbol == nullptr) {
        return std::unexpected(path_to_utf8(absolute) +
                               " is not ONNX Runtime: it has no OrtGetApiBase.");
    }
    return reinterpret_cast<void*>(symbol);
}

} // namespace fv::diarize::detail
