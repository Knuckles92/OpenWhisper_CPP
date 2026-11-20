#include "speech/incremental_session.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "base/hash.h"
#include "base/log.h"
#include "engine/error.h"
#include "pyport/number.h"
#include "speech/detail/crc32.h"
#include "speech/detail/incremental_checks.h"
#include "speech/detail/thread_name.h"
#include "speech/detail/wav_pcm.h"
#include "speech/file_windows.h"
#include "speech/messages.h"
#include "speech/window_decode.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.incremental_dictation"};

static_assert(std::endian::native == std::endian::little,
              "the tape's samples are compared with the WAV's little-endian bytes as they are");

// Reasons the early text is dropped (DictationSession._stale and finish's callers).
constexpr std::string_view kReloaded = "the engine was reloaded, released, or canceled";

// The followed audio cannot be proven to be the saved recording.
class Mismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

AudioClip to_clip(dsp::Window&& window) {
    AudioClip clip;
    clip.end = window.samples.size();
    clip.samples = std::make_shared<const std::vector<float>>(std::move(window.samples));
    clip.offset_s = window.start_s;
    return clip;
}

std::span<const std::uint8_t> bytes_of(std::span<const std::int16_t> samples) {
    return {reinterpret_cast<const std::uint8_t*>(samples.data()), samples.size_bytes()};
}

// os.path.normcase(os.path.abspath(path)): absolute, normalized and, on Windows, with
// backslashes and lower case.
std::filesystem::path::string_type normcase(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    auto text = absolute.lexically_normal().make_preferred().native();
#ifdef _WIN32
    for (auto& unit : text) {
        unit = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(unit)));
    }
#endif
    return text;
}

bool same_file(const std::filesystem::path& first, const std::filesystem::path& second) {
    return normcase(first) == normcase(second);
}

// A window's transcript moved to file time.
Transcript in_file_time(Transcript transcript, double offset_s) {
    for (Segment& segment : transcript.segments) {
        segment.start += offset_s;
        segment.end += offset_s;
    }
    for (Word& word : transcript.words) {
        word.start += offset_s;
        word.end += offset_s;
    }
    return transcript;
}

} // namespace

namespace detail {

std::optional<std::string> resampled_length_mismatch(std::int64_t total, std::int64_t frames,
                                                     int rate) {
    const double expected = static_cast<double>(frames * dsp::SAMPLE_RATE) / rate;
    if (std::abs(static_cast<double>(total) - expected) >= 1.0) {
        return "resampled " + std::to_string(total) + " samples where the file holds " +
               py::format_fixed(expected, 1);
    }
    return std::nullopt;
}

} // namespace detail

bool decodes_incrementally(std::string_view backend_id) {
    return backend_id == "parakeet";
}

ContextKey context_key(std::string_view language, std::span<const std::string> phrases) {
    return ContextKey{std::string(language), {phrases.begin(), phrases.end()}};
}

// --- IncrementalSession ---

std::unique_ptr<IncrementalSession>
IncrementalSession::create(SpeechService& service, std::shared_ptr<const audio::PcmTape> tape,
                           const RecognitionOptions& options, std::string backend_id,
                           std::vector<std::string> phrases, std::chrono::milliseconds poll) {
    if (!tape || !decodes_incrementally(backend_id)) {
        return nullptr;
    }
    if (service.readiness(Slot::Dictation).state != ReadyState::Ready) {
        return nullptr;
    }
    // The session rebuilds frames from raw PCM; only the recorder's own mono int16 format is
    // verified to resample exactly like the file.
    if (tape->channels() != 1) {
        return nullptr;
    }
    return std::unique_ptr<IncrementalSession>(
        new IncrementalSession(service, std::move(tape), options, std::move(phrases), poll));
}

IncrementalSession::IncrementalSession(SpeechService& service,
                                       std::shared_ptr<const audio::PcmTape> tape,
                                       RecognitionOptions options, std::vector<std::string> phrases,
                                       std::chrono::milliseconds poll)
    : service_(service), tape_(std::move(tape)), options_(std::move(options)),
      phrases_(std::move(phrases)), poll_(poll), generation_(service.generation(Slot::Dictation)) {}

IncrementalSession::~IncrementalSession() {
    request_stop();
    decodes_.cancel();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

void IncrementalSession::start() {
    if (thread_.joinable()) {
        return;
    }
    following_ = true;
    thread_ = std::thread([this] { run(); });
}

void IncrementalSession::run() {
    detail::set_current_thread_name("incremental-dictation");
    try {
        while (poll()) {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, poll_, [this] { return stop_.load(); });
        }
    } catch (const std::exception& error) {
        kLog.warning("Incremental dictation polling failed");
        invalidate(error.what());
    }
    following_ = false;
}

