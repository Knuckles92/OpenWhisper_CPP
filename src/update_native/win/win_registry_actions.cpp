#include "update_native/win_registry_actions.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "base/utf8.h"
#include "update/update_contract.h"
#include "update_native/manifest.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

namespace fv::update_native {
namespace {
struct Key {
    HKEY value = nullptr;
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    Key(Key&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~Key() {
        if (value)
            RegCloseKey(value);
    }
};
[[noreturn]] void fail(LSTATUS status, const char* message) {
    throw std::system_error(static_cast<int>(status), std::system_category(), message);
}
std::wstring wide(std::string_view value) {
    return path_from_utf8(value).native();
}
std::wstring read_string(HKEY key, const wchar_t* name) {
    DWORD type = 0, bytes = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS)
        fail(status, "Reading update registration");
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536 || bytes % sizeof(wchar_t))
        throw UpdateError("The install registration is invalid.");
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS)
        fail(status, "Reading update registration");
    return std::wstring(buffer.data());
}
void write_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    const auto size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status =
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), size);
    if (status != ERROR_SUCCESS)
        fail(status, "Writing update registration");
}
void delete_value(HKEY key, const wchar_t* name) {
    const auto status = RegDeleteValueW(key, name);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        fail(status, "Removing update registration value");
}
void flush(HKEY key) {
    const auto status = RegFlushKey(key);
    if (status != ERROR_SUCCESS)
        fail(status, "Flushing update registration");
}
bool same(std::wstring a, std::wstring b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
Key open_registration(const JournalContext& context, REGSAM rights) {
    if (!context.expected_registration ||
        context.expected_registration->hive != update::RegistryHive::CurrentUser)
        throw UpdateError("The trusted install registration is missing.");
    Key key;
    const auto& expected = *context.expected_registration;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(expected.key_path).c_str(), 0,
                                         rights | static_cast<REGSAM>(expected.view), &key.value);
    if (status != ERROR_SUCCESS)
        fail(status, "Opening update registration");
    const auto actual =
        std::filesystem::path(read_string(key.value, L"InstallLocation")).lexically_normal();
    if (!same(actual.native(), context.install_dir.lexically_normal().native()) ||
        read_string(key.value, L"UninstallString") != wide(expected.uninstall_string))
        throw UpdateError("The install registration has changed.");
    return key;
}
} // namespace
WinRegistryActions::WinRegistryActions(JournalContext context, std::string_view id)
    : context_(std::move(context)), paths_(journal_paths(context_, id)) {
    if (context_.expected_runonce_key.empty())
        throw UpdateError("The trusted recovery key is missing.");
    const auto updater = paths_.transaction_dir / L"openwhisper-cpp-updater.exe";
    command_ = L"\"" + updater.native() + L"\" --recover --transaction-id " + wide(id);
}
void WinRegistryActions::verify(const UpdateJournal& j) {
    (void)validate_journal_binding(context_, j, j.transaction_id);
    auto key = open_registration(context_, KEY_QUERY_VALUE);
    const auto version = read_string(key.value, L"DisplayVersion");
    if (version != wide(j.old_display_version) && version != wide(j.new_version))
        throw UpdateError("The install registration version has changed.");
}
void WinRegistryActions::install_recovery(const UpdateJournal& j) {
    (void)validate_journal_binding(context_, j, j.transaction_id);
    Key key;
    const LSTATUS status =
        RegCreateKeyExW(HKEY_CURRENT_USER, wide(context_.expected_runonce_key).c_str(), 0, nullptr,
                        0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key.value, nullptr);
    if (status != ERROR_SUCCESS)
        fail(status, "Opening update recovery key");
    const auto name = wide(update::runonce_value_name());
    DWORD type = 0;
    const auto existing =
        RegQueryValueExW(key.value, name.c_str(), nullptr, &type, nullptr, nullptr);
    if (existing == ERROR_SUCCESS && read_string(key.value, name.c_str()) != command_)
        throw UpdateError("Another update recovery entry already exists.");
    if (existing != ERROR_SUCCESS && existing != ERROR_FILE_NOT_FOUND)
        fail(existing, "Reading update recovery entry");
    write_string(key.value, name.c_str(), command_);
    flush(key.value);
}
void WinRegistryActions::clear_recovery(const UpdateJournal& j) {
    (void)validate_journal_binding(context_, j, j.transaction_id);
    Key key;
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(context_.expected_runonce_key).c_str(), 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, &key.value);
    if (status == ERROR_FILE_NOT_FOUND)
        return;
    if (status != ERROR_SUCCESS)
        fail(status, "Opening update recovery key");
    const auto value_name = wide(update::runonce_value_name());
    DWORD type = 0;
    status = RegQueryValueExW(key.value, value_name.c_str(), nullptr, &type, nullptr, nullptr);
    if (status == ERROR_FILE_NOT_FOUND)
        return;
    if (status != ERROR_SUCCESS)
        fail(status, "Reading update recovery entry");
    if (read_string(key.value, value_name.c_str()) != command_)
        throw UpdateError("The update recovery entry has changed.");
    status = RegDeleteValueW(key.value, value_name.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        fail(status, "Removing update recovery entry");
    flush(key.value);
}
void WinRegistryActions::write_version(const UpdateJournal& j, bool rollback) {
    (void)validate_journal_binding(context_, j, j.transaction_id);
    auto key = open_registration(context_, KEY_QUERY_VALUE | KEY_SET_VALUE);
    const auto current = read_string(key.value, L"DisplayVersion");
    if (current != wide(j.old_display_version) && current != wide(j.new_version))
        throw UpdateError("The install registration version has changed.");
    write_string(key.value, L"DisplayVersion",
                 wide(rollback ? j.old_display_version : j.new_version));
    if (rollback && j.old_estimated_size_kb && *j.old_estimated_size_kb >= 0 &&
        static_cast<std::uint64_t>(*j.old_estimated_size_kb) <= std::numeric_limits<DWORD>::max()) {
        const DWORD size = static_cast<DWORD>(*j.old_estimated_size_kb);
        const auto status = RegSetValueExW(key.value, L"EstimatedSize", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE*>(&size), sizeof(size));
        if (status != ERROR_SUCCESS)
            fail(status, "Restoring install size");
    } else if (rollback && !j.old_estimated_size_kb) {
        delete_value(key.value, L"EstimatedSize");
    } else if (!rollback) {
        std::uint64_t bytes = 0;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(paths_.install_dir)) {
            if (entry.is_regular_file()) {
                const auto size = entry.file_size();
                if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
                    throw UpdateError("The installed application size is invalid.");
                bytes += size;
            }
        }
        const DWORD size = static_cast<DWORD>(std::min<std::uint64_t>(
            std::max<std::uint64_t>(1, bytes / 1024), std::numeric_limits<DWORD>::max()));
        const auto status = RegSetValueExW(key.value, L"EstimatedSize", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE*>(&size), sizeof(size));
        if (status != ERROR_SUCCESS)
            fail(status, "Writing install size");
    } else {
        throw UpdateError("The previous install size is invalid.");
    }
    if (rollback) {
        if (j.old_install_date.empty())
            delete_value(key.value, L"InstallDate");
        else
            write_string(key.value, L"InstallDate", wide(j.old_install_date));
    } else {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        wchar_t date[9]{};
        swprintf_s(date, L"%04u%02u%02u", static_cast<unsigned>(now.wYear),
                   static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay));
        write_string(key.value, L"InstallDate", date);
    }
    flush(key.value);
}
void WinRegistryActions::update(const UpdateJournal& j) {
    write_version(j, false);
}
void WinRegistryActions::restore(const UpdateJournal& j) {
    write_version(j, true);
}
} // namespace fv::update_native
#endif
