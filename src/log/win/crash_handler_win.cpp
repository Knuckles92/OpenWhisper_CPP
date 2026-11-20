#include "log/crash_handler.h"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "log/detail/crash_line.h"

namespace fv::log {

namespace {

constexpr DWORD kModuleChars = 1024;
// How long a faulting thread waits for the watcher before the process ends anyway.
constexpr DWORD kDumpWaitMs = 60'000;

// Everything the crash path touches, allocated at install and never freed, so a crash during
// static destruction still finds it.
struct CrashState {
    std::wstring log_path;
    std::wstring dump_folder;
    std::vector<wchar_t> dump_path; // dump_folder + '\' + name, the name written at crash time
    std::size_t dump_name_at = 0;
    HANDLE request = nullptr;
    HANDLE done = nullptr;
    HANDLE watcher = nullptr; // fv-crash-watch, signaled once it has ended
    std::atomic<bool> crashing{false};
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD thread_id = 0;
    wchar_t module_path[kModuleChars] = {};
    char module_utf8[kModuleChars * 3] = {};
    char dump_name[64] = {};
    char dump_relative[96] = {};
    char line[kModuleChars * 3 + 256] = {};
};

CrashState* g_state = nullptr;
std::once_flag g_install;

detail::UtcFields now_utc() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    detail::UtcFields fields;
    fields.year = time.wYear;
    fields.month = time.wMonth;
    fields.day = time.wDay;
    fields.hour = time.wHour;
    fields.minute = time.wMinute;
    fields.second = time.wSecond;
    return fields;
}

// The file name of the module that holds `address`, as UTF-8 in state.module_utf8.
std::string_view faulting_module(CrashState& state, const void* address) {
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    const DWORD length = GetModuleFileNameW(module, state.module_path, kModuleChars);
    if (length == 0 || length >= kModuleChars) {
        return {};
    }
    const wchar_t* name = state.module_path;
    for (DWORD i = 0; i < length; ++i) {
        if (state.module_path[i] == L'\\' || state.module_path[i] == L'/') {
            name = state.module_path + i + 1;
        }
    }
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, name, static_cast<int>(state.module_path + length - name), state.module_utf8,
        static_cast<int>(sizeof(state.module_utf8)), nullptr, nullptr);
    return bytes > 0 ? std::string_view(state.module_utf8, static_cast<std::size_t>(bytes))
                     : std::string_view();
}

// Appends the crash line naming `dump_relative` (empty: no minidump).
void write_line(CrashState& state, const detail::UtcFields& time, std::string_view dump_relative) {
    const void* address = state.pointers && state.pointers->ExceptionRecord
                              ? state.pointers->ExceptionRecord->ExceptionAddress
                              : nullptr;
    const std::uint32_t code = state.pointers && state.pointers->ExceptionRecord
                                   ? state.pointers->ExceptionRecord->ExceptionCode
                                   : 0;
    detail::LineBuffer line(state.line, sizeof(state.line));
    detail::put_crash_line(line, time, code, faulting_module(state, address), dump_relative);

    const HANDLE log =
        CreateFileW(state.log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(log, state.line, static_cast<DWORD>(line.size()), &written, nullptr);
        FlushFileBuffers(log);
        CloseHandle(log);
    }
}

// Runs on fv-crash-watch while the faulting thread waits.
void write_crash(CrashState& state) {
    const detail::UtcFields time = now_utc();

    detail::LineBuffer name(state.dump_name, sizeof(state.dump_name) - 1);
    detail::put_stamp(name, time);
    name.put("-");
    name.put_number(GetCurrentProcessId());
    name.put(".dmp");
    const std::string_view dump_name(state.dump_name, name.size());
    std::size_t at = state.dump_name_at;
    for (const char c : dump_name) {
        if (at + 1 < state.dump_path.size()) {
            state.dump_path[at++] = static_cast<wchar_t>(c);
        }
    }
    state.dump_path[at] = L'\0';

    CreateDirectoryW(state.dump_folder.c_str(), nullptr);
    bool dumped = false;
    const HANDLE file = CreateFileW(state.dump_path.data(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = state.thread_id;
        exception.ExceptionPointers = state.pointers;
        exception.ClientPointers = FALSE;
        const auto type =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                   state.pointers ? &exception : nullptr, nullptr, nullptr) != 0;
        CloseHandle(file);
        if (!dumped) {
            DeleteFileW(state.dump_path.data());
        }
    }

    detail::LineBuffer relative(state.dump_relative, sizeof(state.dump_relative));
    if (dumped) {
        relative.put("crashes\\");
        relative.put(dump_name);
    }
    write_line(state, time, std::string_view(state.dump_relative, relative.size()));
}

