#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "diarize/clustering.h"

namespace fv::diarize::detail {

// A mutex that knows which thread holds it, so that calls to the host can assert they are not
// made under it.
class OwnedMutex {
public:
    void lock() {
        mutex_.lock();
        owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }
    void unlock() {
        owner_.store(std::thread::id(), std::memory_order_relaxed);
        mutex_.unlock();
    }
    bool held_by_this_thread() const {
        return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

private:
    std::mutex mutex_;
    std::atomic<std::thread::id> owner_{};
};

// One observed segment: its embedding (if any) and current label.
struct SegRecord {
    std::string segment_id;
    std::optional<std::vector<float>> embedding;
    std::optional<std::string> participant_id;
    bool pinned = false;
    double start_s = 0.0;
    double end_s = 0.0;
};

// One speaker cluster.
struct Cluster {
    std::string participant_id;
    std::vector<float> centroid;
    int count = 0;
    std::set<std::string> pinned_segments;

    // Pinned supervision outweighs organic membership: one pin dominates until the cluster has
    // more than kPinWeight members, and a dominated cluster's centroid does not drift.
    bool pinned_dominated() const {
        return static_cast<long long>(pinned_segments.size()) * kPinWeight >= count && count > 0;
    }
};

// A participant's out-of-window history summarised as one vector. Anchors join the re-cluster's
// distance matrix but are never relabelled and never emit ops.
struct Anchor {
    std::string participant_id;
    std::vector<float> centroid;
    double weight = 0.0;
};

// One average-linkage group resolved to a participant (the re-cluster's plan).
struct PlannedGroup {
    // The participant the group maps to; nullopt while a new one is still to be created.
    std::optional<std::string> participant_id;
    // Relabelable (unpinned, non-anchor) members.
    std::vector<std::string> segment_ids;
    // The group lost the contest for its dominant participant and is split off as a new
    // speaker.
    bool needs_new_participant = false;
};

// Records and clusters are shared pointers because Python's are objects: a cluster matched
// under the lock is updated after the lock was released and taken again, even if a pin has
// removed it from the list meanwhile.
struct DiarizerState {
    mutable OwnedMutex lock;
    std::vector<std::shared_ptr<Cluster>> clusters;
    std::vector<std::shared_ptr<SegRecord>> records;
    std::unordered_map<std::string, std::shared_ptr<SegRecord>> by_segment_id;
    int speaker_counter = 0;
    int new_since_recluster = 0;
    std::chrono::steady_clock::time_point last_recluster{};
    // Tests only: runs between the re-cluster and the stale-op filter, where a racing pin lands.
    std::function<void(const RelabelOps&)> after_recluster;
};

} // namespace fv::diarize::detail
