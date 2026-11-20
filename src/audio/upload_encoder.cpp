#include "audio/upload_encoder.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "audio/detail/ffmpeg.h"

namespace fv::audio {
namespace {

void require(int result) {
    if (result < 0) {
        throw std::runtime_error(detail::av_error_text(result));
    }
}

struct Output {
    AVFormatContext* format = nullptr;

    ~Output() {
        if (format && format->pb) {
            std::uint8_t* bytes = nullptr;
            avio_close_dyn_buf(format->pb, &bytes);
            av_free(bytes);
        }
        avformat_free_context(format);
    }

    std::string finish() {
        std::uint8_t* raw = nullptr;
        const auto size = avio_close_dyn_buf(format->pb, &raw);
        format->pb = nullptr;
        const std::unique_ptr<std::uint8_t, decltype(&av_free)> bytes(raw, av_free);
        require(size);
        if (size == 0) {
            throw std::runtime_error("The FLAC encoder returned empty audio.");
        }
        return {reinterpret_cast<const char*>(bytes.get()), static_cast<std::size_t>(size)};
    }
};

void free_codec(AVCodecContext* codec) {
    avcodec_free_context(&codec);
}
void free_packet(AVPacket* packet) {
    av_packet_free(&packet);
}

} // namespace

bool has_upload_flac() {
    return avcodec_find_encoder(AV_CODEC_ID_FLAC) && av_guess_format("flac", nullptr, nullptr);
}

std::string encode_upload_flac(std::span<const std::int16_t> samples, int sample_rate) {
    if (samples.empty() || sample_rate <= 0) {
        throw std::invalid_argument("No audio samples are available for encoding.");
    }
    const auto* implementation = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!implementation) {
        throw std::runtime_error("The FLAC audio encoder is unavailable.");
    }
    Output output;
    require(avformat_alloc_output_context2(&output.format, nullptr, "flac", nullptr));
    const std::unique_ptr<AVCodecContext, decltype(&free_codec)> codec(
        avcodec_alloc_context3(implementation), free_codec);
    const std::unique_ptr<AVPacket, decltype(&free_packet)> packet(av_packet_alloc(), free_packet);
    if (!codec || !packet) {
        throw std::bad_alloc();
    }
    codec->sample_rate = sample_rate;
    codec->sample_fmt = AV_SAMPLE_FMT_S16;
    codec->time_base = AVRational{1, sample_rate};
    av_channel_layout_default(&codec->ch_layout, 1);
    require(avcodec_open2(codec.get(), implementation, nullptr));
    auto* stream = avformat_new_stream(output.format, nullptr);
    if (!stream) {
        throw std::bad_alloc();
    }
    stream->time_base = codec->time_base;
    require(avcodec_parameters_from_context(stream->codecpar, codec.get()));
    require(avio_open_dyn_buf(&output.format->pb));
    require(avformat_write_header(output.format, nullptr));

    const auto drain = [&] {
        for (;;) {
            const auto result = avcodec_receive_packet(codec.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            require(result);
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            require(av_interleaved_write_frame(output.format, packet.get()));
        }
    };
    const auto frame_size =
        static_cast<std::size_t>(codec->frame_size > 0 ? codec->frame_size : 4096);
    for (std::size_t offset = 0; offset < samples.size();) {
        const auto count = std::min(frame_size, samples.size() - offset);
        detail::FrameData storage;
        auto* frame = storage.frame;
        frame->format = codec->sample_fmt;
        frame->sample_rate = sample_rate;
        require(av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout));
        frame->nb_samples = static_cast<int>(count);
        frame->pts = static_cast<std::int64_t>(offset);
        require(av_frame_get_buffer(frame, 0));
        std::memcpy(frame->data[0], samples.data() + offset, count * sizeof(std::int16_t));
        require(avcodec_send_frame(codec.get(), frame));
        drain();
        offset += count;
    }
    require(avcodec_send_frame(codec.get(), nullptr));
    drain();
    require(av_write_trailer(output.format));
    return output.finish();
}

} // namespace fv::audio
