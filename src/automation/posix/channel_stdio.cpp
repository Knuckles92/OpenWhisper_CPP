// The channel's protocol streams elsewhere than Windows: blocking reads, which hold no lock
// another thread's stdio calls wait on.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "automation/channel_bridge.h"
#include "automation/detail/stdio_lines.h"

namespace fv::automation {
namespace {

bool usable(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

std::size_t read_fd(int fd, char* buffer, std::size_t size) {
    for (;;) {
        const ssize_t count = ::read(fd, buffer, std::min<std::size_t>(size, 1 << 16));
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            throw std::runtime_error("reading the protocol stream failed");
        }
    }
}

void write_fd(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            throw std::runtime_error("writing the protocol stream failed");
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}

} // namespace

std::optional<ProtocolStreams> protocol_streams() {
    if (!usable(0) || !usable(1)) {
        return std::nullopt;
    }
    const int in_fd = ::dup(0);
    const int out_fd = ::dup(1);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) {
            ::close(in_fd);
        }
        if (out_fd >= 0) {
            ::close(out_fd);
        }
        return std::nullopt;
    }
    // A write to a pipe Claude Code closed fails (EPIPE) instead of ending the process, as in
    // Python, which ignores SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    ProtocolStreams streams;
    streams.reader = std::make_unique<detail::ChunkLineReader>(
        [in_fd](char* buffer, std::size_t size) { return read_fd(in_fd, buffer, size); },
        [in_fd] { ::close(in_fd); });
    streams.writer = std::make_unique<detail::FunctionLineWriter>(
        [out_fd](std::string_view bytes) { write_fd(out_fd, bytes); },
        [out_fd] { ::close(out_fd); });
    std::fflush(stdout);
    if (::dup2(2, 1) < 0) {
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) {
            ::dup2(null, 1);
            ::close(null);
        }
    }
    return streams;
}

} // namespace fv::automation
