#include "process/win/explorer_open.h"

#include <exdisp.h>
#include <servprov.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <array>
#include <format>

#include "base/log.h"

namespace fv::process_win {

namespace {

constexpr log::Logger logger("services.actions.launch");

void check(HRESULT hr) {
    if (FAILED(hr)) {
        throw ComError(hr);
    }
}

DISPID dispatch_id(IDispatch& object, const wchar_t* name) {
    DISPID id = 0;
    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    check(object.GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id));
    return id;
}

VARIANT bstr_variant(std::string_view text) {
    VARIANT value;
    VariantInit(&value);
    const std::wstring wide = widen(text);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
    if (value.bstrVal == nullptr) {
        throw ComError(E_OUTOFMEMORY);
    }
    return value;
}

} // namespace

ComError::ComError(HRESULT hr)
    : std::runtime_error(std::format("COM call failed ({:#x})", static_cast<unsigned long>(hr))),
      hr_(hr) {}

std::optional<DesktopApplication> desktop_application() {
    wil::com_ptr_nothrow<IShellWindows> windows;
    check(
        CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)));
    VARIANT location;
    VariantInit(&location);
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT empty;
    VariantInit(&empty);
    long window = 0;
    wil::com_ptr_nothrow<IDispatch> desktop;
    const HRESULT found =
        windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop);
    check(found);
    if (found == S_FALSE || !desktop) {
        return std::nullopt;
    }
    wil::com_ptr_nothrow<IServiceProvider> provider;
    check(desktop->QueryInterface(IID_PPV_ARGS(&provider)));
    wil::com_ptr_nothrow<IShellBrowser> browser;
    check(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)));
    wil::com_ptr_nothrow<IShellView> view;
    check(browser->QueryActiveShellView(&view));
    wil::com_ptr_nothrow<IDispatch> background;
    check(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background)));
    DISPPARAMS none{};
    VARIANT result;
    VariantInit(&result);
    check(background->Invoke(dispatch_id(*background, L"Application"), IID_NULL,
                             LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &none, &result, nullptr,
                             nullptr));
    if (result.vt != VT_DISPATCH || result.pdispVal == nullptr) {
        VariantClear(&result);
        throw ComError(E_NOINTERFACE);
    }
    DesktopApplication application;
    application.application.attach(result.pdispVal);
    application.window = reinterpret_cast<HWND>(static_cast<LONG_PTR>(window));
    return application;
}

void allow_foreground(HWND window) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(window, &pid) != 0) {
        AllowSetForegroundWindow(pid);
    }
}

void invoke_shell_execute(IDispatch& application, std::string_view file, std::string_view args) {
    const DISPID id = dispatch_id(application, L"ShellExecute");
    // DISPPARAMS hold the arguments last first: File, vArgs, vDir, vOperation, vShow.
    std::array<VARIANT, 5> values{};
    for (auto& value : values) {
        VariantInit(&value);
    }
    HRESULT hr = S_OK;
    try {
        values[4] = bstr_variant(file);
        values[3] = bstr_variant(args);
        values[2] = bstr_variant("");
        values[1] = bstr_variant(""); // the default verb, as ShellExecute without one
        values[0].vt = VT_I4;
        values[0].lVal = SW_SHOWNORMAL;
        DISPPARAMS parameters{values.data(), nullptr, static_cast<UINT>(values.size()), 0};
        hr = application.Invoke(id, IID_NULL, 0, DISPATCH_METHOD, &parameters, nullptr, nullptr,
                                nullptr);
    } catch (...) {
        for (auto& value : values) {
            VariantClear(&value);
        }
        throw;
    }
    for (auto& value : values) {
        VariantClear(&value);
    }
    check(hr);
}

bool explorer_open(std::string_view file, std::string_view args,
                   const std::function<std::optional<DesktopApplication>()>& find_desktop,
                   const std::function<void(HWND)>& allow) {
    try {
        const auto desktop = find_desktop();
        if (!desktop || !desktop->application) {
            return false;
        }
        allow(desktop->window);
        invoke_shell_execute(*desktop->application, file, args);
    } catch (const ComError& error) {
        logger.debug("Explorer couldn't be asked to open a link ({:#x})",
                     static_cast<unsigned long>(error.hr()));
        return false;
    }
    return true;
}

bool shell_execute_here(std::string_view file, std::string_view args) {
    const std::wstring wide_file = widen(file);
    const std::wstring wide_args = widen(args);
    const HINSTANCE result =
        ShellExecuteW(nullptr, nullptr, wide_file.c_str(),
                      wide_args.empty() ? nullptr : wide_args.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool reveal_in_folder(const std::filesystem::path& path) {
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (item == nullptr) {
        return false;
    }
    const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
    return SUCCEEDED(hr);
}

} // namespace fv::process_win
