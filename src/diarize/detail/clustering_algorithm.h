#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "diarize/clustering.h"
#include "diarize/detail/clustering_state.h"

// The diarizer's steps on its state, shared by OnlineDiarizer and its test probe.
namespace fv::diarize::detail {

inline constexpr log::Logger clustering_logger("meeting.diarize.clustering");

// float32 dot product (accumulated in double, rounded once).
inline float dot(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        throw std::runtime_error(
            std::format("Embedding sizes differ ({} and {})", a.size(), b.size()));
    }
    // Raw pointers: this is the re-cluster's inner loop, and a checked operator[] would make a
    // Debug build's pass many times slower.
    const float* left = a.data();
    const float* right = b.data();
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += static_cast<double>(left[i]) * static_cast<double>(right[i]);
    }
    return static_cast<float>(sum);
}

// float(np.linalg.norm(v)) of a float32 vector.
inline double norm(const std::vector<float>& v) {
    return static_cast<double>(std::sqrt(dot(v, v)));
}

// np.stack(vectors).mean(axis=0) in float32: rows added one after another, then divided by the
// count.
inline std::vector<float> mean_of(const std::vector<const std::vector<float>*>& vectors) {
    std::vector<float> mean(vectors.front()->size(), 0.0f);
    for (const std::vector<float>* vector : vectors) {
        if (vector->size() != mean.size()) {
            throw std::runtime_error("Embedding sizes differ");
        }
        const float* values = vector->data();
        float* sums = mean.data();
        for (std::size_t i = 0; i < mean.size(); ++i) {
            sums[i] += values[i];
        }
    }
    const auto count = static_cast<float>(vectors.size());
    for (float& value : mean) {
        value /= count;
    }
    return mean;
}

// centroid / norm in float32, the norm being Python's float of the float32 norm.
inline void divide(std::vector<float>& vector, double by) {
    const auto divisor = static_cast<float>(by);
    for (float& value : vector) {
        value /= divisor;
    }
}

// The rest of the algorithm, on the state with its lock held unless said otherwise.
struct Algorithm {
    detail::DiarizerState& s;

    std::shared_ptr<Cluster> find_cluster(const std::optional<std::string>& pid) const {
        if (!pid) {
            return nullptr;
        }
        for (const auto& cluster : s.clusters) {
            if (cluster->participant_id == *pid) {
                return cluster;
            }
        }
        return nullptr;
    }

    // Records a segment observation. A pin recorded before the segment was embedded wins: the
    // record keeps its participant and pinned flag, and only gains the embedding and the real
    // times. Returns the participant actually recorded.
    std::optional<std::string> track(const Segment& segment,
                                     std::optional<std::vector<float>> embedding,
                                     std::optional<std::string> participant_id) {
        if (const auto found = s.by_segment_id.find(segment.id);
            found != s.by_segment_id.end() && found->second->pinned) {
            SegRecord& existing = *found->second;
            if (embedding) {
                existing.embedding = std::move(embedding);
                s.new_since_recluster += 1;
            }
            existing.start_s = segment.start_s;
            existing.end_s = segment.end_s;
            return existing.participant_id;
        }
        const bool embedded = embedding.has_value();
        auto record =
            std::make_shared<SegRecord>(SegRecord{segment.id, std::move(embedding), participant_id,
                                                  false, segment.start_s, segment.end_s});
        s.records.push_back(record);
        s.by_segment_id[segment.id] = record;
        if (embedded) {
            s.new_since_recluster += 1;
        }
        return participant_id;
    }

    // The best cluster and its similarity, or nothing to seed a new one. Once kMaxClusters
    // exist the best one is returned even below the threshold (the re-cluster corrects forced
    // assignments).
    std::optional<std::pair<std::shared_ptr<Cluster>, double>>
    match_cluster(const std::vector<float>& embedding) const {
        if (s.clusters.empty()) {
            return std::nullopt;
        }
        std::size_t best = 0;
        double best_similarity = 0.0;
        for (std::size_t i = 0; i < s.clusters.size(); ++i) {
            const auto similarity = static_cast<double>(dot(s.clusters[i]->centroid, embedding));
            // np.argmax: the first maximum.
            if (i == 0 || similarity > best_similarity) {
                best = i;
                best_similarity = similarity;
            }
        }
        if (best_similarity >= kAssignThreshold || s.clusters.size() >= kMaxClusters) {
            return std::make_pair(s.clusters[best], best_similarity);
        }
        return std::nullopt;
    }

