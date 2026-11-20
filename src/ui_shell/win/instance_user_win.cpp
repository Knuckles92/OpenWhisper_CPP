#include "ui_shell/single_instance.h"

#include <windows.h>

#include <string>

#include <QString>

#include "appkit/qt_bridge.h"
#include "base/hash.h"

namespace fv {

// Windows has no uid: the account name stands in. Pipe names are one namespace for the whole
// machine, so without it two accounts on one data folder would share a server name. Hashed,
// because an account name may hold characters a pipe or file name should not carry.
std::string instanceUserPart() {
    wchar_t name[257] = {};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (GetUserNameW(name, &size) == 0) {
        return "0";
    }
    return sha256_hex(qt::toUtf8(QString::fromWCharArray(name).toLower())).substr(0, 8);
}

} // namespace fv
