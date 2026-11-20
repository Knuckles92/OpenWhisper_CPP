// fv_process_child: the program the [platform] tests of tests/process start. Its arguments are
// commands run in order:
//
//   print TEXT          TEXT and a newline to stdout   write TEXT   TEXT alone
//   write-hex HEX       the bytes HEX spells (two hex digits each) to stdout
//   emit N             N bytes "x" to stdout       emit-err N   N bytes "E" to stderr
//   echo                stdin to stdout             upper        stdin upper-cased (ASCII, Latin-1)
//   count-stdin         stdin's length to stdout    read-to FILE stdin's length to FILE
//   ignore-stdin        nothing                     args ...     the rest of the arguments as a
//                                                                JSON list (ends the commands)
//   sleep S             S seconds                   exit C       exit with code C
//   env NAME            the variable and a newline, "<unset>" without it
//   print-cwd           the working folder and a newline
//   in-job              "1" when this process is in a kill-on-close job its starter isn't in,
//                       else "0"
//   write-file F TEXT   TEXT to F (through a rename, so a reader never sees half)
//   console-window F    "window" to F when this process has a console window
//                       (GetConsoleWindow), else "none"; always "none" off Windows
//   ticks MARKER        "tick" lines every 50 ms for 30 s; when one can't be written, "closed"
//                       to MARKER
//   spawn PIDFILE OUT IN COMMANDS... ;;
//                       starts this program with COMMANDS, its stdout and stderr this one's
//                       (OUT "inherit") or NUL ("null"), its stdin likewise (IN), and writes its
//                       process id to PIDFILE
//   record FILE NAME ...  appends {"cmd": [command line], "args": [the rest], "mark": [NAME's
//                       value or "<unset>"]} as one line to FILE (ends the commands)
//
// Text is UTF-8 on every platform.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

bool write_to(DWORD which, std::string_view bytes) {
    HANDLE handle = GetStdHandle(which);
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

bool write_out(std::string_view bytes) {
    return write_to(STD_OUTPUT_HANDLE, bytes);
}
bool write_err(std::string_view bytes) {
    return write_to(STD_ERROR_HANDLE, bytes);
}

std::string read_stdin() {
    std::string data;
    char buffer[65536];
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        data.append(buffer, got);
    }
    return data;
}

std::filesystem::path path_of(std::string_view utf8) {
    return std::filesystem::path(widen(utf8));
}
#else
bool write_fd(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(fd, bytes.data(), bytes.size());
        if (written <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}
bool write_out(std::string_view bytes) {
    return write_fd(1, bytes);
}
bool write_err(std::string_view bytes) {
    return write_fd(2, bytes);
}
std::string read_stdin() {
    std::string data;
    char buffer[65536];
    for (;;) {
        const auto got = ::read(0, buffer, sizeof(buffer));
        if (got <= 0) {
            return data;
        }
        data.append(buffer, static_cast<std::size_t>(got));
    }
}
std::filesystem::path path_of(std::string_view utf8) {
    return std::filesystem::path(std::string(utf8));
}
#endif

void write_file(std::string_view file, std::string_view text) {
    const auto target = path_of(file);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto byte = static_cast<unsigned char>(out[i]);
        if (byte >= 'a' && byte <= 'z') {
            out[i] = static_cast<char>(byte - 'a' + 'A');
        } else if (byte == 0xC3 && i + 1 < out.size()) {
            // U+00E0..U+00FE (except U+00F7) are two bytes C3 A0..BE; upper case is 0x20 less.
            const auto next = static_cast<unsigned char>(out[i + 1]);
            if (next >= 0xA0 && next <= 0xBE && next != 0xB7) {
                out[i + 1] = static_cast<char>(next - 0x20);
            }
            ++i;
        }
    }
    return out;
}

std::string json_list(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += '"';
        for (const char c : items[i]) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (byte < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                out += escaped;
            } else {
                out += c;
            }
        }
        out += '"';
    }
    return out + "]";
}