    // Exponential moving average of the centroid, renormalised.
    static void ema_update(Cluster& cluster, const std::vector<float>& embedding) {
        std::vector<float> centroid(cluster.centroid.size());
        const auto keep = static_cast<float>(1.0 - kEmaAlpha);
        const auto take = static_cast<float>(kEmaAlpha);
        for (std::size_t i = 0; i < centroid.size(); ++i) {
            centroid[i] = keep * cluster.centroid[i] + take * embedding.at(i);
        }
        const double length = norm(centroid);
        if (length > 1e-8) {
            divide(centroid, length);
            cluster.centroid = std::move(centroid);
        }
    }

    // The label of the temporally nearest labelled segment within kInheritWindowS; a later
    // record wins a tie.
    std::optional<std::string> nearest_temporal_pid(const Segment& segment) const {
        std::optional<std::string> best_pid;
        double best_gap = kInheritWindowS;
        for (const auto& record : s.records) {
            if (!record->participant_id) {
                continue;
            }
            const double gap =
                std::max({0.0, record->start_s - segment.end_s, segment.start_s - record->end_s});
            if (gap <= best_gap) {
                best_gap = gap;
                best_pid = record->participant_id;
            }
        }
        return best_pid;
    }

    bool should_recluster(const Clock& clock) const {
        if (s.new_since_recluster <= 0) {
            return false;
        }
        if (s.new_since_recluster >= kReclusterEveryN) {
            return true;
        }
        return clock.steady() - s.last_recluster >= kReclusterEvery;
    }

    // Every pinned embedding (pins always anchor identity), then the most recent unpinned ones
    // up to the remaining budget; everything older becomes one unit-norm anchor per participant.
    std::pair<std::vector<std::shared_ptr<SegRecord>>, std::vector<Anchor>>
    working_set(const std::vector<std::shared_ptr<SegRecord>>& embedded) const {
        std::vector<std::shared_ptr<SegRecord>> selected;
        std::vector<std::shared_ptr<SegRecord>> unpinned;
        for (const auto& record : embedded) {
            (record->pinned ? selected : unpinned).push_back(record);
        }
        const std::size_t budget =
            selected.size() >= kMaxWorkingSet
                ? 0
                : std::min(kReclusterWindow, kMaxWorkingSet - selected.size());
        const std::size_t recent = std::min(budget, unpinned.size());
        selected.insert(selected.end(), unpinned.end() - static_cast<std::ptrdiff_t>(recent),
                        unpinned.end());

        // By segment id, as Python's selected_ids: a segment assigned twice leaves its older
        // record in the list, and that record stays out of the anchors while the newer one is in
        // the working set.
        std::set<std::string> selected_ids;
        for (const auto& record : selected) {
            selected_ids.insert(record->segment_id);
        }
        // Participants in order of first appearance, as Python's dict.
        std::vector<std::pair<std::string, std::vector<const std::vector<float>*>>> history;
        for (const auto& record : embedded) {
            if (selected_ids.contains(record->segment_id) || !record->participant_id) {
                continue;
            }
            auto entry = std::find_if(history.begin(), history.end(), [&](const auto& item) {
                return item.first == *record->participant_id;
            });
            if (entry == history.end()) {
                history.emplace_back(*record->participant_id,
                                     std::vector<const std::vector<float>*>{});
                entry = history.end() - 1;
            }
            entry->second.push_back(&*record->embedding);
        }

        std::vector<Anchor> anchors;
        for (const auto& [pid, vectors] : history) {
            std::vector<float> centroid = mean_of(vectors);
            const double length = norm(centroid);
            if (length <= 1e-8) {
                continue;
            }
            divide(centroid, length);
            anchors.push_back(Anchor{
                pid, std::move(centroid),
                static_cast<double>(std::min(vectors.size(), std::size_t{kAnchorMaxWeight}))});
        }
        return {std::move(selected), std::move(anchors)};
    }

