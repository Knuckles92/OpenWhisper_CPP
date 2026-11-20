#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/identity.h"
#include "base/paths.h"

namespace fv::log {

// One fatal error, as its line in the crash log records it.
struct CrashInfo {
    std::string utc;    // "2026-11-14T10:20:30Z"
    std::uint32_t code; // the SEH exception code, a kCrash* code below, or the POSIX signal
    std::string module; // file name of the module holding the faulting address, or "unknown"
    std::string dump;   // the minidump relative to the data root ("crashes\<stamp>-<pid>.dmp"),
                        // empty when none was written
};

// Codes for fatal errors that are not hardware exceptions (customer bit set, "FV").
inline constexpr std::uint32_t kCrashTerminate = 0xE0465601;        // std::terminate
inline constexpr std::uint32_t kCrashPureCall = 0xE0465602;         // pure virtual call
inline constexpr std::uint32_t kCrashInvalidParameter = 0xE0465603; // CRT invalid parameter
inline constexpr std::uint32_t kCrashAbort = 0xE0465604;            // abort(), SIGABRT

// The native counterpart of OpenWhisper's faulthandler crash log: a hard crash appends one line
// to <log file>.crash.log ("openwhisper-cpp.crash.log", Path.with_suffix(".crash.log")) and, on
// Windows, writes a minidump of every thread to crashes\<UTC stamp>-<pid>.dmp under the data
// root. On Windows it covers unhandled SEH exceptions (SetUnhandledExceptionFilter),
// std::terminate on the installing thread, pure virtual calls, CRT invalid parameters and
// SIGABRT; the dump is written by the pre-created thread "fv-crash-watch", never by the faulting
// thread, with every buffer allocated at install. Elsewhere it is a stub that appends the line
// for SIGSEGV, SIGABRT, SIGFPE, SIGILL and SIGBUS, without a dump.
// Affinity: thread-safe; install once per process, before other threads start.
class CrashHandler {
public:
    // Throws std::filesystem::filesystem_error when the crash log's folder cannot be prepared or
    // the crash log cannot be opened.
    static void install(const Paths&, const AppIdentity&);
    // The last crash the crash log records, or nullopt.
    static std::optional<CrashInfo> last_crash(const Paths&, const AppIdentity&);

    static std::filesystem::path crash_log_file(const Paths&, const AppIdentity&);
    static std::filesystem::path dump_folder(const Paths&);

    // "<utc> Fatal error 0x<8 hex> in <module>; minidump <dump>" (or "; no minidump").
    static std::optional<CrashInfo> parse_line(std::string_view line);
};

} // namespace fv::log
