#include "audio/wav_writer.h"

#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "audio/detail/raw_file.h"
#include "audio/testing/origination_stamp.h"
#include "audio/wav_metadata.h"
#include "base/log.h"

namespace fv::audio {

namespace {

const log::Logger kLog("services.recorder");

constexpr std::uint16_t kWaveFormatPcm = 1;
constexpr std::uint64_t kHeaderBytes = 44;

// The stamp a test put in place of append_origination, and its lock.
struct StampSlot {
    std::mutex mutex;
    testing::OriginationStamp stamp;
};

StampSlot& stamp_slot() {
    static StampSlot slot;
    return slot;
}

void stamp_origination(RiffFile& wav, std::chrono::local_seconds when) {
    testing::OriginationStamp replaced;
    {
        const std::lock_guard lock(stamp_slot().mutex);
        replaced = stamp_slot().stamp;
    }
    if (replaced) {
        replaced(wav, when);
    } else {
        append_origination(wav, when);
    }
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>(value >> 8));
}

void put_tag(std::vector<std::byte>& out, std::string_view tag) {
    for (const char c : tag) {
        out.push_back(static_cast<std::byte>(c));
    }
}

// wave.Wave_write._write_header with the data length known.
std::vector<std::byte> header(const WavWriter::Format& format, std::uint32_t data_length) {
    std::vector<std::byte> bytes;
    bytes.reserve(kHeaderBytes);
    put_tag(bytes, "RIFF");
    put_u32(bytes, 36 + data_length);
    put_tag(bytes, "WAVE");
    put_tag(bytes, "fmt ");
    put_u32(bytes, 16);
    put_u16(bytes, kWaveFormatPcm);
    put_u16(bytes, static_cast<std::uint16_t>(format.channels));
    put_u32(bytes, static_cast<std::uint32_t>(format.rate));
    put_u32(bytes, static_cast<std::uint32_t>(format.channels * format.rate * format.sample_width));
    put_u16(bytes, static_cast<std::uint16_t>(format.channels * format.sample_width));
    put_u16(bytes, static_cast<std::uint16_t>(format.sample_width * 8));
    put_tag(bytes, "data");
    put_u32(bytes, data_length);
    return bytes;
}

} // namespace

namespace detail {

struct WavWriterImpl {
    std::filesystem::path target;
    std::filesystem::path temporary;
    WavWriter::Format format;
    RawFile file;
    std::uint64_t data_bytes = 0;
    bool done = false;

    void remove_temporary() noexcept {
        try {
            file.close();
        } catch (...) { // NOLINT(bugprone-empty-catch): the file is being thrown away
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
};

} // namespace detail

WavWriter::WavWriter(std::filesystem::path target, Format format)
    : impl_(std::make_unique<detail::WavWriterImpl>()) {
    if (format.rate <= 0 || format.channels <= 0 || format.sample_width <= 0) {
        throw std::invalid_argument("WavWriter needs a positive rate, channel count and width");
    }
    impl_->target = std::filesystem::absolute(target);
    impl_->format = format;
    std::filesystem::path directory = impl_->target.parent_path();
    if (directory.empty()) {
        directory = std::filesystem::current_path();
    }
    impl_->file = detail::RawFile::create_temp(directory, ".wav", impl_->temporary);
    try {
        impl_->file.write_all(header(format, 0));
    } catch (...) {
        impl_->remove_temporary();
        throw;
    }
}

WavWriter::~WavWriter() {
    if (impl_ && !impl_->done) {
        impl_->remove_temporary();
    }
}

WavWriter::WavWriter(WavWriter&&) noexcept = default;
WavWriter& WavWriter::operator=(WavWriter&&) noexcept = default;

void WavWriter::write(std::span<const std::byte> frames) {
    if (impl_->done) {
        throw std::logic_error("WavWriter was already committed or discarded");
    }
    impl_->file.write_all(frames);
    impl_->data_bytes += frames.size();
}

void WavWriter::write(std::span<const std::int16_t> samples) {
    // WAV is little-endian, as every platform OpenWhisper_CPP builds for.
    write(std::as_bytes(samples));
}

std::uint64_t WavWriter::data_bytes() const {
    return impl_->data_bytes;
}

const std::filesystem::path& WavWriter::temporary_path() const {
    return impl_->temporary;
}

void WavWriter::commit(std::optional<std::chrono::local_seconds> origination) {
    if (impl_->done) {
        throw std::logic_error("WavWriter was already committed or discarded");
    }
    try {
        if (impl_->data_bytes > 0xFFFFFFFFu - 36) {
            // struct.pack("<L") refuses it in Python's wave writer.
            throw std::system_error(std::make_error_code(std::errc::file_too_large),
                                    "The recording is too large for a WAV file");
        }
        const auto length = static_cast<std::uint32_t>(impl_->data_bytes);
        impl_->file.seek(0);
        impl_->file.write_all(header(impl_->format, length));
        impl_->file.seek_end();
        if (origination) {
            try {
                stamp_origination(impl_->file, *origination);
            } catch (const std::exception& error) {
                kLog.warning("Failed to stamp WAV origination metadata ({})", error.what());
            }
        }
        impl_->file.sync();
        impl_->file.close();
        detail::replace_file(impl_->temporary, impl_->target);
        impl_->done = true;
    } catch (...) {
        impl_->done = true;
        impl_->remove_temporary();
        throw;
    }
}

void WavWriter::discard() {
    if (!impl_->done) {
        impl_->done = true;
        impl_->remove_temporary();
    }
}

namespace testing {

OriginationStamp exchange_origination_stamp(OriginationStamp replacement) {
    const std::lock_guard lock(stamp_slot().mutex);
    return std::exchange(stamp_slot().stamp, std::move(replacement));
}

} // namespace testing

} // namespace fv::audio
