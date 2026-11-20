#include "dictation/uploads.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "dictation/cleaner_port.h"
#include "dictation/messages.h"
#include "pyport/text.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.transcription"};

// sum() of floats in Python 3.12: Neumaier's compensated sum, the compensation added at the end
// only when it is non-zero and finite.
class PythonSum {
public:
    void add(double value) {
        const double t = total_ + value;
        if (std::fabs(total_) >= std::fabs(value)) {
            compensation_ += (total_ - t) + value;
        } else {
            compensation_ += (value - t) + total_;
        }
        total_ = t;
    }
    double result() const {
        if (compensation_ != 0.0 && std::isfinite(compensation_)) {
            return total_ + compensation_;
        }
        return total_;
    }

private:
    double total_ = 0.0;
    double compensation_ = 0.0;
};

std::optional<std::int64_t> file_size_or_none(const std::string& audio_path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_from_utf8(audio_path), error);
    if (error) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(size);
}

std::vector<std::pair<std::string, std::string>> sections(const BatchResult& result, bool raw) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(result.items.size());
    for (const BatchItemResult& item : result.items) {
        out.emplace_back(item.item.source_name(), batch_line(item, raw));
    }
    return out;
}

} // namespace

std::string batch_line(const BatchItemResult& item, bool raw) {
    if (item.error && !item.error->empty()) {
        return "Error: " + *item.error;
    }
    if (raw && item.raw_text && !item.raw_text->empty()) {
        return *item.raw_text;
    }
    std::string text = py::strip(item.text);
    return text.empty() ? std::string(EMPTY_ASR_MESSAGE) : text;
}

std::pair<std::string, std::optional<std::string>> batch_display(const BatchResult& result,
                                                                 bool is_empty) {
    if (result.request.combine()) {
        if (is_empty) {
            return {std::string(EMPTY_ASR_MESSAGE), std::nullopt};
        }
        return {result.combined_text.value_or(std::string()), result.combined_raw_text};
    }
    const std::string display = format_batch_transcript(sections(result, false));
    std::optional<std::string> raw_display;
    for (const BatchItemResult& item : result.items) {
        if (item.raw_text && !item.raw_text->empty()) {
            raw_display = format_batch_transcript(sections(result, true));
            break;
        }
    }
    return {display, raw_display};
}

std::string batch_status(const BatchResult& result, std::size_t saveable, bool canceled) {
    std::string status;
    if (canceled) {
        const std::string kept =
            result.request.combine()
                ? std::string("the transcript")
                : std::format("{} of {} files", saveable, result.request.items.size());
        status = "Canceled — kept " + kept;
    } else {
        status = "Ready";
    }
    return status + cleanup_status_suffix(result.cleanup_error);
}

BatchRunner::BatchRunner(BatchSteps steps, const Clock& clock)
    : steps_(std::move(steps)), clock_(clock) {}

std::optional<BatchResult> BatchRunner::run(const BatchUploadRequest& request) const {
    const auto seconds_since = [this](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double>(clock_.steady() - start).count();
    };
    const auto started = clock_.steady();
    const int total = static_cast<int>(request.items.size());
    const bool combine = request.combine();
    std::vector<BatchItemResult> results;
    std::vector<std::string> raw_parts;
    bool canceled = false;
    int position = 0;
    for (const BatchItem& item : request.items) {
        ++position;
        if (steps_.cancel_requested()) {
            canceled = true;
            break;
        }
        const std::string name = item.source_name();
        steps_.progress(position, total, name);
        const auto item_started = clock_.steady();
        const auto file_size = file_size_or_none(item.audio_path);
        std::string raw;
        try {
            raw = steps_.transcribe(item);
        } catch (const std::exception& error) {
            if (steps_.cancel_requested()) {
                canceled = true;
                break;
            }
            if (combine) {
                // One missing part invalidates a stitched transcript.
                throw;
            }
            kLog.error_with(error, std::format("Batch file {} failed", name));
            BatchItemResult failed;
            failed.item = item;
            failed.error = error.what();
            failed.elapsed_s = seconds_since(item_started);
            failed.file_size = file_size;
            results.push_back(std::move(failed));
            steps_.item_finished(position, false, "");
            continue;
        }
        const double asr_elapsed = seconds_since(item_started);
        // A combined job has one transcript, so its rows get no text of their own: the per-file
        // ASR is an unfinished part of it.
        std::string own_transcript;
        BatchItemResult done;
        done.item = item;
        done.elapsed_s = asr_elapsed;
        done.file_size = file_size;
        if (combine) {
            raw_parts.push_back(raw);
            done.text = raw;
        } else {
            CleanedText cleaned = steps_.clean(raw, request.batch_context(&item), std::nullopt);
            done.text = cleaned.text;
            done.raw_text = cleaned.raw_text;
            if (cleaned.info) {
                done.cleanup_provider = cleaned.info->provider;
                done.cleanup_model = cleaned.info->model;
                done.cleanup_elapsed_s = cleaned.info->elapsed_s;
            }
            own_transcript = py::strip(cleaned.text);
        }
        results.push_back(std::move(done));
        steps_.item_finished(position, true, own_transcript);
    }

    const double total_elapsed = seconds_since(started);
    PythonSum transcription_time;
    for (const BatchItemResult& result : results) {
        transcription_time.add(result.elapsed_s);
    }
    BatchResult result;
    result.request = request;
    result.total_elapsed_s = total_elapsed;
    result.transcription_time_s = transcription_time.result();
    if (combine) {
        if (canceled) {
            return std::nullopt;
        }
        const std::string joined = join_raw_parts(raw_parts);
        CleanedText cleaned =
            steps_.clean(joined, request.batch_context(), TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S);
        result.items = std::move(results);
        result.combined_text = cleaned.text;
        result.combined_raw_text = cleaned.raw_text;
        if (cleaned.info) {
            result.combined_cleanup_provider = cleaned.info->provider;
            result.combined_cleanup_model = cleaned.info->model;
            result.cleanup_time_s = cleaned.info->elapsed_s;
        }
        result.cleanup_error = cleaned.failure;
        return result;
    }
    PythonSum cleanup_time;
    for (const BatchItemResult& item : results) {
        cleanup_time.add(item.cleanup_elapsed_s);
    }
    result.items = std::move(results);
    result.cleanup_time_s = cleanup_time.result();
    return result;
}

} // namespace fv::dictation
