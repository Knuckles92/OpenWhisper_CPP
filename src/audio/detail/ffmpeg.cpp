#include "audio/detail/ffmpeg.h"

#include <array>
#include <stdexcept>

namespace fv::audio::detail {

FrameData::FrameData() : frame(av_frame_alloc()) {
    if (frame == nullptr) {
        throw std::bad_alloc();
    }
}

FrameData::~FrameData() {
    av_frame_free(&frame);
}

std::string av_error_text(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    if (av_strerror(code, text.data(), text.size()) < 0) {
        return "error " + std::to_string(code);
    }
    return text.data();
}

void SwrDeleter::operator()(SwrContext* context) const {
    swr_free(&context);
}

SwrPtr make_converter(AVSampleFormat in_format, const AVChannelLayout& in_layout, int in_rate,
                      const AVChannelLayout& out_layout, int out_rate) {
    SwrContext* raw = nullptr;
    int result = swr_alloc_set_opts2(&raw, &out_layout, AV_SAMPLE_FMT_S16, out_rate, &in_layout,
                                     in_format, in_rate, 0, nullptr);
    SwrPtr converter(raw);
    if (result < 0) {
        throw std::runtime_error("The audio converter could not be set up: " +
                                 av_error_text(result));
    }
    result = swr_init(converter.get());
    if (result < 0) {
        throw std::runtime_error("The audio converter could not be set up: " +
                                 av_error_text(result));
    }
    return converter;
}

void convert_append(SwrContext* converter, const std::uint8_t* const* planes, int samples,
                    int out_channels, std::vector<std::int16_t>& out) {
    // A flush may need several calls when the delay exceeds the room asked for.
    for (;;) {
        const int room = swr_get_out_samples(converter, samples);
        if (room < 0) {
            throw std::runtime_error("The audio converter failed: " + av_error_text(room));
        }
        if (room == 0) {
            return;
        }
        const std::size_t start = out.size();
        out.resize(start + static_cast<std::size_t>(room) * static_cast<std::size_t>(out_channels));
        auto* target = reinterpret_cast<std::uint8_t*>(out.data() + start);
        const int made = swr_convert(converter, &target, room, planes, samples);
        if (made < 0) {
            out.resize(start);
            throw std::runtime_error("The audio converter failed: " + av_error_text(made));
        }
        out.resize(start + static_cast<std::size_t>(made) * static_cast<std::size_t>(out_channels));
        if (planes != nullptr || made == 0) {
            return;
        }
    }
}

} // namespace fv::audio::detail
