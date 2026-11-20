#include "audio/detail/raw_file.h"

#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace fv::audio::detail {

RawFile::RawFile(RawFile&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

RawFile& RawFile::operator=(RawFile&& other) noexcept {
    if (this != &other) {
        try {
            close();
        } catch (...) { // NOLINT(bugprone-empty-catch): a failed close leaves nothing to undo
        }
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

RawFile::~RawFile() {
    try {
        close();
    } catch (...) { // NOLINT(bugprone-empty-catch): destructors never throw
    }
}

bool RawFile::is_open() const {
    return handle_ != -1;
}

void RawFile::write_all(std::span<const std::byte> data) {
    while (!data.empty()) {
        const std::size_t written = write(data);
        if (written == 0) {
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                    "Recording storage accepted no more data");
        }
        data = data.subspan(written);
    }
}

namespace {

// tempfile's _RandomNameSequence alphabet.
constexpr std::string_view kTempCharacters = "abcdefghijklmnopqrstuvwxyz0123456789_";

} // namespace

std::string random_temp_name(std::string_view suffix) {
    thread_local std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, kTempCharacters.size() - 1);
    std::string name = "tmp";
    for (int i = 0; i < 8; ++i) {
        name += kTempCharacters[pick(generator)];
    }
    name += suffix;
    return name;
}

} // namespace fv::audio::detail
