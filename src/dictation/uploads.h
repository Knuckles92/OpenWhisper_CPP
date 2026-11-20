#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "base/clock.h"
#include "dictation/batch.h"
#include "dictation/cleanup_info.h"

// The multi-file upload job of the transcription runtime (services/runtime/transcription.py
// transcribe_batch and the _batch_* builders): the strictly serial worker loop and the texts a
// finished batch shows. TranscriptionRuntime::upload_audio_files drives it.
namespace fv::dictation {

// "Error: {error}" for a failed file, else its stripped words (raw: its raw text first), else
// EMPTY_ASR_MESSAGE (_batch_line). Thread-safe.
std::string batch_line(const BatchItemResult& item, bool raw = false);

// The transcript view's text and raw text for a finished batch (_batch_display): a combined
// batch shows its one transcript (EMPTY_ASR_MESSAGE when `is_empty`) and its raw text; a
// separate one shows a "## {name}" section per file, and a raw twin only when a file has raw
// text. Thread-safe.
std::pair<std::string, std::optional<std::string>> batch_display(const BatchResult& result,
                                                                 bool is_empty);

// The status line once a batch is saved (_batch_status): "Ready", or "Canceled — kept {the
// transcript | k of n files}" with `saveable` files kept, followed by cleanup_status_suffix of
// the combined cleanup's error. Thread-safe.
std::string batch_status(const BatchResult& result, std::size_t saveable, bool canceled);

// _maybe_cleanup_transcript's (text, raw_text, info), and why the pass fell back to raw text
// (_last_cleanup_failure). Affinity: value type.
struct CleanedText {
    std::string text;
    std::optional<std::string> raw_text;
    std::optional<CleanupInfo> info;
    std::optional<std::string> failure;
};

// What one batch reaches; every function is called on the thread that runs the batch.
struct BatchSteps {
    // The job's Cancel was requested.
    std::function<bool()> cancel_requested;
    // ASR of one file (_transcribe_path); throws on failure.
    std::function<std::string(const BatchItem& item)> transcribe;
    // Cleanup of `raw` with the batch's description and a per-request timeout (seconds).
    std::function<CleanedText(const std::string& raw, const std::optional<std::string>& context,
                              std::optional<double> timeout_s)>
        clean;
    // batch_progress: (1-based position, total, source name) as each file starts.
    std::function<void(int position, int total, const std::string& name)> progress;
    // batch_item_finished: (position, succeeded, the file's own stripped transcript) as each
    // file ends; the text is empty on failure and in a combined batch.
    std::function<void(int position, bool ok, const std::string& text)> item_finished;
};

// transcribe_batch's loop: the files one after another, a Cancel checked before each. A
// separate batch cleans each file on its own and keeps going past a failed file; a combined one
// (sequential, or custom with combine) stops at its first error, which propagates, and cleans
// the joined text once with TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S. A Cancel (before a file, or a
// failed file with the Cancel set) ends the loop: a separate batch returns what finished, a
// combined one returns nullopt ("Transcription canceled") without reaching its cleanup.
// Affinity: one thread at a time; blocks on the decodes and cleanups it runs.
class BatchRunner {
public:
    BatchRunner(BatchSteps steps, const Clock& clock);
    std::optional<BatchResult> run(const BatchUploadRequest& request) const;

private:
    BatchSteps steps_;
    const Clock& clock_;
};

} // namespace fv::dictation