void IncrementalSession::discard(std::string reason) {
    invalidate(reason);
    request_stop();
    decodes_.cancel();
}

void IncrementalSession::request_stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
}

void IncrementalSession::add_decode_seconds(double seconds) {
    std::lock_guard lock(mutex_);
    decode_s_ += seconds;
}

void IncrementalSession::invalidate(const std::string& reason) {
    {
        std::lock_guard lock(mutex_);
        if (invalid_) {
            return;
        }
        invalid_ = reason;
    }
    kLog.info("Incremental dictation abandoned: {}", reason);
}

std::optional<std::string> IncrementalSession::stale() const {
    {
        std::lock_guard lock(mutex_);
        if (invalid_) {
            return invalid_;
        }
    }
    // Also how the thread ends promptly at shutdown or when a meeting releases the engine.
    if (service_.generation(Slot::Dictation) != generation_) {
        return std::string(kReloaded);
    }
    return std::nullopt;
}

bool IncrementalSession::poll() {
    const std::lock_guard polling(poll_mutex_);
    if (stop_ || invalid_reason()) {
        return false;
    }
    if (!resampler_) {
        // No window completes before a full window of audio exists, so a short dictation costs
        // one length check per poll.
        if (static_cast<double>(tape_->frames()) / tape_->rate() <
            static_cast<double>(dsp::MAX_SAMPLES) / dsp::SAMPLE_RATE) {
            return true;
        }
        resampler_.emplace(tape_->rate());
    }
    if (const auto reason = stale()) {
        invalidate(*reason);
        return false;
    }
    const std::int64_t frames = tape_->frames();
    if (frames > consumed_) {
        std::vector<std::int16_t> samples(static_cast<std::size_t>(frames - consumed_));
        tape_->view(consumed_, frames).copy_to(samples);
        crc_ = detail::crc32(bytes_of(samples), crc_);
        consumed_ = frames;
        push(samples);
    }
    decode_ready();
    return !stop_ && !invalid_reason();
}

void IncrementalSession::push(std::span<const std::int16_t> samples) {
    if (samples.empty()) {
        return;
    }
    std::vector<std::int16_t> resampled;
    resampler_->push(samples, resampled);
    for (dsp::Window& window : splitter_.push(resampled)) {
        ready_.push_back(to_clip(std::move(window)));
    }
}

DecodeResult IncrementalSession::decode(const AudioClip& window, Lane lane,
                                        const CancelToken& cancel) {
    DecodeJob job;
    job.input = window;
    job.options = options_;
    job.lane = lane;
    job.cancel = cancel;
    job.slot = Slot::Dictation;
    job.phrases = phrases_;
    return service_.run(std::move(job));
}

void IncrementalSession::decode_ready() {
    while (!ready_.empty() && !stop_) {
        if (const auto reason = stale()) {
            invalidate(*reason);
            return;
        }
        const AudioClip& window = ready_.front();
        const auto started = std::chrono::steady_clock::now();
        DecodeResult result;
        try {
            result = decode(window, Lane::Incremental, decodes_.token());
        } catch (const std::exception& error) {
            invalidate(std::string("early decode failed: ") + error.what());
            return;
        }
        // A reload during the decode would mix two engines' text.
        if (const auto reason = stale()) {
            invalidate(*reason);
            return;
        }
        if (result.generation != generation_) {
            invalidate(std::string(kReloaded));
            return;
        }
        const double offset_s = window.offset_s;
        const double length_s = static_cast<double>(window.end - window.begin) / window.rate;
        add_decode_seconds(result.decode_s);
        transcripts_.push_back(in_file_time(std::move(result.transcript), offset_s));
        transcripts_.back().audio_processed = offset_s + transcripts_.back().audio_processed;
        fingerprints_.push_back(fingerprint(window));
        ready_.erase(ready_.begin());
        std::size_t decoded = 0;
        {
            std::lock_guard lock(mutex_);
            decoded = ++early_;
        }
        kLog.info("Decoded window {} ({}-{} s) while recording in {} ms", decoded,
                  py::format_fixed(offset_s, 1), py::format_fixed(offset_s + length_s, 1),
                  py::format_fixed(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started)
                                       .count(),
                                   0));
    }
}

IncrementalSession::Fingerprint IncrementalSession::fingerprint(const AudioClip& clip) {
    const std::span<const float> samples(clip.samples->data() + clip.begin, clip.end - clip.begin);
    Sha256 hash;
    hash.update(std::as_bytes(samples));
    return Fingerprint{clip.offset_s, samples.size(), hash.hexdigest()};
}

