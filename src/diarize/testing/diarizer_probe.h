#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "diarize/clustering.h"

namespace fv::diarize {

// Test access to an OnlineDiarizer's internals, the ones OpenWhisper's tests reach through the
// underscore attributes: records, clusters, the counters and the re-cluster's steps.
// Affinity: the test's thread; each call takes the diarizer's lock as Python's tests do.
class OnlineDiarizerProbe {
public:
    struct Record {
        std::string segment_id;
        std::optional<std::vector<float>> embedding;
        std::optional<std::string> participant_id;
        bool pinned = false;
        double start_s = 0.0;
        double end_s = 0.0;
    };
    struct ClusterView {
        std::string participant_id;
        std::vector<float> centroid;
        int count = 0;
        std::set<std::string> pinned_segments;
    };
    struct AnchorView {
        std::string participant_id;
        std::vector<float> centroid;
        double weight = 0.0;
    };

    explicit OnlineDiarizerProbe(OnlineDiarizer& diarizer) : diarizer_(diarizer) {}

    // Replaces the records (and the id index), as the tests' _prefill does.
    void set_records(const std::vector<Record>& records);
    std::vector<Record> records() const;
    std::optional<Record> record(const std::string& segment_id) const;
    void set_pinned(const std::string& segment_id, bool pinned);
    void set_participant(const std::string& segment_id, std::optional<std::string> participant);

    void set_clusters(const std::vector<ClusterView>& clusters);
    std::vector<ClusterView> clusters() const;
    std::optional<ClusterView> find_cluster(const std::string& participant_id) const;

    void set_new_since_recluster(int count);
    int new_since_recluster() const;

    // The re-cluster, the stale-op filter, the working set over every embedded record, and the
    // temporal inheritance lookup.
    RelabelOps recluster();
    RelabelOps filter_stale_ops(const RelabelOps& ops) const;
    std::pair<std::vector<Record>, std::vector<AnchorView>> working_set() const;
    std::optional<std::string> nearest_temporal_pid(const Segment& segment) const;

    // Runs between the re-cluster assign triggers and the stale-op filter.
    void after_recluster(std::function<void(const RelabelOps&)> hook);
    // Whether the calling thread holds the diarizer's lock.
    bool lock_held_by_this_thread() const;

private:
    OnlineDiarizer& diarizer_;
};

} // namespace fv::diarize
