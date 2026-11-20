#include "focus/foreground.h"

#include <windows.h>

#include <wil/resource.h>

#include "focus/detail/platform.h"
#include "focus/detail/wide.h"
#include "focus/uia_text_reader.h"
#include "pyport/text.h"

namespace fv::focus {

namespace {

constexpr std::wstring_view kUwpCoreWindow = L"Windows.UI.Core.CoreWindow";

HWND as_window(std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

std::wstring wide_class_name(HWND window) {
    wchar_t buffer[256];
    const int length = GetClassNameW(window, buffer, 256);
    return length > 0 ? std::wstring(buffer, static_cast<std::size_t>(length)) : std::wstring();
}

std::uint32_t pid_of(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid;
}

class RealWin32Desktop final : public Win32Desktop {
public:
    std::uintptr_t foreground_window() override {
        return reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    }

    std::uint32_t window_pid(std::uintptr_t window) override { return pid_of(as_window(window)); }

    std::string class_name(std::uintptr_t window) override {
        return detail::narrow(wide_class_name(as_window(window)));
    }

    std::string title(std::uintptr_t window) override {
        wchar_t buffer[WINDOW_TITLE_CHARS + 1];
        const int length = GetWindowTextW(as_window(window), buffer, WINDOW_TITLE_CHARS + 1);
        return length > 0 ? detail::narrow(std::wstring_view(buffer, length)) : std::string();
    }

    std::string image_name(std::uint32_t pid) override {
        if (pid == 0) {
            return "";
        }
        wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process) {
            return "";
        }
        std::wstring buffer(32768, L'\0');
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size)) {
            return "";
        }
        buffer.resize(size);
        // ntpath.basename: either separator ends the folder part.
        const auto cut = buffer.find_last_of(L"\\/");
        const std::wstring base = cut == std::wstring::npos ? buffer : buffer.substr(cut + 1);
        return py::lower(detail::narrow(base));
    }

    std::optional<std::pair<std::uintptr_t, std::uint32_t>>
    uwp_app_window(std::uintptr_t frame, std::uint32_t frame_pid) override {
        struct Search {
            std::uint32_t frame_pid;
            std::optional<std::pair<std::uintptr_t, std::uint32_t>> found;
        } search{frame_pid, std::nullopt};
        EnumChildWindows(
            as_window(frame),
            [](HWND child, LPARAM param) -> BOOL {
                auto& state = *reinterpret_cast<Search*>(param);
                if (wide_class_name(child) == kUwpCoreWindow) {
                    const std::uint32_t pid = pid_of(child);
                    if (pid != 0 && pid != state.frame_pid) {
                        state.found.emplace(reinterpret_cast<std::uintptr_t>(child), pid);
                        return FALSE;
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
};

class WindowsPlatform final : public FocusPlatform {
public:
    std::string_view name() const override { return "windows"; }
    bool sync_identity() const override { return true; }
    bool text_supported() const override { return true; }
    std::optional<AppIdentity> identity() override {
        return foreground_identity(desktop_, GetCurrentProcessId());
    }
    std::unique_ptr<TextReader> text_reader() override { return std::make_unique<UiaTextReader>(); }

private:
    RealWin32Desktop desktop_;
};

} // namespace

std::unique_ptr<Win32Desktop> make_win32_desktop() {
    return std::make_unique<RealWin32Desktop>();
}

std::optional<AppIdentity> foreground_identity() {
    RealWin32Desktop desktop;
    return foreground_identity(desktop, GetCurrentProcessId());
}

namespace detail {

std::shared_ptr<FocusPlatform> make_native_platform() {
    return std::make_shared<WindowsPlatform>();
}

bool native_text_supported() {
    return true;
}

} // namespace detail

} // namespace fv::focus
