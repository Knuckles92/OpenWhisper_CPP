#include "cli_process.h"

#ifndef _WIN32

#include "channel_support.h"
#include "pyport/text.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fv::clitest {
namespace {
using namespace std::chrono_literals;

void check(int result, const char* operation) {
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), operation);
    }
}

class Fd {
public:
    explicit Fd(int file) : file_(file) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return file_; }
    int release() { return std::exchange(file_, -1); }
    void reset() {
        if (file_ != -1) {
            close(release());
        }
    }

private:
    int file_;
};

class SpawnActions {
public:
    SpawnActions() { check(posix_spawn_file_actions_init(&value), "setting up CLI pipes"); }
    ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
    posix_spawn_file_actions_t value;
};

class SpawnAttributes {
public:
    SpawnAttributes() { check(posix_spawnattr_init(&value), "setting up CLI process group"); }
    ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
    posix_spawnattr_t value;
};

std::array<int, 2> make_pipe() {
    std::array<int, 2> files{};
    if (pipe(files.data()) != 0) {
        check(errno, "opening CLI test pipe");
    }
    return files;
}

std::vector<char*> pointers(std::vector<std::string>& storage) {
    std::vector<char*> result;
    result.reserve(storage.size() + 1);
    for (auto& entry : storage) {
        result.push_back(entry.data());
    }
    result.push_back(nullptr);
    return result;
}

void pump(std::stop_token stop, int fd, const ByteSink& sink) {
    std::array<char, 4096> buffer{};
    while (!stop.stop_requested()) {
        pollfd descriptor{fd, POLLIN, 0};
        const auto ready = poll(&descriptor, 1, 100);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0) {
            break;
        }
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        if (sink) {
            sink(std::span(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (sink) {
        sink({});
    }
}

// Test-only alternative for POSIX, where the desktop process launcher remains a stub.
// No shell interprets the executable, the arguments, or the fixture environment.
class PosixCli final : public Process {
public:
    explicit PosixCli(const ProcessSpec& spec) {
        // The app executable too: its --api and --channel forward to the CLI beside it.
        if (spec.program != cli_executable() &&
            (app_executable().empty() || spec.program != app_executable())) {
            throw std::invalid_argument(
                "The CLI fixture runs only the compiled CLI and app executables.");
        }
        std::array<int, 2> input{};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, input.data()) != 0) {
            check(errno, "opening CLI test input");
        }
        Fd in(input[0]);
        Fd child_in(input[1]);
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(in.get(), SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            check(errno, "setting up CLI test input");
        }
#endif
        const auto output = make_pipe();
        Fd out(output[0]);
        Fd child_out(output[1]);
        const auto error = make_pipe();
        Fd err(error[0]);
        Fd child_err(error[1]);

        SpawnActions actions;
        for (const auto& [fd, target] :
             {std::pair{child_in.get(), STDIN_FILENO}, std::pair{child_out.get(), STDOUT_FILENO},
              std::pair{child_err.get(), STDERR_FILENO}}) {
            check(posix_spawn_file_actions_adddup2(&actions.value, fd, target),
                  "wiring the CLI test child");
        }
#if defined(__linux__) && defined(__GLIBC__)
        // The child must not keep a parent's loopback listener open across a fixture restart.
        check(posix_spawn_file_actions_addclosefrom_np(&actions.value, STDERR_FILENO + 1),
              "closing inherited CLI test descriptors");
#else
        for (const int fd :
             {in.get(), child_in.get(), out.get(), child_out.get(), err.get(), child_err.get()}) {
            check(posix_spawn_file_actions_addclose(&actions.value, fd),
                  "wiring the CLI test child");
        }
#endif
        SpawnAttributes attributes;
        short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
        check(posix_spawnattr_setflags(&attributes.value, flags), "setting up CLI process group");
        check(posix_spawnattr_setpgroup(&attributes.value, 0), "setting up CLI process group");

        std::vector<std::string> args{spec.program.string()};
        args.insert(args.end(), spec.args.begin(), spec.args.end());
        std::vector<std::string> env;
        for (const auto& [name, value] : spec.env) {
            env.push_back(name + "=" + value);
        }
        auto argv = pointers(args);
        auto envp = pointers(env);
        check(posix_spawn(&pid_, argv.front(), &actions.value, &attributes.value, argv.data(),
                          envp.data()),
              "starting the CLI test child");
        in_ = in.release();
        out_ = out.release();
        err_ = err.release();
    }

    ~PosixCli() override {
        if (!exit_code()) {
            kill_tree();
            wait(2s);
        }
        release_pipes(0ms);
    }

    std::uint32_t pid() const override { return static_cast<std::uint32_t>(pid_); }

    void write_stdin(std::string_view data) override {
        while (in_ != -1 && !data.empty()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto written = send(in_, data.data(), data.size(), flags);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return;
            }
            data.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    void on_line(LineSink, LineSink) override {
        throw std::logic_error("The CLI test fixture uses raw byte pipes.");
    }

    void on_bytes(ByteSink out, ByteSink err) override {
        out_thread_ = std::jthread(pump, out_, std::move(out));
        err_thread_ = std::jthread(pump, err_, std::move(err));
    }

    void close_stdin() override {
        if (in_ != -1) {
            close(std::exchange(in_, -1));
        }
    }

    std::optional<int> wait(std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            if (auto code = exit_code()) {
                return code;
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return exit_code();
    }

    std::optional<int> exit_code() const override {
        if (!code_) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
        return code_;
    }

    void kill_tree() override {
        if (!exit_code()) {
            kill(-pid_, SIGKILL);
        }
    }

    void disown() override {
        throw std::logic_error("The CLI test fixture always owns its child.");
    }

    void release_pipes(std::chrono::milliseconds) override {
        close_stdin();
        out_thread_ = std::jthread();
        err_thread_ = std::jthread();
        if (out_ != -1) {
            close(std::exchange(out_, -1));
        }
        if (err_ != -1) {
            close(std::exchange(err_, -1));
        }
    }

private:
    pid_t pid_ = -1;
    int in_ = -1;
    int out_ = -1;
    int err_ = -1;
    mutable std::optional<int> code_;
    std::jthread out_thread_;
    std::jthread err_thread_;
};

} // namespace

std::unique_ptr<Process> start_posix_cli_fixture(const ProcessSpec& spec) {
    return std::make_unique<PosixCli>(spec);
}
#ifdef __linux__
std::vector<std::string> mapped_modules(std::uint32_t pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::vector<std::string> names;
    for (std::string line; std::getline(maps, line);) {
        if (const auto slash = line.find('/'); slash != std::string::npos) {
            names.push_back(
                py::lower(std::filesystem::path(line.substr(slash)).filename().string()));
        }
    }
    return names;
}
#endif
} // namespace fv::clitest

#endif