DWORD WINAPI watch(void* parameter) {
    auto& state = *static_cast<CrashState*>(parameter);
    WaitForSingleObject(state.request, INFINITE);
    write_crash(state);
    SetEvent(state.done);
    return 0;
}

// Hands the crash to the watcher and waits for it. A second thread crashing meanwhile waits
// for the first crash to end the process.
void report(EXCEPTION_POINTERS* pointers) {
    CrashState* state = g_state;
    if (!state) {
        return;
    }
    if (state->crashing.exchange(true)) {
        Sleep(INFINITE);
    }
    state->pointers = pointers;
    state->thread_id = GetCurrentThreadId();
    SetEvent(state->request);
    // ExitProcess ends every other thread before the DLLs detach, so a crash in a DLL's detach
    // code (NeMo-Speech.cpp's, say) finds the watcher gone. No one is left to write the dump, and
    // waiting for it would hold up the exit for the whole timeout: write the line alone.
    const HANDLE waits[] = {state->done, state->watcher};
    if (WaitForMultipleObjects(2, waits, FALSE, kDumpWaitMs) == WAIT_OBJECT_0 + 1) {
        write_line(*state, now_utc(), {});
    }
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* pointers) {
    report(pointers);
    return EXCEPTION_EXECUTE_HANDLER; // ends the process with the exception's code, no WER UI
}

int report_and_handle(EXCEPTION_POINTERS* pointers) {
    report(pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Failures that are not SEH exceptions get one raised, so the dump has a context to show.
[[noreturn]] void fatal(DWORD code) {
    __try {
        RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (report_and_handle(GetExceptionInformation())) {
    }
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) {
        Sleep(INFINITE);
    }
}

void on_terminate() {
    fatal(kCrashTerminate);
}

void __cdecl on_pure_call() {
    fatal(kCrashPureCall);
}

void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                  std::uintptr_t) {
    fatal(kCrashInvalidParameter);
}

void __cdecl on_abort(int) {
    fatal(kCrashAbort);
}

void install_once(const std::filesystem::path& log_path, const std::filesystem::path& folder) {
    auto* state = new CrashState;
    state->log_path = log_path.native();
    state->dump_folder = folder.native();
    state->dump_path.assign(state->dump_folder.begin(), state->dump_folder.end());
    state->dump_path.push_back(L'\\');
    state->dump_name_at = state->dump_path.size();
    state->dump_path.resize(state->dump_path.size() + 64, L'\0');
    state->request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->request || !state->done) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Cannot create the crash handler's events");
    }
    // A small stack is enough: the watcher only calls into the system and dbghelp.
    const HANDLE thread =
        CreateThread(nullptr, 256 * 1024, watch, state, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Cannot start the crash handler's thread");
    }
    SetThreadDescription(thread, L"fv-crash-watch");
    state->watcher = thread; // kept open: report() waits on it
    g_state = state;

    SetUnhandledExceptionFilter(on_unhandled_exception);
    std::set_terminate(on_terminate);
    _set_purecall_handler(on_pure_call);
    _set_invalid_parameter_handler(on_invalid_parameter);
    // abort() would otherwise show its message box before raising SIGABRT.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, on_abort);
}

} // namespace

void CrashHandler::install(const Paths& paths, const AppIdentity& identity) {
    const auto log_path = crash_log_file(paths, identity);
    const auto folder = dump_folder(paths);
    std::filesystem::create_directories(log_path.parent_path());
    // faulthandler's log is opened for appending at startup, so it exists from then on; a log
    // that cannot be opened fails the install, as Python's open() raising did.
    if (!std::ofstream(log_path, std::ios::binary | std::ios::app)) {
        throw std::filesystem::filesystem_error("Cannot open the crash log", log_path,
                                                std::make_error_code(std::errc::io_error));
    }
    std::call_once(g_install, [&] { install_once(log_path, folder); });
}

} // namespace fv::log