    // Maps each group to the weighted-majority participant of its members (pins weigh
    // kPinWeight, anchors their capped history). When two groups claim one participant the
    // online pass merged two speakers: the weaker groups are flagged for new participants.
    std::vector<PlannedGroup> resolve_groups(const std::vector<std::vector<std::size_t>>& groups,
                                             const std::vector<std::shared_ptr<SegRecord>>& records,
                                             const std::vector<Anchor>& anchors,
                                             const std::string& meeting_id) const {
        std::vector<PlannedGroup> planned;
        planned.reserve(groups.size());
        // Participant -> (weight, index into planned), in order of first claim.
        std::vector<std::pair<std::string, std::vector<std::pair<double, std::size_t>>>> claims;

        for (const auto& group : groups) {
            std::vector<std::pair<std::string, double>> weights;
            std::vector<std::string> members;
            for (const std::size_t index : group) {
                std::optional<std::string> pid;
                double weight = 0.0;
                if (index < records.size()) {
                    const SegRecord& record = *records[index];
                    if (!record.pinned) {
                        members.push_back(record.segment_id);
                    }
                    pid = record.participant_id;
                    weight = record.pinned ? static_cast<double>(kPinWeight) : 1.0;
                } else {
                    const Anchor& anchor = anchors[index - records.size()];
                    pid = anchor.participant_id;
                    weight = anchor.weight;
                }
                if (!pid) {
                    continue;
                }
                auto entry = std::find_if(weights.begin(), weights.end(),
                                          [&](const auto& item) { return item.first == *pid; });
                if (entry == weights.end()) {
                    weights.emplace_back(*pid, weight);
                } else {
                    entry->second += weight;
                }
            }
            if (weights.empty()) {
                continue;
            }
            // max((weight, pid)): the heaviest, ties to the greater id.
            const auto dominant = std::max_element(
                weights.begin(), weights.end(), [](const auto& left, const auto& right) {
                    return std::tie(left.second, left.first) < std::tie(right.second, right.first);
                });
            planned.push_back(PlannedGroup{dominant->first, std::move(members), false});
            auto claim = std::find_if(claims.begin(), claims.end(), [&](const auto& item) {
                return item.first == dominant->first;
            });
            if (claim == claims.end()) {
                claims.emplace_back(dominant->first, std::vector<std::pair<double, std::size_t>>{});
                claim = claims.end() - 1;
            }
            claim->second.emplace_back(dominant->second, planned.size() - 1);
        }

        for (auto& [pid, contenders] : claims) {
            if (contenders.size() < 2) {
                continue;
            }
            // The strongest claim keeps the participant: by weight, then size, then first id.
            std::stable_sort(
                contenders.begin(), contenders.end(), [&](const auto& left, const auto& right) {
                    const PlannedGroup& a = planned[left.second];
                    const PlannedGroup& b = planned[right.second];
                    const std::string first_a = a.segment_ids.empty() ? "" : a.segment_ids[0];
                    const std::string first_b = b.segment_ids.empty() ? "" : b.segment_ids[0];
                    return std::make_tuple(-left.first,
                                           -static_cast<long long>(a.segment_ids.size()), first_a) <
                           std::make_tuple(-right.first,
                                           -static_cast<long long>(b.segment_ids.size()), first_b);
                });
            for (std::size_t i = 1; i < contenders.size(); ++i) {
                PlannedGroup& entry = planned[contenders[i].second];
                if (entry.segment_ids.size() >= kMinSplitSegments) {
                    entry.needs_new_participant = true;
                    entry.participant_id.reset();
                    clustering_logger.info(
                        "Re-cluster splitting {} segments off participant {} (meeting {})",
                        entry.segment_ids.size(), pid, meeting_id);
                }
            }
        }
        return planned;
    }

    // Centroids rebuilt from the assignments after a re-cluster.
    void rebuild_clusters() {
        std::vector<std::pair<std::string, std::vector<std::shared_ptr<SegRecord>>>> by_pid;
        for (const auto& record : s.records) {
            if (!record->embedding || !record->participant_id) {
                continue;
            }
            auto entry = std::find_if(by_pid.begin(), by_pid.end(), [&](const auto& item) {
                return item.first == *record->participant_id;
            });
            if (entry == by_pid.end()) {
                by_pid.emplace_back(*record->participant_id,
                                    std::vector<std::shared_ptr<SegRecord>>{});
                entry = by_pid.end() - 1;
            }
            entry->second.push_back(record);
        }
        std::vector<std::shared_ptr<Cluster>> clusters;
        for (const auto& [pid, members] : by_pid) {
            std::vector<const std::vector<float>*> vectors;
            std::set<std::string> pinned;
            for (const auto& record : members) {
                vectors.push_back(&*record->embedding);
                if (record->pinned) {
                    pinned.insert(record->segment_id);
                }
            }
            std::vector<float> centroid = mean_of(vectors);
            const double length = norm(centroid);
            if (length > 1e-8) {
                divide(centroid, length);
            }
            clusters.push_back(std::make_shared<Cluster>(Cluster{
                pid, std::move(centroid), static_cast<int>(members.size()), std::move(pinned)}));
        }
        s.clusters = std::move(clusters);
    }
};

} // namespace fv::diarize::detail