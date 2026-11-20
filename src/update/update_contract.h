#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "pyport/errors.h"

// Shared names and version rules of application update artifacts (services/update_contract.py).
// Product-specific names come from the release identity and identity(); the transaction names
// are kept for the native updater (M10), which writes them.
namespace fv::update {

// How this process may apply a release. A native archive never selects Native before the
// native updater exists; macOS's disk-image mode has no Windows counterpart.
enum class ApplyMode { Native, Setup, NotifyOnly };
std::string_view apply_mode_name(ApplyMode); // "native", "setup", "notify_only"

// update_contract.TransactionState.
namespace transaction_state {
inline constexpr std::string_view preparing = "preparing";
inline constexpr std::string_view prepared = "prepared";
inline constexpr std::string_view old_moved = "old_moved";
inline constexpr std::string_view new_active = "new_active";
inline constexpr std::string_view healthy = "healthy";
inline constexpr std::string_view rolled_back = "rolled_back";
inline constexpr std::string_view superseded = "superseded";
} // namespace transaction_state

inline constexpr int kManifestSchemaVersion = 1;
inline constexpr int kTopologyRevision = 1;
inline constexpr std::string_view kArchitecture = "win-x64";
// The first release that ships the native updater (architecture amendment 22).
inline constexpr std::string_view kMinimumUpdaterVersion = "0.1.0";

inline constexpr std::string_view kUpdatesDirname = "updates";
inline constexpr std::string_view kTransactionsDirname = "tx";
inline constexpr std::string_view kJournalName = "journal.json";
inline constexpr std::string_view kHealthName = "healthy";
inline constexpr std::string_view kErrorName = "apply_error.txt";
inline constexpr std::string_view kNativeResultPrefix = "native:";

inline constexpr std::string_view kHealthArg = "--update-health";
inline constexpr std::string_view kTransactionArg = "--transaction-id";
inline constexpr std::string_view kRecoverArg = "--recover";
inline constexpr std::string_view kCleanupArg = "--cleanup-transaction";
inline constexpr std::string_view kParentPidArg = "--parent-pid";

// ".openwhisper-cpp-update.json" and ".openwhisper-cpp-update-complete" (amendment 22), from
// identity().
std::string manifest_name();
std::string sentinel_name();
// "!OpenWhisper_CPPUpdateRecover": the "!" makes Windows delete the RunOnce value only once the
// recovery command has started.
std::string runonce_value_name();

// Strips surrounding whitespace and one leading "v" or "V", then whitespace again.
std::string normalize_version(std::string_view raw);

// (major, minor, patch) of exactly "major.minor.patch" (after normalize_version); anything else,
// such as "2.4.0-rc.1", throws py::ValueError("Not a strict major.minor.patch version: '...'").
// Only this parser authorizes applying an update.
std::array<long long, 3> parse_strict_version(std::string_view raw);

// candidate > current, both strict.
bool is_newer_version(std::string_view current, std::string_view candidate);

// A version at or below kMinimumUpdaterVersion must update through the setup first.
bool is_setup_bridge_release(std::string_view version);

// "<setup_prefix><version>.exe" ("OpenWhisper_CPP-Setup-0.1.1.exe").
std::string setup_asset_name(std::string_view version);
// "<product>-<version>-win64.zip" (amendment 22): parsed from a release, unused before M10.
std::string archive_asset_name(std::string_view version);

// The canonical (lower-case) 32-hex transaction id; throws py::ValueError("Invalid update
// transaction id.") for anything path-like.
std::string validate_transaction_id(std::string_view transaction_id);
std::string encode_native_result(std::string_view transaction_id);
std::optional<std::string> decode_native_result(std::string_view raw);

} // namespace fv::update