std::string current_folder() {
#ifdef _WIN32
    return narrow(std::filesystem::current_path().wstring());
#else
    return std::filesystem::current_path().string();
#endif
}

#ifdef _WIN32
DWORD parent_pid() {
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Process32FirstW(snapshot, &entry); found;
         found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}
#endif

// In a job of its starter's own making: whatever started the test runner may hold every process
// in a job already, so only the innermost job counts, and only when it ends its processes on
// close and doesn't hold the process that started this one. (A console program's conhost.exe
// shares the job, so the job holds two processes.)
bool in_job() {
#ifdef _WIN32
    BOOL result = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &result) || !result) {
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits), nullptr) ||
        (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0) {
        return false;
    }
    // A job of many processes doesn't fit: ERROR_MORE_DATA, and not the launcher's.
    struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST list;
        ULONG_PTR more[15];
    } ids{};
    if (!QueryInformationJobObject(nullptr, JobObjectBasicProcessIdList, &ids, sizeof(ids),
                                   nullptr)) {
        return false;
    }
    const DWORD parent = parent_pid();
    const ULONG_PTR* first = ids.list.ProcessIdList;
    const ULONG_PTR* last = first + ids.list.NumberOfProcessIdsInList;
    return parent != 0 && std::find(first, last, static_cast<ULONG_PTR>(parent)) == last;
#else
    return false;
#endif
}

#ifdef _WIN32
// subprocess.list2cmdline.
std::wstring command_line(const std::vector<std::wstring>& args) {
    std::wstring result;
    for (const auto& arg : args) {
        if (!result.empty()) {
            result += L' ';
        }
        const bool quote = arg.empty() || arg.find_first_of(L" \t") != std::wstring::npos;
        if (quote) {
            result += L'"';
        }
        std::wstring backslashes;
        for (const wchar_t c : arg) {
            if (c == L'\\') {
                backslashes += c;
            } else if (c == L'"') {
                result.append(backslashes.size() * 2, L'\\');
                backslashes.clear();
                result += L"\\\"";
            } else {
                result += backslashes;
                backslashes.clear();
                result += c;
            }
        }
        result += backslashes;
        if (quote) {
            result += backslashes;
            result += L'"';
        }
    }
    return result;
}

HANDLE standard_for_child(DWORD which, bool inherit, DWORD nul_access) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    if (inherit) {
        HANDLE copy = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetStdHandle(which), GetCurrentProcess(), &copy, 0,
                            TRUE, DUPLICATE_SAME_ACCESS)) {
            return copy;
        }
    }
    return CreateFileW(L"NUL", nul_access, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       OPEN_EXISTING, 0, nullptr);
}

