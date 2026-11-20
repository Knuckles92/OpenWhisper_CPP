#pragma once

// FFmpeg stays out of the public headers (architecture 10.2): only this library's sources see it.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fv::audio::detail {

struct FrameData {
    FrameData();
    ~FrameData();
    FrameData(const FrameData&) = delete;
    FrameData& operator=(const FrameData&) = delete;

    AVFrame* frame = nullptr;
};

// av_strerror's text for an FFmpeg error code.
std::string av_error_text(int code);

struct SwrDeleter {
    void operator()(SwrContext* context) const;
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

// A converter from (in_format, in_layout, in_rate) to interleaved s16 with out_layout at out_rate,
// configured as libavfilter's auto-inserted aresample configures one: every swresample option
// at its default. Throws std::runtime_error when FFmpeg refuses the formats.
SwrPtr make_converter(AVSampleFormat in_format, const AVChannelLayout& in_layout, int in_rate,
                      const AVChannelLayout& out_layout, int out_rate);

// Converts `samples` frames at `planes` through `converter` and appends the output frames'
// interleaved int16 to `out`; null planes with 0 frames flush. `out_channels` is the output's
// channel count.
void convert_append(SwrContext* converter, const std::uint8_t* const* planes, int samples,
                    int out_channels, std::vector<std::int16_t>& out);

} // namespace fv::audio::detail
