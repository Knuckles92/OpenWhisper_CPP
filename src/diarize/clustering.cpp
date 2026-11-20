#include "diarize/clustering.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <tuple>

#include "base/log.h"
#include "base/utf8.h"
#include "diarize/detail/average_linkage.h"
#include "diarize/detail/clustering_algorithm.h"
#include "diarize/detail/clustering_state.h"
#include "dsp/fbank.h"
#include "dsp/fft_resample.h"

namespace fv::diarize {

namespace {

constexpr const log::Logger& logger = detail::clustering_logger;

using detail::Algorithm;
using detail::Anchor;
using detail::Cluster;
using detail::dot;
using detail::PlannedGroup;
using detail::SegRecord;

using Lock = std::lock_guard<detail::OwnedMutex>;

// Python's round() (half to even) of a non-negative double, as an index.
long long round_half_even(double value) {
    return static_cast<long long>(std::nearbyint(value));
}

} // namespace

OnlineDiarizer::OnlineDiarizer(std::unique_ptr<SpeakerEmbedder> embedder, DiarizerHost& host,
                               std::string meeting_id, const Clock& clock)
    : embedder_(std::move(embedder)), host_(host), meeting_id_(std::move(meeting_id)),
      clock_(clock), state_(std::make_unique<detail::DiarizerState>()) {
    state_->last_recluster = clock_.steady();
}

OnlineDiarizer::~OnlineDiarizer() = default;

std::optional<std::string> OnlineDiarizer::assign(Segment& segment, std::span<const float> audio,
                                                  int rate) {
    if (degraded_.load()) {
        return std::nullopt;
    }
    try {
        return assign_inner(segment, audio, rate);
    } catch (const std::exception& error) {
        degrade("Diarizer failed", &error, true);
    } catch (...) {
        degrade("Diarizer failed", nullptr, true);
    }
    return std::nullopt;
}

void OnlineDiarizer::pin(const std::string& segment_id, const std::string& participant_id) {
    try {
        detail::DiarizerState& s = *state_;
        Lock lock(s.lock);
        std::shared_ptr<SegRecord> record;
        if (const auto found = s.by_segment_id.find(segment_id); found != s.by_segment_id.end()) {
            record = found->second;
        } else {
            // A segment never embedded (the mic channel, before the diarizer, or too short):
            // tracked so that re-clusters and inheritance respect the pin; -inf times keep it
            // out of the inheritance window.
            constexpr double never = -std::numeric_limits<double>::infinity();
            record = std::make_shared<SegRecord>(
                SegRecord{segment_id, std::nullopt, participant_id, true, never, never});
            s.records.push_back(record);
            s.by_segment_id[segment_id] = record;
        }

        const std::optional<std::string> old_pid = record->participant_id;
        record->pinned = true;
        record->participant_id = participant_id;
        const bool moved = record->embedding.has_value() && old_pid != participant_id;
        const Algorithm algorithm{s};

        if (moved) {
            if (const auto old_cluster = algorithm.find_cluster(old_pid)) {
                old_cluster->count = std::max(0, old_cluster->count - 1);
                old_cluster->pinned_segments.erase(segment_id);
                if (old_cluster->count == 0 && old_cluster->pinned_segments.empty()) {
                    s.clusters.erase(std::find(s.clusters.begin(), s.clusters.end(), old_cluster));
                }
            }
        }

        if (const auto target = algorithm.find_cluster(participant_id)) {
            target->pinned_segments.insert(segment_id);
            if (moved) {
                target->count += 1;
                Algorithm::ema_update(*target, *record->embedding);
            }
        } else if (record->embedding) {
            // Supervision may exceed kMaxClusters: a pinned cluster is ground truth, not an
            // inference.
            s.clusters.push_back(std::make_shared<Cluster>(
                Cluster{participant_id, *record->embedding, 1, {segment_id}}));
        }
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Diarizer pin failed for segment {}", segment_id));
    }
}

bool OnlineDiarizer::is_available() const {
    if (degraded_.load()) {
        return false;
    }
    bool available = false;
    try {
        available = embedder_ != nullptr && embedder_->available();
    } catch (const std::exception& error) {
        degrade("Diarizer embedder availability probe failed", &error, true);
        return false;
    } catch (...) {
        degrade("Diarizer embedder availability probe failed", nullptr, true);
        return false;
    }
    if (!available) {
        degrade("Diarizer embedder became unavailable", nullptr, false);
    }
    return available;
}

void OnlineDiarizer::degrade(const char* message, const std::exception* error, bool failed) const {
    std::lock_guard lock(degrade_mutex_);
    if (degraded_.load()) {
        return;
    }
    degraded_.store(true);
    const std::string line =
        std::format("{} (meeting {}); degrading to channel-level labels", message, meeting_id_);
    if (error != nullptr) {
        logger.error_with(*error, line);
    } else if (failed) {
        logger.error("{}", line);
    } else {
        logger.warning("{}", line);
    }
}

std::optional<std::string> OnlineDiarizer::current_label(const std::string& segment_id) const {
    detail::DiarizerState& s = *state_;
    Lock lock(s.lock);
    const auto found = s.by_segment_id.find(segment_id);
    if (found == s.by_segment_id.end()) {
        return std::nullopt;
    }
    return found->second->participant_id;
}

std::optional<std::string> OnlineDiarizer::assign_inner(Segment& segment,
                                                        std::span<const float> audio, int rate) {
    detail::DiarizerState& s = *state_;
    Algorithm algorithm{s};

    std::vector<float> resampled;
    std::span<const float> mono = audio;
    if (rate != dsp::kFbankSampleRate && !audio.empty()) {
        if (rate == 0) {
            // Python divides by the rate and degrades on the ZeroDivisionError; the cast of an
            // infinite count below would be undefined.
            throw std::invalid_argument("The segment's audio has a sample rate of 0 Hz.");
        }
        const auto count = std::max<long long>(
            1, round_half_even(static_cast<double>(audio.size()) * dsp::kFbankSampleRate /
                               static_cast<double>(rate)));
        resampled = dsp::fft_resample(audio, static_cast<std::size_t>(count));
        mono = resampled;
    }
    std::optional<std::vector<float>> embedding = embedder_->embed(mono);

    if (!embedding) {
        Lock lock(s.lock);
        std::optional<std::string> pid = algorithm.nearest_temporal_pid(segment);
        return algorithm.track(segment, std::nullopt, std::move(pid));
    }

    bool honor_pin = false;
    std::optional<std::pair<std::shared_ptr<Cluster>, double>> match;
    {
        Lock lock(s.lock);
        // A human may have pinned this segment already (a correction racing this thread): the
        // pin is authoritative, the embedding only joins as supervision.
        const auto found = s.by_segment_id.find(segment.id);
        honor_pin = found != s.by_segment_id.end() && found->second->pinned;
        if (!honor_pin) {
            match = algorithm.match_cluster(*embedding);
        }
    }

    std::optional<std::string> pid;
    bool do_recluster = false;
    if (honor_pin) {
        Lock lock(s.lock);
        pid = algorithm.track(segment, embedding, std::nullopt);
        do_recluster = algorithm.should_recluster(clock_);
    } else if (!match) {
        std::string created = create_participant(); // the host is called outside the lock
        Lock lock(s.lock);
        s.clusters.push_back(std::make_shared<Cluster>(Cluster{created, *embedding, 1, {}}));
        pid = algorithm.track(segment, embedding, std::move(created));
        do_recluster = algorithm.should_recluster(clock_);
    } else {
        const auto& [cluster, similarity] = *match;
        Lock lock(s.lock);
        if (similarity >= kAssignThreshold && !cluster->pinned_dominated()) {
            Algorithm::ema_update(*cluster, *embedding);
        }
        cluster->count += 1;
        pid = algorithm.track(segment, embedding, cluster->participant_id);
        do_recluster = algorithm.should_recluster(clock_);
    }

    // The chunk commit stores this with the segment row; writing it anywhere now would race a
    // row that does not exist yet.
    segment.embedding.resize(embedding->size() * sizeof(float));
    std::memcpy(segment.embedding.data(), embedding->data(), segment.embedding.size());

    if (do_recluster) {
        RelabelOps ops = recluster();
        if (s.after_recluster) {
            s.after_recluster(ops);
        }
        ops = filter_stale_ops(ops);
        // The caller stores this segment with the returned label, so its own relabel travels as
        // the return value, not as an op on a row that does not exist yet.
        std::erase_if(ops, [&](const auto& op) { return op.first == segment.id; });
        // Python's `if current:`: an empty id keeps the label assign arrived at.
        if (std::optional<std::string> current = current_label(segment.id);
            current && !current->empty()) {
            pid = std::move(current);
        }
        if (!ops.empty()) {
            assert(!s.lock.held_by_this_thread());
            host_.relabel(ops);
        }
    }
    return pid;
}

std::string OnlineDiarizer::create_participant() {
    detail::DiarizerState& s = *state_;
    int number = 0;
    {
        Lock lock(s.lock);
        number = ++s.speaker_counter;
    }
    assert(!s.lock.held_by_this_thread());
    std::expected<std::string, std::string> pid =
        host_.create_participant(std::format("Speaker {}", number));
    if (!pid) {
        throw std::runtime_error("Participant creation rejected: " + pid.error());
    }
    if (pid->empty()) {
        throw std::runtime_error("Participant creation returned no id");
    }
    return *pid;
}

RelabelOps OnlineDiarizer::filter_stale_ops(const RelabelOps& ops) const {
    if (ops.empty()) {
        return {};
    }
    detail::DiarizerState& s = *state_;
    Lock lock(s.lock);
    RelabelOps fresh;
    for (const auto& op : ops) {
        const auto found = s.by_segment_id.find(op.first);
        if (found == s.by_segment_id.end() || found->second->pinned) {
            continue;
        }
        if (found->second->participant_id != op.second) {
            continue;
        }
        fresh.push_back(op);
    }
    return fresh;
}

// Three phases, so that creating participants (through the host) never happens under the lock:
// plan under the lock (working set, linkage, groups resolved to participants, losers flagged
// for a split), create the split participants without it, then apply under it, checking the
// pins again (one may have landed meanwhile) and rebuilding the centroids.
RelabelOps OnlineDiarizer::recluster() {
    detail::DiarizerState& s = *state_;
    Algorithm algorithm{s};

    std::vector<PlannedGroup> planned;
    {
        Lock lock(s.lock);
        s.new_since_recluster = 0;
        s.last_recluster = clock_.steady();

        std::vector<std::shared_ptr<SegRecord>> embedded;
        for (const auto& record : s.records) {
            if (record->embedding) {
                embedded.push_back(record);
            }
        }
        if (embedded.size() < 2) {
            return {};
        }
        auto [records, anchors] = algorithm.working_set(embedded);
        const std::size_t n = records.size() + anchors.size();
        if (n < 2) {
            return {};
        }
        std::vector<const std::vector<float>*> vectors;
        vectors.reserve(n);
        for (const auto& record : records) {
            vectors.push_back(&*record->embedding);
        }
        for (const auto& anchor : anchors) {
            vectors.push_back(&anchor.centroid);
        }
        // np.clip(1 - M @ M.T, 0, 2) in float32, then float64 for the linkage.
        std::vector<double> dist(n * n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i; j < n; ++j) {
                const float distance = std::clamp(1.0f - dot(*vectors[i], *vectors[j]), 0.0f, 2.0f);
                dist[i * n + j] = distance;
                dist[j * n + i] = distance;
            }
        }
        const auto groups = detail::average_linkage(dist, n, kMergeDistance);
        // Python resolves the groups after releasing the lock, reading records that only this
        // thread relabels; here it stays held, so a pin cannot change a vote mid-way.
        planned = algorithm.resolve_groups(groups, records, anchors, meeting_id_);
    }
    if (planned.empty()) {
        return {};
    }

