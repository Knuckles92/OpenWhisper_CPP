#include "update_native/detail/health_child.h"

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

// The updater ships for Windows only; this child keeps the shared health wait testable.
namespace fv::update_native::detail {
namespace {
class PosixHealthChild final : public HealthChild {
public:
    explicit PosixHealthChild(pid_t pid) : pid_(pid) {}
    ~PosixHealthChild() override {
        // Reap a child that has ended; one still running is left to init.
        if (!exited_)
            (void)::waitpid(pid_, nullptr, WNOHANG);
    }

    bool wait(std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (exited_)
                return true;
            const pid_t result = ::waitpid(pid_, nullptr, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                exited_ = true;
                return true;
            }
            if (result < 0 && errno != EINTR)
                throw std::system_error(errno, std::system_category(),
                                        "Waiting for updated application");
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void terminate() override {
        if (wait(std::chrono::milliseconds(0)))
            return;
        if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH)
            throw std::system_error(errno, std::system_category(),
                                    "Stopping failed updated application");
        if (!wait(std::chrono::seconds(15)))
            throw std::runtime_error("The failed updated application did not exit.");
    }

private:
    pid_t pid_;
    bool exited_ = false;
};
} // namespace

std::unique_ptr<HealthChild> HealthChild::launch(const std::filesystem::path& executable,
                                                 const std::vector<std::string>& arguments,
                                                 const std::filesystem::path& directory) {
    std::vector<std::string> owned{executable.string()};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : owned)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0)
        throw std::system_error(errno, std::system_category(), "Launching updated application");
    if (pid == 0) {
        // Only async-signal-safe calls between fork and exec. The new version inherits none of
        // the helper's streams, as on Windows, where no handle is inherited.
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            (void)::dup2(null, STDIN_FILENO);
            (void)::dup2(null, STDOUT_FILENO);
            (void)::dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                (void)::close(null);
        }
        if (::chdir(directory.c_str()) == 0)
            ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    return std::make_unique<PosixHealthChild>(pid);
}
} // namespace fv::update_native::detail
