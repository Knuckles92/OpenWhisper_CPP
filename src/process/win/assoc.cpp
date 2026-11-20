#include "process/win/explorer_open.h"

#include <shlwapi.h>

namespace fv::process_win {

// The lookup ShellExecute itself uses. A protocol's handler is a command line, or a COM object
// named by DelegateExecute: every packaged (Microsoft Store / MSIX) app's protocol, and Windows'
// own pages such as ms-settings:. ShellExecute prefers the COM object.
AssocAnswer assoc_query(std::string_view scheme, AssocString what) {
    DWORD size = 2048;
    std::wstring buffer(size, L'\0');
    const std::wstring wide = widen(scheme);
    const HRESULT hr = AssocQueryStringW(
        ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN | ASSOCF_NOTRUNCATE,
        static_cast<ASSOCSTR>(static_cast<int>(what)), wide.c_str(), nullptr, buffer.data(), &size);
    AssocAnswer answer;
    answer.result = static_cast<std::uint32_t>(hr);
    answer.value = narrow(buffer.c_str());
    return answer;
}

} // namespace fv::process_win