    const bool splits = std::any_of(planned.begin(), planned.end(),
                                    [](const PlannedGroup& p) { return p.needs_new_participant; });
    if (splits) {
        std::vector<PlannedGroup> resolved;
        for (PlannedGroup& entry : planned) {
            if (!entry.needs_new_participant) {
                resolved.push_back(std::move(entry));
                continue;
            }
            try {
                entry.participant_id = create_participant();
            } catch (const std::exception& error) {
                // Not fatal: the group keeps its labels for this pass.
                logger.error_with(error,
                                  std::format("Re-cluster could not create a split participant "
                                              "(meeting {}); leaving the group merged",
                                              meeting_id_));
                continue;
            } catch (...) {
                logger.error("Re-cluster could not create a split participant (meeting {}); "
                             "leaving the group merged",
                             meeting_id_);
                continue;
            }
            resolved.push_back(std::move(entry));
        }
        planned = std::move(resolved);
    }

    RelabelOps ops;
    Lock lock(s.lock);
    for (const PlannedGroup& entry : planned) {
        if (!entry.participant_id) {
            continue;
        }
        const std::string& pid = *entry.participant_id;
        for (const std::string& segment_id : entry.segment_ids) {
            const auto found = s.by_segment_id.find(segment_id);
            if (found == s.by_segment_id.end() || found->second->pinned) {
                continue; // human corrections are never relabelled
            }
            if (found->second->participant_id == pid) {
                continue;
            }
            found->second->participant_id = pid;
            ops.emplace_back(segment_id, pid);
        }
    }
    algorithm.rebuild_clusters();
    return ops;
}

std::unique_ptr<OnlineDiarizer> create_diarizer(const std::filesystem::path& model,
                                                const std::filesystem::path& onnxruntime_dll,
                                                DiarizerHost& host, std::string meeting_id) {
    if (model.empty()) {
        logger.info("Diarization disabled: no speaker embedding model configured");
        return nullptr;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(model, error)) {
        logger.warning("Diarization disabled: model not found at {}", path_to_utf8(model));
        return nullptr;
    }
    std::unique_ptr<OrtEmbeddingModel> loaded;
    try {
        loaded = OrtEmbeddingModel::load(onnxruntime_dll, model);
    } catch (const std::exception& failure) {
        logger.warning("Diarization disabled: embedder failed to load: {}", failure.what());
        return nullptr;
    }
    return std::make_unique<OnlineDiarizer>(std::make_unique<SpeakerEmbedder>(std::move(loaded)),
                                            host, std::move(meeting_id));
}

} // namespace fv::diarize