std::uint32_t spawn(const std::vector<std::string>& commands, bool inherit_out, bool inherit_in) {
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::vector<std::wstring> args{self};
    for (const auto& command : commands) {
        args.push_back(widen(command));
    }
    std::wstring line = command_line(args);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = standard_for_child(STD_INPUT_HANDLE, inherit_in, GENERIC_READ);
    startup.hStdOutput = standard_for_child(STD_OUTPUT_HANDLE, inherit_out, GENERIC_WRITE);
    startup.hStdError = standard_for_child(STD_ERROR_HANDLE, inherit_out, GENERIC_WRITE);
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(startup.hStdInput);
    CloseHandle(startup.hStdOutput);
    CloseHandle(startup.hStdError);
    if (!created) {
        return 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
}
#else
std::uint32_t spawn(const std::vector<std::string>& commands, bool inherit_out, bool inherit_in) {
    std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid < 0 ? 0 : static_cast<std::uint32_t>(pid);
    }
    const int nul = ::open("/dev/null", O_RDWR);
    if (!inherit_in) {
        ::dup2(nul, 0);
    }
    if (!inherit_out) {
        ::dup2(nul, 1);
        ::dup2(nul, 2);
    }
    std::vector<std::string> owned{self};
    owned.insert(owned.end(), commands.begin(), commands.end());
    std::vector<char*> argv;
    for (auto& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execv(self.c_str(), argv.data());
    ::_exit(127);
}
#endif

int run(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& command = args[i];
        const auto next = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (command == "print") {
            write_out(next() + "\n");
        } else if (command == "write") {
            write_out(next());
        } else if (command == "write-hex") {
            const auto hex = next();
            std::string bytes;
            for (std::size_t at = 0; at + 1 < hex.size(); at += 2) {
                bytes += static_cast<char>(std::stoi(hex.substr(at, 2), nullptr, 16));
            }
            write_out(bytes);
        } else if (command == "emit") {
            write_out(std::string(std::stoul(next()), 'x'));
        } else if (command == "emit-err") {
            write_err(std::string(std::stoul(next()), 'E'));
        } else if (command == "echo") {
            write_out(read_stdin());
        } else if (command == "upper") {
            write_out(upper(read_stdin()));
        } else if (command == "count-stdin") {
            write_out(std::to_string(read_stdin().size()));
        } else if (command == "read-to") {
            const auto file = next();
            write_file(file, std::to_string(read_stdin().size()));
        } else if (command == "ignore-stdin") {
        } else if (command == "args") {
            write_out(json_list(std::vector<std::string>(args.begin() + i + 1, args.end())) + "\n");
            return 0;
        } else if (command == "record") {
            // Appends {"cmd": [its command line], "args": [...], "mark": [the variable or
            // "<unset>"]} as one line to FILE.
            const auto file = next();
            const auto mark = next();
            std::vector<std::string> rest(args.begin() + i + 1, args.end());
#ifdef _WIN32
            const std::string line = narrow(GetCommandLineW());
            wchar_t value[32768];
            const DWORD size = GetEnvironmentVariableW(widen(mark).c_str(), value, 32768);
            const std::string marked =
                size == 0 ? std::string("<unset>") : narrow(std::wstring_view(value, size));
#else
            const std::string line;
            const char* value = std::getenv(mark.c_str());
            const std::string marked = value == nullptr ? std::string("<unset>") : value;
#endif
            std::ofstream out(path_of(file), std::ios::binary | std::ios::app);
            out << "{\"cmd\": " << json_list({line}) << ", \"args\": " << json_list(rest)
                << ", \"mark\": " << json_list({marked}) << "}\n";
            return 0;
        } else if (command == "sleep") {
            std::this_thread::sleep_for(std::chrono::duration<double>(std::stod(next())));
        } else if (command == "exit") {
            return std::stoi(next());
        } else if (command == "env") {
            const auto name = next();
#ifdef _WIN32
            wchar_t value[32768];
            const DWORD size = GetEnvironmentVariableW(widen(name).c_str(), value, 32768);
            write_out((size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                          ? std::string("<unset>\n")
                          : narrow(std::wstring_view(value, size)) + "\n");
#else
            const char* value = std::getenv(name.c_str());
            write_out(value == nullptr ? std::string("<unset>\n") : std::string(value) + "\n");
#endif
        } else if (command == "print-cwd") {
            write_out(current_folder() + "\n");
        } else if (command == "in-job") {
            write_out(in_job() ? "1" : "0");
        } else if (command == "write-file") {
            const auto file = next();
            write_file(file, next());
        } else if (command == "console-window") {
            const auto file = next();
#ifdef _WIN32
            write_file(file, GetConsoleWindow() == nullptr ? "none" : "window");
#else
            write_file(file, "none");
#endif
        } else if (command == "ticks") {
            const auto marker = next();
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < until) {
                if (!write_out("tick\n")) {
                    write_file(marker, "closed");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } else if (command == "spawn") {
            const auto pid_file = next();
            const bool inherit_out = next() == "inherit";
            const bool inherit_in = next() == "inherit";
            std::vector<std::string> commands;
            while (i + 1 < args.size() && args[i + 1] != ";;") {
                commands.push_back(args[++i]);
            }
            ++i; // ";;"
            write_file(pid_file, std::to_string(spawn(commands, inherit_out, inherit_in)));
        } else {
            write_err("unknown command: " + command + "\n");
            return 2;
        }
    }
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(narrow(argv[i]));
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
