#include "audio/file_decoder.h"

#include <cerrno>
#include <format>
#include <string>
#include <utility>

#include "audio/detail/ffmpeg.h"
#include "audio/wav_metadata.h"
#include "base/utf8.h"

namespace fv::audio {

DecodedFrame::DecodedFrame() : data_(std::make_unique<detail::FrameData>()) {}
DecodedFrame::~DecodedFrame() = default;
DecodedFrame::DecodedFrame(DecodedFrame&&) noexcept = default;
DecodedFrame& DecodedFrame::operator=(DecodedFrame&&) noexcept = default;

int DecodedFrame::rate() const {
    return data_ ? data_->frame->sample_rate : 0;
}

int DecodedFrame::channels() const {
    return data_ ? data_->frame->ch_layout.nb_channels : 0;
}

std::size_t DecodedFrame::frames() const {
    return data_ ? static_cast<std::size_t>(data_->frame->nb_samples) : 0;
}

bool DecodedFrame::empty() const {
    return frames() == 0;
}

namespace detail {

namespace {

// Before it decodes 16-bit PCM from a WAV, FFmpeg searches the first 32 packets for a
// compressed stream carried inside it (DTS or AC-3), which takes two channels. A one-channel
// recording has nothing to find; this ends the search after one packet.
constexpr const char* kMonoPcmProbeSize = "4096";

std::string file_label(const std::filesystem::path& path) {
    return path_to_utf8(path.filename());
}

} // namespace

struct DecoderImpl {
    std::filesystem::path path;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    int stream_index = -1;
    bool draining = false;
    bool ended = false;
    // read_s16's converter and the input it was made for.
    SwrPtr to_s16;
    AVSampleFormat s16_from_format = AV_SAMPLE_FMT_NONE;
    AVChannelLayout s16_from_layout{};
    int s16_from_rate = 0;
    DecodedFrame scratch;

    ~DecoderImpl() {
        av_channel_layout_uninit(&s16_from_layout);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    [[noreturn]] void fail(std::string_view what, int code) const {
        std::string message =
            std::format("{} {} ({}).", what, file_label(path), av_error_text(code));
        // PyAV raises its PermissionError for both errno values.
        if (code == AVERROR(EACCES) || code == AVERROR(EPERM)) {
            throw AudioFileDenied(message);
        }
        throw AudioDecodeError(message);
    }

    void open(const std::filesystem::path& file) {
        path = file;
        AVDictionary* options = nullptr;
        if (is_mono_pcm16(file)) {
            av_dict_set(&options, "probesize", kMonoPcmProbeSize, 0);
        }
        const std::string utf8 = path_to_utf8(file);
        int result = avformat_open_input(&format, utf8.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (result < 0) {
            fail("Could not open the audio file", result);
        }
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0) {
            fail("Could not read the audio file", result);
        }
        // container.decode(audio=0): the first audio stream in the container's order.
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                stream_index = static_cast<int>(i);
                break;
            }
        }
        if (stream_index < 0) {
            throw AudioDecodeError(
                std::format("The file {} has no audio track.", file_label(path)));
        }
        const AVStream* stream = format->streams[stream_index];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            throw AudioDecodeError(
                std::format("No decoder can read the audio in {}.", file_label(path)));
        }
        codec = avcodec_alloc_context3(decoder);
        packet = av_packet_alloc();
        if (codec == nullptr || packet == nullptr) {
            throw std::bad_alloc();
        }
        result = avcodec_parameters_to_context(codec, stream->codecpar);
        if (result < 0) {
            fail("Could not set up the decoder for", result);
        }
        // As PyAV sets it for a stream's decoder.
        codec->pkt_timebase = stream->time_base;
        result = avcodec_open2(codec, decoder, nullptr);
        if (result < 0) {
            fail("Could not open the decoder for", result);
        }
    }

    bool next(DecodedFrame& decoded) {
        if (!decoded.data_) {
            decoded.data_ = std::make_unique<FrameData>();
        }
        AVFrame* frame = decoded.data_->frame;
        while (!ended) {
            int result = avcodec_receive_frame(codec, frame);
            if (result == 0) {
                frame->pts = AV_NOPTS_VALUE;
                return true;
            }
            if (result == AVERROR_EOF) {
                ended = true;
                break;
            }
            if (result != AVERROR(EAGAIN)) {
                fail("Could not decode the audio in", result);
            }
            if (draining) {
                ended = true;
                break;
            }
            result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                draining = true;
                result = avcodec_send_packet(codec, nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    fail("Could not decode the audio in", result);
                }
                continue;
            }
            if (result < 0) {
                fail("Could not read the audio in", result);
            }
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            result = avcodec_send_packet(codec, packet);
            av_packet_unref(packet);
            if (result < 0 && result != AVERROR(EAGAIN)) {
                fail("Could not decode the audio in", result);
            }
        }
        av_frame_unref(frame);
        return false;
    }

    bool read_s16(std::vector<std::int16_t>& out) {
        if (!next(scratch)) {
            return false;
        }
        const AVFrame* frame = scratch.data_->frame;
        const auto format_of_frame = static_cast<AVSampleFormat>(frame->format);
        if (!to_s16 || format_of_frame != s16_from_format || frame->sample_rate != s16_from_rate ||
            av_channel_layout_compare(&frame->ch_layout, &s16_from_layout) != 0) {
            to_s16 = make_converter(format_of_frame, frame->ch_layout, frame->sample_rate,
                                    frame->ch_layout, frame->sample_rate);
            s16_from_format = format_of_frame;
            s16_from_rate = frame->sample_rate;
            av_channel_layout_uninit(&s16_from_layout);
            if (av_channel_layout_copy(&s16_from_layout, &frame->ch_layout) < 0) {
                throw std::bad_alloc();
            }
        }
        // Same rate: the converter holds nothing back between frames.
        convert_append(to_s16.get(), const_cast<const std::uint8_t* const*>(frame->extended_data),
                       frame->nb_samples, frame->ch_layout.nb_channels, out);
        return true;
    }
};

} // namespace detail

AudioFileDecoder::AudioFileDecoder(const std::filesystem::path& path)
    : impl_(std::make_unique<detail::DecoderImpl>()) {
    impl_->open(path);
}

AudioFileDecoder::~AudioFileDecoder() = default;
AudioFileDecoder::AudioFileDecoder(AudioFileDecoder&&) noexcept = default;
AudioFileDecoder& AudioFileDecoder::operator=(AudioFileDecoder&&) noexcept = default;

int AudioFileDecoder::rate() const {
    return impl_->codec->sample_rate;
}

int AudioFileDecoder::channels() const {
    return impl_->codec->ch_layout.nb_channels;
}

double AudioFileDecoder::duration_s() const {
    const AVStream* stream = impl_->format->streams[impl_->stream_index];
    if (stream->duration != AV_NOPTS_VALUE) {
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }
    if (impl_->format->duration != AV_NOPTS_VALUE) {
        return static_cast<double>(impl_->format->duration) / AV_TIME_BASE;
    }
    return -1.0;
}

bool AudioFileDecoder::next(DecodedFrame& frame) {
    return impl_->next(frame);
}

bool AudioFileDecoder::read_s16(std::vector<std::int16_t>& out) {
    return impl_->read_s16(out);
}

} // namespace fv::audio
