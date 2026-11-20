#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "diarize/embedder.h"

// Online speaker clustering for the loopback ("Others") channel. Each transcript segment gets an
// embedding and joins the nearest speaker cluster by cosine similarity, or seeds a new "Speaker N"
// participant; a periodic agglomerative re-cluster corrects early mistakes and relabels the
// segments whose assignment changed. Human pins are authoritative: a pinned segment is never
// relabelled and dominates the cluster-to-participant mapping.
//
// The re-cluster runs over a bounded working set: the most recent kReclusterWindow embeddings,
// every pinned one, and one centroid "anchor" per participant summarising everything older.
// Average linkage is O(N^3); bounding the set makes each pass cost the same however long the
// meeting is. Segments older than the window are no longer revisited, but their identity still
// votes through the anchors.
//
// Any internal failure degrades the diarizer once: assign returns nullopt from then on and the
// meeting keeps channel-level Me/Others labels.
namespace fv::diarize {

// Cosine similarity at or above which a segment joins an existing cluster.
inline constexpr double kAssignThreshold = 0.62;
// Weight of a new embedding in a cluster centroid's exponential moving average.
inline constexpr double kEmaAlpha = 0.15;
// Beyond this many clusters a segment joins the nearest one anyway.
inline constexpr std::size_t kMaxClusters = 10;
// Re-cluster after this many new embeddings, or after kReclusterEvery, whichever comes first
// (checked in assign).
inline constexpr int kReclusterEveryN = 25;
inline constexpr std::chrono::seconds kReclusterEvery{60};
// Average linkage stops merging once the closest clusters are farther apart than this cosine
// distance.
inline constexpr double kMergeDistance = 0.38;
// A pinned segment counts this many times in the cluster-to-participant vote.
inline constexpr int kPinWeight = 100;
// Largest gap, in seconds, over which a segment without an embedding inherits a label.
inline constexpr double kInheritWindowS = 10.0;
// Most recent unpinned embeddings one re-cluster considers.
inline constexpr std::size_t kReclusterWindow = 400;
// Hard cap on the working set (pins, recent embeddings), so that many pins cannot bring the
// O(N^3) cost back.
inline constexpr std::size_t kMaxWorkingSet = 600;
// An anchor's vote is capped below a pin's, so a long history never outranks a human.
inline constexpr int kAnchorMaxWeight = kPinWeight - 1;
// Relabelable members an over-merged group needs before it earns a participant of its own, so
// re-clustering noise does not invent speakers.
inline constexpr std::size_t kMinSplitSegments = 3;

// A transcript segment as the diarizer sees it (meeting.interfaces.TranscriptSegment's fields
// that it reads or writes). Times are meeting-clock seconds.
struct Segment {
    std::string id;
    double start_s = 0.0;
    double end_s = 0.0;
    // The float32 embedding's bytes (native byte order), set by assign for the chunk commit to
    // store; empty when the segment has none.
    std::vector<std::byte> embedding;
    // Set by assign_from_frames when the diarizer gives a label.
    std::optional<std::string> speaker_participant_id;
    std::string speaker_source = "channel"; // channel | diarizer | human
};

// (segment id, participant id): one reassign_segment_speaker op.
using RelabelOps = std::vector<std::pair<std::string, std::string>>;

// What the diarizer needs from the meeting (Python hands it the store and the repository).
// Implemented over the meeting's state store; the diarizer never calls it with its lock held.
// Affinity: called on the thread that runs assign.
class DiarizerHost {
public:
    virtual ~DiarizerHost() = default;
    // Creates a provisional participant named `display_name` ("Speaker N", kind others_cluster)
    // as the actor system/diarizer, and returns its id (empty when the store named none), or the
    // store's rejection reason as Python's f-string prints it ("no result" when the store answered
    // nothing, "None" when it gave no reason). May throw; either failure is the diarizer's to
    // handle.
    virtual std::expected<std::string, std::string>
    create_participant(std::string display_name) = 0;
    // Applies reassign_segment_speaker ops (as the actor system), in order.
    virtual void relabel(const RelabelOps& segment_to_participant) = 0;
    // The meeting's participants, id to display name, for speaker passes that name clusters.
    virtual std::map<std::string, std::string> participant_names() = 0;
};

// meeting.interfaces.Diarizer: progressive speaker separation for the loopback channel.
class Diarizer {
public:
    virtual ~Diarizer() = default;
    // A participant id for the segment, or nullopt to keep the channel-level label. `audio` is
    // the segment's mono audio at `rate` Hz.
    virtual std::optional<std::string> assign(Segment& segment, std::span<const float> audio,
                                              int rate) = 0;
    // A human speaker correction, as clustering supervision.
    virtual void pin(const std::string& segment_id, const std::string& participant_id) = 0;
    // The label currently recorded for a segment (after later re-clusters); nullopt when the
    // segment is unknown. A diarizer that keeps no labels returns nullopt.
    virtual std::optional<std::string> current_label(const std::string& /*segment_id*/) const {
        return std::nullopt;
    }
    // False once the embedding model is missing or the diarizer degraded.
    virtual bool is_available() const = 0;
};

class OnlineDiarizerProbe;
namespace detail {
struct DiarizerState;
} // namespace detail

// Incremental cosine assignment plus periodic re-clustering.
// Affinity: thread-safe. assign runs on the meeting's ASR thread (the periodic re-cluster runs
// inline there), pin arrives from the dashboard's threads. The internal lock is never held while
// the host is called; ops computed under it are re-checked against the pins just before they go
// to the host, so a pin landing in between wins.
class OnlineDiarizer final : public Diarizer {
public:
    OnlineDiarizer(std::unique_ptr<SpeakerEmbedder> embedder, DiarizerHost& host,
                   std::string meeting_id = {}, const Clock& clock = system_clock());
    ~OnlineDiarizer() override;
    OnlineDiarizer(const OnlineDiarizer&) = delete;
    OnlineDiarizer& operator=(const OnlineDiarizer&) = delete;

