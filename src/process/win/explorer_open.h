#pragma once

#include "process/win/wide.h"

#include <oaidl.h>
#include <wil/com.h>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "process/launch.h"

// ShellExecute inside Explorer (launch.py _explorer_open, _desktop_application,
// _allow_foreground). IShellWindows::FindWindowSW finds Explorer's desktop; its folder view's
// Application (IShellDispatch2) lives in Explorer, so its ShellExecute runs there and the app a
// link opens starts with Explorer's environment, the user's own.
namespace fv::process_win {

// A COM call failed (pythoncom.com_error).
class ComError : public std::runtime_error {
public:
    explicit ComError(HRESULT hr);
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

struct DesktopApplication {
    wil::com_ptr_nothrow<IDispatch> application;
    HWND window = nullptr;
};

// Explorer's Shell.Application for its desktop, and the desktop window; nullopt when there is no
// desktop. Throws ComError when Explorer couldn't be asked. Needs COM on the calling thread.
std::optional<DesktopApplication> desktop_application();

// Lets the Explorer that owns `window` bring the app it starts to the front; nothing when this
// process may not.
void allow_foreground(HWND window);

// Invokes the application's ShellExecute(file, args, "", "", SW_SHOWNORMAL) through IDispatch:
// the default verb. Throws ComError.
void invoke_shell_execute(IDispatch& application, std::string_view file, std::string_view args);

// The whole of _explorer_open over replaceable parts: false when the desktop can't be found or
// a COM call fails (logged at debug), true once Explorer has the link.
bool explorer_open(std::string_view file, std::string_view args,
                   const std::function<std::optional<DesktopApplication>()>& find_desktop,
                   const std::function<void(HWND)>& allow);

// ShellExecuteW in this process with the default verb (os.startfile); false when it failed.
bool shell_execute_here(std::string_view file, std::string_view args);

// SHOpenFolderAndSelectItems on `path`.
bool reveal_in_folder(const std::filesystem::path& path);

// AssocQueryStringW(ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN | ASSOCF_NOTRUNCATE) with a
// 2,048-character buffer.
AssocAnswer assoc_query(std::string_view scheme, AssocString what);

} // namespace fv::process_win
