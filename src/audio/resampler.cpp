#include "audio/resampler.h"

#include <stdexcept>

#include "audio/detail/ffmpeg.h"
#include "audio/file_decoder.h"

namespace fv::audio {

namespace detail {

struct ResamplerImpl {
    // Set by the first input; `configured` stays false until then.
    bool configured = false;
    bool finished = false;
    // PyAV returns a frame that already is s16 mono 16 kHz unchanged (is_passthrough).
    bool passthrough = false;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;
    AVChannelLayout layout{};
    int rate = 0;
    SwrPtr converter;

    ~ResamplerImpl() { av_channel_layout_uninit(&layout); }

    void configure(AVSampleFormat in_format, const AVChannelLayout& in_layout, int in_rate) {
        format = in_format;
        if (av_channel_layout_copy(&layout, &in_layout) < 0) {
            throw std::bad_alloc();
        }
        rate = in_rate;
        configured = true;
        const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        passthrough = in_format == AV_SAMPLE_FMT_S16 &&
                      av_channel_layout_compare(&in_layout, &mono) == 0 &&
                      in_rate == Resampler16k::kRate;
        if (!passthrough) {
            converter = make_converter(in_format, in_layout, in_rate, mono, Resampler16k::kRate);
        }
    }

    static const AVFrame* frame_of(const DecodedFrame& decoded) { return decoded.data_->frame; }

    void check_open() const {
        if (finished) {
            throw std::logic_error("Resampler16k was already finished");
        }
    }
};

} // namespace detail

namespace {

AVChannelLayout default_layout(int channels) {
    if (channels <= 0) {
        throw std::invalid_argument("Resampler16k needs at least one input channel");
    }
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    return layout;
}

} // namespace

Resampler16k::Resampler16k(int input_rate) : Resampler16k(input_rate, 1) {}

Resampler16k::Resampler16k(int input_rate, int input_channels)
    : impl_(std::make_unique<detail::ResamplerImpl>()) {
    if (input_rate <= 0) {
        throw std::invalid_argument("Resampler16k needs a positive input rate");
    }
    AVChannelLayout layout = default_layout(input_channels);
    impl_->configure(AV_SAMPLE_FMT_S16, layout, input_rate);
    av_channel_layout_uninit(&layout);
}

Resampler16k::Resampler16k() : impl_(std::make_unique<detail::ResamplerImpl>()) {}

Resampler16k::~Resampler16k() = default;
Resampler16k::Resampler16k(Resampler16k&&) noexcept = default;
Resampler16k& Resampler16k::operator=(Resampler16k&&) noexcept = default;

void Resampler16k::push(std::span<const std::int16_t> samples, std::vector<std::int16_t>& out) {
    impl_->check_open();
    if (!impl_->configured || impl_->format != AV_SAMPLE_FMT_S16) {
        throw std::logic_error("Resampler16k::push(span) needs a resampler built for int16 input");
    }
    const int channels = impl_->layout.nb_channels;
    if (samples.size() % static_cast<std::size_t>(channels) != 0) {
        throw std::invalid_argument("Resampler16k::push takes whole frames");
    }
    if (samples.empty()) {
        return;
    }
    if (impl_->passthrough) {
        out.insert(out.end(), samples.begin(), samples.end());
        return;
    }
    const auto* plane = reinterpret_cast<const std::uint8_t*>(samples.data());
    detail::convert_append(impl_->converter.get(), &plane,
                           static_cast<int>(samples.size() / static_cast<std::size_t>(channels)), 1,
                           out);
}

void Resampler16k::push(const DecodedFrame& decoded, std::vector<std::int16_t>& out) {
    impl_->check_open();
    if (decoded.empty()) {
        return;
    }
    const AVFrame* frame = detail::ResamplerImpl::frame_of(decoded);
    const auto format = static_cast<AVSampleFormat>(frame->format);
    if (!impl_->configured) {
        impl_->configure(format, frame->ch_layout, frame->sample_rate);
    } else if (format != impl_->format ||
               av_channel_layout_compare(&frame->ch_layout, &impl_->layout) != 0 ||
               frame->sample_rate != impl_->rate) {
        throw std::invalid_argument("Frame does not match AudioResampler setup.");
    }
    if (impl_->passthrough) {
        const auto* samples = reinterpret_cast<const std::int16_t*>(frame->extended_data[0]);
        out.insert(out.end(), samples, samples + frame->nb_samples);
        return;
    }
    detail::convert_append(impl_->converter.get(),
                           const_cast<const std::uint8_t* const*>(frame->extended_data),
                           frame->nb_samples, 1, out);
}

void Resampler16k::finish(std::vector<std::int16_t>& out) {
    if (impl_->finished) {
        return;
    }
    impl_->finished = true;
    if (impl_->converter) {
        detail::convert_append(impl_->converter.get(), nullptr, 0, 1, out);
    }
}

} // namespace fv::audio
