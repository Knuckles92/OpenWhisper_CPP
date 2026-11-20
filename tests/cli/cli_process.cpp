#include "cli_process.h"

#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "testkit/testkit.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fv::clitest {
namespace {

// Puts the variables in place for the child (it inherits ours) and puts ours back.
class ScopedEnv {
public:
    explicit ScopedEnv(const std::vector<EnvOverride>& overrides) {
        std::vector<EnvOverride> wanted = {{"FIORIVOCE_RUNTIME_DIR", std::nullopt},
                                           {"FIORIVOCE_MODEL", std::nullopt},
                                           {"FIORIVOCE_LOG_LEVEL", std::nullopt}};
        wanted.insert(wanted.end(), overrides.begin(), overrides.end());
        for (const auto& [name, value] : wanted) {
            saved_.emplace_back(name, testkit::get_env(name));
            testkit::set_env(name, value);
        }
    }
    ~ScopedEnv() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            try {
                testkit::set_env(it->first, it->second);
            } catch (const std::exception&) {
                // Restoring is best effort; the next case sets what it needs.
            }
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::vector<EnvOverride> saved_;
};

std::filesystem::path scratch_file(const char* stem) {
    static std::atomic<unsigned> counter{0};
    return std::filesystem::temp_directory_path() /
           ("fv-cli-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
            "-" + std::to_string(counter++) + "-" + stem);
}

} // namespace

std::filesystem::path cli_executable() {
    return std::filesystem::path(FV_CLI_EXECUTABLE);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

#if defined(_WIN32)

namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) {
            CloseHandle(value);
        }
        value = INVALID_HANDLE_VALUE;
    }
};

HANDLE open_inheritable(const wchar_t* path, DWORD access, DWORD disposition) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, disposition,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// One argument as CommandLineToArgvW and the CRT parse it back: backslashes are literal unless
// they precede a quote, so a run of them before a quote, or before the closing quote, is doubled.
std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

} // namespace

CliResult run_cli(const std::vector<std::string>& args, const std::vector<EnvOverride>& env,
                  std::chrono::milliseconds timeout) {
    const ScopedEnv scoped(env);
    const std::filesystem::path out_file = scratch_file("out.txt");
    const std::filesystem::path err_file = scratch_file("err.txt");
    Handle out(open_inheritable(out_file.c_str(), GENERIC_WRITE, CREATE_ALWAYS));
    Handle err(open_inheritable(err_file.c_str(), GENERIC_WRITE, CREATE_ALWAYS));
    Handle in(open_inheritable(L"NUL", GENERIC_READ, OPEN_EXISTING));
    if (out.value == INVALID_HANDLE_VALUE || err.value == INVALID_HANDLE_VALUE ||
        in.value == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "opening the child's standard streams");
    }

    std::wstring command = quote(cli_executable().native());
    for (const std::string& arg : args) {
        command +=
            L" " + quote(std::filesystem::path(std::u8string(arg.begin(), arg.end())).native());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in.value;
    startup.hStdOutput = out.value;
    startup.hStdError = err.value;
    PROCESS_INFORMATION info{};
    // Detached, so that no console host joins the job and the count is the child's own; suspended
    // until it is in the job, so that nothing it starts can escape the count.
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateProcessW");
    }
    Handle process(info.hProcess);
    Handle thread(info.hThread);
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.value == nullptr || !AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1);
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "AssignProcessToJobObject");
    }
    ResumeThread(thread.value);

    CliResult result;
    if (WaitForSingleObject(process.value, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process.value, &code);
        result.exit_code = code;
    } else {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(process.value, 10'000);
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting,
                                  sizeof(accounting), nullptr)) {
        result.processes = accounting.TotalProcesses;
    }
    out.reset();
    err.reset();
    result.out = read_text(out_file);
    result.err = read_text(err_file);
    std::error_code ignored;
    std::filesystem::remove(out_file, ignored);
    std::filesystem::remove(err_file, ignored);
    return result;
}

#else

CliResult run_cli(const std::vector<std::string>& args, const std::vector<EnvOverride>& env,
                  std::chrono::milliseconds timeout) {
    const ScopedEnv scoped(env);
    const std::filesystem::path out_file = scratch_file("out.txt");
    const std::filesystem::path err_file = scratch_file("err.txt");
    std::vector<std::string> owned{cli_executable().string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "fork");
    }
    if (pid == 0) {
        const int out = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int err = open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int in = open("/dev/null", O_RDONLY);
        dup2(in, 0);
        dup2(out, 1);
        dup2(err, 2);
        execv(argv[0], argv.data());
        _exit(127);
    }
    CliResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            result.exit_code = WIFEXITED(status)
                                   ? static_cast<std::uint32_t>(WEXITSTATUS(status))
                                   : static_cast<std::uint32_t>(128 + WTERMSIG(status));
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    result.out = read_text(out_file);
    result.err = read_text(err_file);
    std::error_code ignored;
    std::filesystem::remove(out_file, ignored);
    std::filesystem::remove(err_file, ignored);
    return result;
}

#endif

} // namespace fv::clitest
