#include "automation/detail/stdio_lines.h"

#include <array>
#include <utility>

namespace fv::automation::detail {

ChunkLineReader::ChunkLineReader(Read read, std::function<void()> close)
    : read_(std::move(read)), close_(std::move(close)) {}

ChunkLineReader::~ChunkLineReader() {
    if (close_) {
        close_();
    }
}

std::optional<std::string> ChunkLineReader::readline() {
    std::array<char, 4096> chunk{};
    for (;;) {
        const std::size_t end = buffer_.find('\n');
        if (end != std::string::npos) {
            std::string line = buffer_.substr(0, end + 1);
            buffer_.erase(0, end + 1);
            return line;
        }
        if (eof_) {
            if (buffer_.empty()) {
                return std::nullopt;
            }
            return std::exchange(buffer_, {});
        }
        const std::size_t count = read_(chunk.data(), chunk.size());
        if (count == 0) {
            eof_ = true;
        } else {
            buffer_.append(chunk.data(), count);
        }
    }
}

FunctionLineWriter::FunctionLineWriter(std::function<void(std::string_view)> write,
                                       std::function<void()> close)
    : write_(std::move(write)), close_(std::move(close)) {}

FunctionLineWriter::~FunctionLineWriter() {
    close();
}

void FunctionLineWriter::write(std::string_view bytes) {
    if (closed_) {
        throw std::runtime_error("the protocol stream is closed");
    }
    write_(bytes);
}

void FunctionLineWriter::close() {
    if (!closed_) {
        closed_ = true;
        if (close_) {
            close_();
        }
    }
}

} // namespace fv::automation::detail
