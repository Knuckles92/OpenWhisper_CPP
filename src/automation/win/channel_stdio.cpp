// The channel's protocol streams on Windows (channel_bridge.py protocol_streams and
// PipeLineReader).
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "automation/channel_bridge.h"
#include "automation/detail/stdio_lines.h"

namespace fv::automation {
namespace {

HANDLE os_handle(int fd) {
    return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
}

// The C runtime gives a descriptor of a process started without that handle the value -2 (and
// a closed one -1); such a process has no stream there, as Python's sys.stdin is None.
bool usable(int fd) {
    const HANDLE handle = os_handle(fd);
    return handle != INVALID_HANDLE_VALUE && handle != reinterpret_cast<HANDLE>(-2) &&
           handle != nullptr;
}

std::size_t read_fd(int fd, char* buffer, std::size_t size) {
    const int count =
        _read(fd, buffer, static_cast<unsigned>(std::min<std::size_t>(size, 1 << 16)));
    if (count < 0) {
        throw std::runtime_error("reading the protocol stream failed");
    }
    return static_cast<std::size_t>(count);
}

// Windows serializes synchronous I/O on a pipe: while a ReadFile waits for data, another
// thread's GetFileType on the same pipe waits with it, and a library that sets up its own stdio
// while it loads calls exactly that. Peeking, then reading only the bytes that have arrived,
// never leaves a read pending.
std::size_t read_pipe(int fd, char* buffer, std::size_t size) {
    const HANDLE handle = os_handle(fd);
    DWORD delay = detail::PIPE_POLL_MIN_MS;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            return 0; // the writer closed its end
        }
        if (available > 0) {
            return read_fd(fd, buffer, std::min<std::size_t>(size, available));
        }
        Sleep(delay);
        delay = std::min<DWORD>(delay * 2, detail::PIPE_POLL_MAX_MS);
    }
}

void write_fd(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const unsigned chunk = static_cast<unsigned>(std::min<std::size_t>(bytes.size(), 1 << 16));
        const int written = _write(fd, bytes.data(), chunk);
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
    const int in_fd = _dup(0);
    const int out_fd = _dup(1);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) {
            _close(in_fd);
        }
        if (out_fd >= 0) {
            _close(out_fd);
        }
        return std::nullopt;
    }
    // An inherited descriptor in text mode would turn "\n" into "\r\n".
    _setmode(in_fd, _O_BINARY);
    _setmode(out_fd, _O_BINARY);

    ProtocolStreams streams;
    const bool pipe = GetFileType(os_handle(in_fd)) == FILE_TYPE_PIPE;
    streams.reader = std::make_unique<detail::ChunkLineReader>(
        [in_fd, pipe](char* buffer, std::size_t size) {
            return pipe ? read_pipe(in_fd, buffer, size) : read_fd(in_fd, buffer, size);
        },
        [in_fd] { _close(in_fd); });
    streams.writer = std::make_unique<detail::FunctionLineWriter>(
        [out_fd](std::string_view bytes) { write_fd(out_fd, bytes); },
        [out_fd] { _close(out_fd); });

    // Whatever else prints (the C runtime's stdout, a native library through the standard
    // handle) goes to standard error from here on. _dup2 on descriptor 1 also sets the
    // process's standard output handle.
    std::fflush(stdout);
    if (_dup2(2, 1) != 0) {
        int nul = -1;
        if (_sopen_s(&nul, "NUL", _O_WRONLY | _O_BINARY, _SH_DENYNO, _S_IWRITE) == 0 && nul >= 0) {
            _dup2(nul, 1);
            _close(nul);
        }
    }
    return streams;
}

} // namespace fv::automation
