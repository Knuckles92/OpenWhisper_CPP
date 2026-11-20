#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "automation/channel_bridge.h"

// The protocol streams of fiorivoce-cli --channel, over whatever reads and writes the platform
// gives (win/channel_stdio.cpp, posix/channel_stdio.cpp).
namespace fv::automation::detail {

// readline() over a function that reads: `read` fills up to `size` bytes, waiting until at least
// one has arrived, returns 0 at the end of the stream and throws std::runtime_error when it
// fails. `close` runs once, when the reader is destroyed.
// Affinity: one thread.
class ChunkLineReader final : public LineReader {
public:
    using Read = std::function<std::size_t(char* buffer, std::size_t size)>;
    explicit ChunkLineReader(Read read, std::function<void()> close = {});
    ~ChunkLineReader() override;
    ChunkLineReader(const ChunkLineReader&) = delete;
    ChunkLineReader& operator=(const ChunkLineReader&) = delete;

    std::optional<std::string> readline() override;

private:
    Read read_;
    std::function<void()> close_;
    std::string buffer_;
    bool eof_ = false;
};

// A LineWriter over two functions: `write` sends every byte or throws std::runtime_error;
// `close` runs once (close() or destruction, whichever is first).
// Affinity: MessageWriter's lock.
class FunctionLineWriter final : public LineWriter {
public:
    FunctionLineWriter(std::function<void(std::string_view)> write, std::function<void()> close);
    ~FunctionLineWriter() override;
    FunctionLineWriter(const FunctionLineWriter&) = delete;
    FunctionLineWriter& operator=(const FunctionLineWriter&) = delete;

    void write(std::string_view bytes) override;
    void close() override;

private:
    std::function<void(std::string_view)> write_;
    std::function<void()> close_;
    bool closed_ = false;
};

// How long a pipe reader waits between looks at a pipe with nothing in it: 5 ms, doubling to
// 50 ms, back to 5 ms once bytes arrived.
inline constexpr int PIPE_POLL_MIN_MS = 5;
inline constexpr int PIPE_POLL_MAX_MS = 50;

} // namespace fv::automation::detail
