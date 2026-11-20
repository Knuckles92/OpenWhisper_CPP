#include "update_native/journal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

#include "base/ids.h"
#include "base/utf8.h"
#include "update/update_contract.h"
#include "update_native/detail/file_ops.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;
constexpr std::uintmax_t kMaxJournalBytes = 4U << 20;
constexpr std::string_view kRunOnce = "software\\microsoft\\windows\\currentversion\\runonce";
constexpr std::string_view kTestPrefix = "software\\openwhisper-cpptest\\";

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    return out;
}
bool lower_hex(std::string_view value, std::size_t length) {
    return value.size() == length && std::ranges::all_of(value, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
bool valid_guid(std::string_view id) {
    if (id.size() != 38 || id.front() != '{' || id.back() != '}')
        return false;
    for (std::size_t i = 1; i < 37; ++i) {
        const bool separator = i == 9 || i == 14 || i == 19 || i == 24;
        const char ch = id[i];
        if (separator ? ch != '-'
                      : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                          (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}
std::string token_of(std::string_view raw) {
    try {
        return update::validate_transaction_id(raw);
    } catch (const std::exception&) {
        throw UpdateError("Invalid update transaction id.");
    }
}
fs::path absolute_path(const fs::path& path, std::string_view reason) {
    if (path.empty() || !path.is_absolute() || path == path.root_path())
        throw UpdateError(std::string(reason));
    return path.lexically_normal();
}
bool same_path(const fs::path& left, const fs::path& right) {
    if (left.empty() || !left.is_absolute())
        return false;
    const auto a = path_to_utf8(left.lexically_normal());
    const auto b = path_to_utf8(right.lexically_normal());
#ifdef _WIN32
    return lower_ascii(a) == lower_ascii(b);
#else
    return a == b;
#endif
}
// Refuse links in each existing ancestor before reading or writing. A missing tail is allowed
// only so callers can validate a prepared candidate or rollback tree before it exists.
void check_chain(const fs::path& path) {
    for (fs::path current = path; !current.empty(); current = current.parent_path()) {
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw UpdateError("Could not inspect an update journal path.");
        if (!error && fs::exists(status)) {
            try {
                if (detail::unsafe_node(current))
                    throw UpdateError("An update journal path is a link.");
            } catch (const fs::filesystem_error&) {
                throw UpdateError("Could not inspect an update journal path.");
            }
        }
        if (current == current.root_path())
            break;
    }
}
bool valid_registry_key(std::string_view key) {
    if (key.empty() || key.front() == '\\' || key.back() == '\\')
        return false;
    std::size_t start = 0;
    while (start < key.size()) {
        const auto end = key.find('\\', start);
        const auto component =
            key.substr(start, end == std::string_view::npos ? key.size() - start : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (std::ranges::any_of(component, [](unsigned char ch) {
                return ch < 32 || ch == 127 || ch == '/' || ch == ':';
            }))
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return lower_ascii(key).starts_with("software\\") && key.size() > 9;
}
bool test_key(std::string_view key) {
    const auto folded = lower_ascii(key);
    return valid_registry_key(key) && folded.starts_with(kTestPrefix) &&
           folded.size() > kTestPrefix.size();
}
bool uninstaller(std::string_view name) {
    const auto value = lower_ascii(name);
    if (!value.starts_with("unins"))
        return false;
    std::size_t i = 5;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        ++i;
    return i > 5 &&
           (value.substr(i) == ".exe" || value.substr(i) == ".dat" || value.substr(i) == ".msg");
}
std::string text_field(const Json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_string())
        throw UpdateError("The update journal is invalid.");
    return object.at(key).get<std::string>();
}
std::int64_t integer_field(const Json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_number_integer())
        throw UpdateError("The update journal is invalid.");
    if (object.at(key).is_number_unsigned() &&
        object.at(key).get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw UpdateError("The update journal is invalid.");
    try {
        return object.at(key).get<std::int64_t>();
    } catch (const UpdateError&) {
        throw;
    } catch (const std::exception&) {
        throw UpdateError("The update journal is invalid.");
    }
}
Json sorted_json(const Json& value) {
    if (value.is_array()) {
        Json result = Json::array();
        for (const auto& item : value)
            result.push_back(sorted_json(item));
        return result;
    }
    if (!value.is_object())
        return value;
    std::vector<std::string> keys;
    for (auto it = value.begin(); it != value.end(); ++it)
        keys.push_back(it.key());
    std::ranges::sort(keys);
    Json result = Json::object();
    for (const auto& key : keys)
        result[key] = sorted_json(value.at(key));
    return result;
}
} // namespace

std::string_view to_string(TransactionState state) {
    switch (state) {
    case TransactionState::Preparing:
        return "preparing";
    case TransactionState::Prepared:
        return "prepared";
    case TransactionState::OldMoved:
        return "old_moved";
    case TransactionState::NewActive:
        return "new_active";
    case TransactionState::Healthy:
        return "healthy";
    case TransactionState::RolledBack:
        return "rolled_back";
    case TransactionState::Superseded:
        return "superseded";
    }
    throw UpdateError("The update journal has an invalid state.");
}
TransactionState parse_transaction_state(std::string_view state) {
    for (const auto candidate :
         {TransactionState::Preparing, TransactionState::Prepared, TransactionState::OldMoved,
          TransactionState::NewActive, TransactionState::Healthy, TransactionState::RolledBack,
          TransactionState::Superseded})
        if (to_string(candidate) == state)
            return candidate;
    throw UpdateError("The update journal has an invalid state.");
}
JournalPaths journal_paths(const JournalContext& context, std::string_view transaction_id) {
    const std::string id = token_of(transaction_id);
    JournalPaths paths;
    paths.data_root = absolute_path(context.data_root, "The trusted update data root is invalid.");
    paths.install_dir =
        absolute_path(context.install_dir, "The trusted installation path is invalid.");
    if (paths.install_dir.filename().empty())
        throw UpdateError("The trusted installation path is invalid.");
    paths.updates_root = paths.data_root / update::kUpdatesDirname;
    paths.transaction_dir = paths.updates_root / update::kTransactionsDirname / id;
    paths.journal_file = paths.transaction_dir / update::kJournalName;
    const auto name = path_to_utf8(paths.install_dir.filename());
    paths.candidate_dir =
        paths.install_dir.parent_path() / path_from_utf8(name + ".new-" + id.substr(0, 8));
    paths.rollback_dir =
        paths.install_dir.parent_path() / path_from_utf8(name + ".old-" + id.substr(0, 8));
    paths.failed_dir =
        paths.install_dir.parent_path() / path_from_utf8(name + ".failed-" + id.substr(0, 8));
    return paths;
}
UpdateJournal make_journal(const JournalContext& context, std::string_view transaction_id) {
    const auto paths = journal_paths(context, transaction_id);
    UpdateJournal journal;
    journal.transaction_id = token_of(transaction_id);
    journal.app_dir = paths.install_dir;
    journal.candidate_dir = paths.candidate_dir;
    journal.rollback_dir = paths.rollback_dir;
    journal.appdata = paths.data_root;
    journal.health_token = uuid4_hex();
    journal.app_id = context.app_id;
    if (context.expected_registration)
        journal.registration = *context.expected_registration;
    journal.runonce_key = context.expected_runonce_key;
    journal.registration.install_location = paths.install_dir;
    return journal;
}
Json to_json(const UpdateJournal& j) {
    Json registration = {
        {"hive", j.registration.hive == update::RegistryHive::CurrentUser ? "HKCU" : "HKLM"},
        {"key_path", j.registration.key_path},
        {"view", j.registration.view}};
    Json result = Json::object();
    result["app_dir"] = path_to_utf8(j.app_dir);
    result["app_id"] = j.app_id;
    result["appdata"] = path_to_utf8(j.appdata);
    result["archive_path"] = path_to_utf8(j.archive_path);
    result["candidate_dir"] = path_to_utf8(j.candidate_dir);
    result["cleanup_rollback"] = j.cleanup_rollback;
    result["compatibility_files"] = sorted_json(j.compatibility_files);
    result["health_token"] = j.health_token;
    result["new_exe_sha256"] = j.new_exe_sha256;
    result["new_version"] = j.new_version;
    result["old_display_version"] = j.old_display_version;
    result["old_estimated_size_kb"] =
        j.old_estimated_size_kb ? Json(*j.old_estimated_size_kb) : Json(nullptr);
    result["old_exe_sha256"] = j.old_exe_sha256;
    result["old_install_date"] = j.old_install_date;
    if (!j.old_manifest_sha256.empty())
        result["old_manifest_sha256"] = j.old_manifest_sha256;
    result["old_version"] = j.old_version;
    result["parent_creation_time"] = j.parent_creation_time;
    result["parent_exe"] = path_to_utf8(j.parent_exe);
    result["parent_pid"] = j.parent_pid;
    result["registration"] = std::move(registration);
    result["rollback_dir"] = path_to_utf8(j.rollback_dir);
    result["runonce_key"] = j.runonce_key;
    result["schema_version"] = update::kManifestSchemaVersion;
    result["state"] = std::string(to_string(j.state));
    result["transaction_id"] = j.transaction_id;
    result["uninstaller_files"] = j.uninstaller_files;
    return result;
}
UpdateJournal journal_from_json(const Json& data) {
    if (!data.is_object())
        throw UpdateError("The update journal is not an object.");
    constexpr std::array required{"schema_version",
                                  "transaction_id",
                                  "state",
                                  "app_dir",
                                  "candidate_dir",
                                  "rollback_dir",
                                  "old_version",
                                  "new_version",
                                  "old_display_version",
                                  "old_estimated_size_kb",
                                  "old_install_date",
                                  "old_exe_sha256",
                                  "new_exe_sha256",
                                  "parent_pid",
                                  "parent_exe",
                                  "parent_creation_time",
                                  "health_token",
                                  "compatibility_files",
                                  "appdata",
                                  "archive_path",
                                  "cleanup_rollback",
                                  "registration",
                                  "runonce_key",
                                  "app_id"};
    std::string missing;
    for (const auto* key : required)
        if (!data.contains(key)) {
            if (!missing.empty())
                missing += ", ";
            missing += key;
        }
    if (!missing.empty())
        throw UpdateError("The update journal is incomplete: " + missing);
    try {
        if (integer_field(data, "schema_version") != update::kManifestSchemaVersion)
            throw UpdateError("The update journal has an invalid schema.");
        UpdateJournal j;
        j.transaction_id = token_of(text_field(data, "transaction_id"));
        j.state = parse_transaction_state(text_field(data, "state"));
        j.app_dir = path_from_utf8(text_field(data, "app_dir"));
        j.candidate_dir = path_from_utf8(text_field(data, "candidate_dir"));
        j.rollback_dir = path_from_utf8(text_field(data, "rollback_dir"));
        j.old_version = text_field(data, "old_version");
        if (data.contains("old_manifest_sha256"))
            j.old_manifest_sha256 = text_field(data, "old_manifest_sha256");
        j.new_version = text_field(data, "new_version");
        j.old_display_version = text_field(data, "old_display_version");
        if (!data.at("old_estimated_size_kb").is_null())
            j.old_estimated_size_kb = integer_field(data, "old_estimated_size_kb");
        j.old_install_date = text_field(data, "old_install_date");
        j.old_exe_sha256 = text_field(data, "old_exe_sha256");
        j.new_exe_sha256 = text_field(data, "new_exe_sha256");
        const auto pid = integer_field(data, "parent_pid");
        if (pid < 0 || static_cast<std::uint64_t>(pid) > std::numeric_limits<std::uint32_t>::max())
            throw UpdateError("The update journal has an invalid parent process.");
        j.parent_pid = static_cast<std::uint32_t>(pid);
        j.parent_exe = path_from_utf8(text_field(data, "parent_exe"));
        j.parent_creation_time = integer_field(data, "parent_creation_time");
        j.health_token = text_field(data, "health_token");
        if (data.contains("uninstaller_files"))
            j.uninstaller_files = data.at("uninstaller_files").get<std::vector<std::string>>();
        if (!data.at("compatibility_files").is_object())
            throw UpdateError("The update journal has invalid compatibility metadata.");
        j.compatibility_files = data.at("compatibility_files");
        j.appdata = path_from_utf8(text_field(data, "appdata"));
        j.archive_path = path_from_utf8(text_field(data, "archive_path"));
        if (!data.at("cleanup_rollback").is_boolean())
            throw UpdateError("The update journal is invalid.");
        j.cleanup_rollback = data.at("cleanup_rollback").get<bool>();
        const Json& registration = data.at("registration");
        if (!registration.is_object())
            throw UpdateError("The update journal has an invalid registration.");
        const auto hive = text_field(registration, "hive");
        if (hive == "HKCU")
            j.registration.hive = update::RegistryHive::CurrentUser;
        else if (hive == "HKLM")
            j.registration.hive = update::RegistryHive::LocalMachine;
        else
            throw UpdateError("The update journal has an invalid registration.");
        j.registration.key_path = text_field(registration, "key_path");
        const auto view = integer_field(registration, "view");
        if (view != 0 && view != 0x100 && view != 0x200)
            throw UpdateError("The update journal has an invalid registration.");
        j.registration.view = static_cast<int>(view);
        j.registration.install_location = j.app_dir;
        j.runonce_key = text_field(data, "runonce_key");
        j.app_id = text_field(data, "app_id");
        return j;
    } catch (const Json::exception&) {
        throw UpdateError("The update journal is invalid.");
    }
}
bool is_test_journal(const UpdateJournal& journal) {
    return journal.registration.hive == update::RegistryHive::CurrentUser &&
           test_key(journal.registration.key_path);
}
JournalPaths validate_journal_binding(const JournalContext& context, const UpdateJournal& j,
                                      std::string_view transaction_id) {
    auto paths = journal_paths(context, transaction_id);
    if (token_of(j.transaction_id) != token_of(transaction_id))
        throw UpdateError("The update journal id does not match its directory.");
    (void)to_string(j.state);
    if (!same_path(j.appdata, paths.data_root))
        throw UpdateError("The update journal has an invalid data root.");
    if (!same_path(j.app_dir, paths.install_dir))
        throw UpdateError("The update journal has an invalid installation path.");
    if (!same_path(j.candidate_dir, paths.candidate_dir))
        throw UpdateError("The update journal has an invalid candidate path.");
    if (!same_path(j.rollback_dir, paths.rollback_dir))
        throw UpdateError("The update journal has an invalid rollback path.");
    if (!lower_hex(j.health_token, 32))
        throw UpdateError("The update journal has an invalid health token.");
    try {
        (void)update::parse_strict_version(j.old_display_version);
        if (!update::is_newer_version(j.old_version, j.new_version))
            throw UpdateError("The update journal has an invalid version transition.");
    } catch (const UpdateError&) {
        throw;
    } catch (const std::exception&) {
        throw UpdateError("The update journal has an invalid version transition.");
    }
    if (j.old_estimated_size_kb && *j.old_estimated_size_kb < 0)
        throw UpdateError("The update journal has an invalid installed size.");
    if (j.parent_pid == 0) {
        if (!j.parent_exe.empty() || j.parent_creation_time != 0)
            throw UpdateError("The update journal has an invalid parent process.");
    } else if (j.parent_creation_time <= 0 || context.parent_exe.empty() ||
               !context.parent_exe.is_absolute() ||
               !same_path(context.parent_exe.parent_path(), paths.install_dir) ||
               !same_path(j.parent_exe, context.parent_exe)) {
        throw UpdateError("The update journal has an invalid parent process.");
    } else {
        paths.parent_exe = context.parent_exe.lexically_normal();
    }
    for (const auto& hash : {j.old_exe_sha256, j.new_exe_sha256, j.old_manifest_sha256})
        if (!hash.empty() && !lower_hex(hash, 64))
            throw UpdateError("The update journal has an invalid file digest.");
    if (j.registration.hive != update::RegistryHive::CurrentUser ||
        !valid_registry_key(j.registration.key_path) ||
        (j.registration.view != 0 && j.registration.view != 0x100 && j.registration.view != 0x200))
        throw UpdateError("The update journal has an invalid registration.");
    if (!valid_registry_key(j.runonce_key) ||
        (lower_ascii(j.runonce_key) != kRunOnce && !test_key(j.runonce_key)))
        throw UpdateError("The update journal has an invalid RunOnce key.");
    if (!context.expected_registration ||
        context.expected_registration->hive != update::RegistryHive::CurrentUser ||
        !valid_registry_key(context.expected_registration->key_path) ||
        (!context.expected_registration->install_location.empty() &&
         !same_path(context.expected_registration->install_location, paths.install_dir)) ||
        context.expected_registration->view != j.registration.view ||
        lower_ascii(context.expected_registration->key_path) !=
            lower_ascii(j.registration.key_path) ||
        !valid_registry_key(context.expected_runonce_key) ||
        lower_ascii(context.expected_runonce_key) != lower_ascii(j.runonce_key))
        throw UpdateError("The update journal does not match the trusted registration.");
    if (!valid_guid(context.app_id) || !valid_guid(j.app_id) ||
        (!is_test_journal(j) && lower_ascii(j.app_id) != lower_ascii(context.app_id)))
        throw UpdateError("The update journal has an invalid application id.");
    std::set<std::string> files;
    if (j.uninstaller_files.size() > 1024 || j.compatibility_files.size() > 1024)
        throw UpdateError("The update journal has invalid compatibility metadata.");
    for (const auto& name : j.uninstaller_files) {
        if (!uninstaller(name) || !files.insert(lower_ascii(name)).second)
            throw UpdateError("The update journal has an invalid compatibility file.");
    }
    if (!j.compatibility_files.is_object())
        throw UpdateError("The update journal has invalid compatibility metadata.");
    std::set<std::string> metadata_names;
    for (auto it = j.compatibility_files.begin(); it != j.compatibility_files.end(); ++it) {
        const auto name = lower_ascii(it.key());
        if (!files.contains(name) || !metadata_names.insert(name).second)
            throw UpdateError("The update journal has an unauthorized compatibility file.");
        const auto& value = it.value();
        if (!value.is_object() || !value.contains("size") ||
            !value.at("size").is_number_integer() || !value.contains("sha256") ||
            !value.at("sha256").is_string() ||
            !lower_hex(value.at("sha256").get<std::string>(), 64))
            throw UpdateError("The update journal has invalid compatibility metadata.");
        const auto size = integer_field(value, "size");
        if (size < 0)
            throw UpdateError("The update journal has invalid compatibility metadata.");
    }
    if (!j.archive_path.empty()) {
        if (!j.archive_path.is_absolute() ||
            !same_path(j.archive_path.parent_path(), paths.updates_root) ||
            j.archive_path.filename().empty())
            throw UpdateError("The update journal has an invalid archive path.");
        try {
            (void)member_name(path_to_utf8(j.archive_path.filename()));
        } catch (const std::exception&) {
            throw UpdateError("The update journal has an invalid archive path.");
        }
        paths.archive_file = paths.updates_root / j.archive_path.filename();
    }
    check_chain(paths.data_root);
    check_chain(paths.transaction_dir);
    check_chain(paths.install_dir);
    check_chain(paths.candidate_dir);
    check_chain(paths.rollback_dir);
    check_chain(paths.failed_dir);
    if (!paths.archive_file.empty())
        check_chain(paths.archive_file);
    if (!paths.parent_exe.empty())
        check_chain(paths.parent_exe);
    return paths;
}
void save_journal(const JournalContext& context, const UpdateJournal& journal) {
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    try {
        check_chain(paths.transaction_dir);
        if (!fs::is_directory(paths.transaction_dir))
            throw UpdateError("The update transaction was not found.");
        GuardedDirectory directory(paths.transaction_dir);
        directory.write_file_atomic(paths.journal_file.filename(), to_json(journal).dump(2));
        detail::flush_directory(paths.transaction_dir);
    } catch (const UpdateError&) {
        throw;
    } catch (const std::exception&) {
        throw UpdateError("The update journal could not be saved.");
    }
}
UpdateJournal load_journal(const JournalContext& context, std::string_view transaction_id) {
    const auto paths = journal_paths(context, transaction_id);
    std::string bytes;
    try {
        GuardedDirectory directory(paths.transaction_dir);
        bytes = directory.read_file(paths.journal_file.filename(), kMaxJournalBytes);
    } catch (const std::exception&) {
        throw UpdateError("The update journal could not be read safely.");
    }
    Json payload;
    try {
        payload = Json::parse(bytes);
    } catch (const Json::exception&) {
        throw UpdateError("The update journal is invalid.");
    }
    UpdateJournal j = journal_from_json(payload);
    const auto trusted = validate_journal_binding(context, j, transaction_id);
    // A later caller receives only the independently derived paths, never the strings read in.
    j.appdata = trusted.data_root;
    j.app_dir = trusted.install_dir;
    j.candidate_dir = trusted.candidate_dir;
    j.rollback_dir = trusted.rollback_dir;
    j.registration.install_location = trusted.install_dir;
    j.parent_exe = trusted.parent_exe;
    j.archive_path = trusted.archive_file;
    return j;
}
} // namespace fv::update_native