    // Embeds the audio and matches it against the cluster centroids, seeding a new "Speaker N"
    // participant when nothing matches; a segment too short to embed inherits the label of the
    // nearest labelled segment within kInheritWindowS. Audio at another rate is resampled to
    // 16 kHz first. Sets segment.embedding, and runs the periodic re-cluster when it is due: the
    // re-cluster's ops for other segments go to the host, while this segment's own new label is
    // the return value (its row does not exist until the caller commits it). Never throws: a
    // failure degrades the diarizer and returns nullopt.
    std::optional<std::string> assign(Segment& segment, std::span<const float> audio,
                                      int rate) override;
    // Marks the segment pinned (never relabelled again) and moves its embedding's weight to the
    // participant's cluster, creating one for it if needed. A segment never embedded is tracked
    // as a pin without times, so it never lends its label. Never throws.
    void pin(const std::string& segment_id, const std::string& participant_id) override;
    std::optional<std::string> current_label(const std::string& segment_id) const override;
    // Latches false for the rest of the meeting once assign failed or the embedder reported
    // itself unusable (or could not say).
    bool is_available() const override;

private:
    friend class OnlineDiarizerProbe;

    std::optional<std::string> assign_inner(Segment& segment, std::span<const float> audio,
                                            int rate);
    void degrade(const char* message, const std::exception* error, bool failed) const;
    std::string create_participant();
    RelabelOps recluster();
    RelabelOps filter_stale_ops(const RelabelOps& ops) const;

    std::unique_ptr<SpeakerEmbedder> embedder_;
    DiarizerHost& host_;
    std::string meeting_id_;
    const Clock& clock_;
    std::unique_ptr<detail::DiarizerState> state_;
    mutable std::atomic<bool> degraded_{false};
    // Separate from the state lock: degradation is announced from paths that may or may not
    // hold it, and is logged exactly once.
    mutable std::mutex degrade_mutex_;
};

// An OnlineDiarizer over the speaker model at `model` run by the ONNX Runtime library at
// `onnxruntime_dll`, or null when diarization is unavailable (no model configured, the model
// file missing, the runtime or the model failing to load): the meeting then keeps channel-level
// labels. Each reason is logged.
std::unique_ptr<OnlineDiarizer> create_diarizer(const std::filesystem::path& model,
                                                const std::filesystem::path& onnxruntime_dll,
                                                DiarizerHost& host, std::string meeting_id = {});

} // namespace fv::diarize