std::vector<AudioClip>
IncrementalSession::verified_final_windows(const std::filesystem::path& wav) {
    // PyAV may resample a stream and a decoded file differently on some platforms even when the
    // source PCM and total sample count agree, so every window is checked against the file's.
    FileWindows file(wav);
    for (const Fingerprint& early : fingerprints_) {
        const auto saved = file.next();
        if (!saved || fingerprint(*saved) != early) {
            throw Mismatch("the saved file's early window differs");
        }
    }

    // _final_windows: the windows left at stop, once the saved file is verified.
    if (const auto reason = stale()) {
        throw Mismatch(*reason);
    }
    const detail::WavPcm saved = detail::read_wav_pcm(wav);
    if (saved.channels != 1 || saved.sample_width != 2 || saved.rate != tape_->rate()) {
        throw Mismatch("the saved WAV is (" + std::to_string(saved.channels) + ", " +
                       std::to_string(saved.sample_width) + ", " + std::to_string(saved.rate) +
                       "), not the recorder's format");
    }
    const auto followed = static_cast<std::size_t>(consumed_) * 2;
    if (saved.pcm.size() != static_cast<std::size_t>(saved.frames) * 2 ||
        saved.pcm.size() < followed) {
        throw Mismatch("the saved WAV is shorter than the audio followed");
    }
    const auto* pcm = reinterpret_cast<const std::uint8_t*>(saved.pcm.data());
    if (detail::crc32(std::span<const std::uint8_t>(pcm, followed)) != crc_) {
        throw Mismatch("the saved WAV does not start with the audio followed");
    }
    // The rest of the recording and the end padding, straight from the file.
    std::vector<std::int16_t> rest((saved.pcm.size() - followed) / 2);
    std::copy_n(pcm + followed, rest.size() * 2, reinterpret_cast<std::uint8_t*>(rest.data()));
    if (!resampler_) {
        resampler_.emplace(tape_->rate());
    }
    push(rest);
    finished_ = true;
    std::vector<std::int16_t> flushed;
    resampler_->finish(flushed);
    for (dsp::Window& window : splitter_.push(flushed)) {
        ready_.push_back(to_clip(std::move(window)));
    }
    for (dsp::Window& window : splitter_.finish()) {
        ready_.push_back(to_clip(std::move(window)));
    }
    if (const auto mismatch =
            detail::resampled_length_mismatch(splitter_.total(), saved.frames, saved.rate)) {
        throw Mismatch(*mismatch);
    }
    std::vector<AudioClip> live = std::exchange(ready_, {});

    for (const AudioClip& window : live) {
        const auto from_file = file.next();
        if (!from_file || fingerprint(*from_file) != fingerprint(window)) {
            throw Mismatch("the saved file's final window differs");
        }
    }
    if (file.next()) {
        throw Mismatch("the saved file has an extra window");
    }
    return live;
}

std::optional<Transcript> IncrementalSession::finish(const std::filesystem::path& wav,
                                                     const CancelToken& cancel) {
    assert_not_gui_thread();
    request_stop();
    // Wait out a window in flight, as the file decode would queue behind it; the job's cancel
    // ends that window instead of only flagging the job.
    const Subscription link = cancel.on_cancel([source = decodes_]() mutable { source.cancel(); });
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    const std::lock_guard polling(poll_mutex_);
    if (invalid_reason() || transcripts_.empty()) {
        return std::nullopt;
    }
    if (finished_) {
        invalidate("the recording was already finished");
        return std::nullopt;
    }
    const std::size_t early = transcripts_.size();
    const auto started = std::chrono::steady_clock::now();
    std::vector<Transcript> at_stop;
    try {
        const std::vector<AudioClip> remaining = verified_final_windows(wav);
        for (const AudioClip& window : remaining) {
            if (cancel.cancelled()) {
                throw EngineError(std::string(kTranscriptionCanceled));
            }
            DecodeResult result = decode(window, Lane::Final, cancel);
            if (result.generation != generation_) {
                throw Mismatch(std::string(kReloaded));
            }
            add_decode_seconds(result.decode_s);
            at_stop.push_back(in_file_time(std::move(result.transcript), window.offset_s));
            at_stop.back().audio_processed = window.offset_s + at_stop.back().audio_processed;
        }
        if (const auto reason = stale()) {
            throw Mismatch(*reason);
        }
    } catch (const std::exception& error) {
        // Cancels land here too; the file path then raises them itself.
        const std::string what = error.what();
        invalidate(what.empty() ? "the saved file could not be verified" : what);
        return std::nullopt;
    }
    kLog.info("Incremental dictation: {} window(s) decoded while recording, {} at stop in {} ms",
              early, at_stop.size(),
              py::format_fixed(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count(),
                               0));
    Transcript joined;
    std::vector<std::string> texts;
    for (std::vector<Transcript>* part : {&transcripts_, &at_stop}) {
        for (Transcript& piece : *part) {
            texts.push_back(std::move(piece.text));
            std::ranges::move(piece.segments, std::back_inserter(joined.segments));
            std::ranges::move(piece.words, std::back_inserter(joined.words));
            joined.audio_processed = piece.audio_processed;
        }
    }
    joined.text = join_texts(texts);
    transcripts_.clear();
    return joined;
}

