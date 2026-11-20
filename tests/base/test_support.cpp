#include "test_support.h"

#include <stdexcept>
#include <system_error>
#include <thread>

#include "base/ids.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fv::test {

TempDir::TempDir()
    : path_(std::filesystem::temp_directory_path() / ("fv-base-test-" + random_hex(12))) {
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

ScopedCwd::ScopedCwd(const std::filesystem::path& to) : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(to);
}

ScopedCwd::~ScopedCwd() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
}

#if defined(_WIN32)

namespace {

HANDLE as_handle(std::intptr_t value) {
    return reinterpret_cast<HANDLE>(value);
}

std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (const wchar_t c : arg) {
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    return out + L"\"";
}

} // namespace

ChildProcess::ChildProcess(const std::filesystem::path& program,
                           const std::vector<std::string>& args) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &inherit, 0) ||
        !CreatePipe(&out_read, &out_write, &inherit, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreatePipe");
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote(program.native());
    for (const auto& arg : args) {
        command +=
            L" " + quote(std::filesystem::path(std::u8string(arg.begin(), arg.end())).native());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in_read;
    startup.hStdOutput = out_write;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    const DWORD error = GetLastError();
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!started) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        throw std::system_error(static_cast<int>(error), std::system_category(), "CreateProcessW");
    }
    CloseHandle(info.hThread);
    process_ = reinterpret_cast<std::intptr_t>(info.hProcess);
    stdin_ = reinterpret_cast<std::intptr_t>(in_write);
    stdout_ = reinterpret_cast<std::intptr_t>(out_read);
}

ChildProcess::~ChildProcess() {
    close_stdin();
    if (!wait(std::chrono::milliseconds(10'000))) {
        kill();
        wait(std::chrono::milliseconds(10'000));
    }
    if (stdout_ != -1) {
        CloseHandle(as_handle(stdout_));
    }
    CloseHandle(as_handle(process_));
}

std::optional<std::string> ChildProcess::read_line(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (const auto end = buffer_.find('\n'); end != std::string::npos) {
            std::string line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(as_handle(stdout_), nullptr, 0, nullptr, &available, nullptr)) {
            return std::nullopt; // the child closed its end
        }
        if (available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!ReadFile(as_handle(stdout_), chunk.data(), available, &read, nullptr)) {
                return std::nullopt;
            }
            buffer_.append(chunk, 0, read);
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::close_stdin() {
    if (stdin_ != -1) {
        CloseHandle(as_handle(stdin_));
        stdin_ = -1;
    }
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds timeout) {
    if (exit_code_) {
        return exit_code_;
    }
    if (WaitForSingleObject(as_handle(process_), static_cast<DWORD>(timeout.count())) !=
        WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(as_handle(process_), &code);
    exit_code_ = static_cast<int>(code);
    return exit_code_;
}

void ChildProcess::kill() {
    TerminateProcess(as_handle(process_), 1);
}

#else

ChildProcess::ChildProcess(const std::filesystem::path& program,
                           const std::vector<std::string>& args) {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "fork");
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        std::vector<std::string> owned{program.string()};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& arg : owned) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    process_ = pid;
    stdin_ = in_pipe[1];
    stdout_ = out_pipe[0];
}

ChildProcess::~ChildProcess() {
    close_stdin();
    if (!wait(std::chrono::milliseconds(10'000))) {
        kill();
        wait(std::chrono::milliseconds(10'000));
    }
    if (stdout_ != -1) {
        close(static_cast<int>(stdout_));
    }
}

std::optional<std::string> ChildProcess::read_line(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (const auto end = buffer_.find('\n'); end != std::string::npos) {
            std::string line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            return line;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return std::nullopt;
        }
        pollfd entry{static_cast<int>(stdout_), POLLIN, 0};
        if (poll(&entry, 1, static_cast<int>(left.count())) <= 0) {
            return std::nullopt;
        }
        char chunk[256];
        const ssize_t read_count = read(static_cast<int>(stdout_), chunk, sizeof(chunk));
        if (read_count <= 0) {
            return std::nullopt;
        }
        buffer_.append(chunk, static_cast<std::size_t>(read_count));
    }
}

void ChildProcess::close_stdin() {
    if (stdin_ != -1) {
        close(static_cast<int>(stdin_));
        stdin_ = -1;
    }
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds timeout) {
    if (exit_code_) {
        return exit_code_;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(static_cast<pid_t>(process_), &status, WNOHANG);
        if (done == static_cast<pid_t>(process_)) {
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            return exit_code_;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::kill() {
    ::kill(static_cast<pid_t>(process_), SIGKILL);
}

#endif

} // namespace fv::test