bool IncrementalSession::matches(const RecognitionOptions& options,
                                 std::span<const std::string> phrases) const {
    return context_key(options.language, phrases) == context_key(options_.language, phrases_);
}

std::size_t IncrementalSession::early_windows() const {
    std::lock_guard lock(mutex_);
    return early_;
}

std::optional<std::string> IncrementalSession::invalid_reason() const {
    std::lock_guard lock(mutex_);
    return invalid_;
}

double IncrementalSession::decode_seconds() const {
    std::lock_guard lock(mutex_);
    return decode_s_;
}

bool IncrementalSession::following() const {
    return following_;
}

// --- IncrementalDictation ---

IncrementalDictation::IncrementalDictation(SpeechService& service) : service_(service) {}

IncrementalDictation::~IncrementalDictation() {
    std::shared_ptr<IncrementalSession> session;
    std::vector<std::shared_ptr<IncrementalSession>> retired;
    {
        std::lock_guard lock(mutex_);
        session = std::move(session_);
        retired = std::move(retired_);
    }
    if (session) {
        session->discard("discarded");
    }
}

void IncrementalDictation::start(std::shared_ptr<const audio::PcmTape> tape,
                                 const RecognitionOptions& options, std::string backend_id,
                                 std::filesystem::path recording_file,
                                 std::vector<std::string> phrases, std::chrono::milliseconds poll) {
    std::shared_ptr<IncrementalSession> session;
    try {
        session = IncrementalSession::create(service_, std::move(tape), options,
                                             std::move(backend_id), std::move(phrases), poll);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Incremental dictation could not start");
        session.reset();
    }
    std::shared_ptr<IncrementalSession> previous;
    {
        std::lock_guard lock(mutex_);
        previous = std::exchange(session_, session);
        recording_file_ = std::move(recording_file);
    }
    if (previous) {
        previous->discard("a new recording started");
    }
    retire(std::move(previous));
    if (session) {
        session->start();
    }
}

void IncrementalDictation::discard() {
    std::shared_ptr<IncrementalSession> session;
    {
        std::lock_guard lock(mutex_);
        session = std::exchange(session_, nullptr);
    }
    if (session) {
        session->discard("the recording was canceled");
        retire(std::move(session));
    }
}

DecodeResult IncrementalDictation::transcribe(const std::filesystem::path& path,
                                              const RecognitionOptions& options,
                                              const CancelToken& cancel,
                                              std::span<const std::string> phrases) {
    assert_not_gui_thread();
    retire(nullptr);
    std::shared_ptr<IncrementalSession> session;
    std::filesystem::path recording;
    {
        std::lock_guard lock(mutex_);
        session = std::exchange(session_, nullptr);
        recording = recording_file_;
    }
    if (session) {
        if (!same_file(path, recording)) {
            session->discard("the job is not this recording's");
        } else if (!session->matches(options, phrases)) {
            session->discard("the dictation's language or vocabulary changed");
        } else {
            const std::uint64_t generation = service_.generation(Slot::Dictation);
            if (auto transcript = session->finish(path, cancel)) {
                DecodeResult result;
                result.transcript = std::move(*transcript);
                result.decode_s = session->decode_seconds();
                result.generation = generation;
                return result;
            }
        }
        retire(std::move(session));
    }
    return decode_file(service_, path, options, Lane::Final, cancel);
}

std::shared_ptr<IncrementalSession> IncrementalDictation::session() const {
    std::lock_guard lock(mutex_);
    return session_;
}

void IncrementalDictation::retire(std::shared_ptr<IncrementalSession> session) {
    std::vector<std::shared_ptr<IncrementalSession>> done;
    {
        std::lock_guard lock(mutex_);
        if (session) {
            retired_.push_back(std::move(session));
        }
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (!(*it)->following()) {
                done.push_back(std::move(*it));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Their threads have returned: destroying them joins at once.
}

} // namespace fv::speech
