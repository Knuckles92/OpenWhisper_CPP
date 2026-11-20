#include "diarize/testing/diarizer_probe.h"

#include <memory>
#include <mutex>

#include "diarize/detail/clustering_algorithm.h"
#include "diarize/detail/clustering_state.h"

namespace fv::diarize {

namespace {

using Lock = std::lock_guard<detail::OwnedMutex>;

OnlineDiarizerProbe::Record view(const detail::SegRecord& record) {
    return {record.segment_id, record.embedding, record.participant_id,
            record.pinned,     record.start_s,   record.end_s};
}

OnlineDiarizerProbe::ClusterView view(const detail::Cluster& cluster) {
    return {cluster.participant_id, cluster.centroid, cluster.count, cluster.pinned_segments};
}

} // namespace

void OnlineDiarizerProbe::set_records(const std::vector<Record>& records) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.records.clear();
    s.by_segment_id.clear();
    for (const Record& record : records) {
        auto stored = std::make_shared<detail::SegRecord>(
            detail::SegRecord{record.segment_id, record.embedding, record.participant_id,
                              record.pinned, record.start_s, record.end_s});
        s.records.push_back(stored);
        s.by_segment_id[record.segment_id] = stored;
    }
}

std::vector<OnlineDiarizerProbe::Record> OnlineDiarizerProbe::records() const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    std::vector<Record> out;
    for (const auto& record : s.records) {
        out.push_back(view(*record));
    }
    return out;
}

std::optional<OnlineDiarizerProbe::Record>
OnlineDiarizerProbe::record(const std::string& segment_id) const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    const auto found = s.by_segment_id.find(segment_id);
    if (found == s.by_segment_id.end()) {
        return std::nullopt;
    }
    return view(*found->second);
}

void OnlineDiarizerProbe::set_pinned(const std::string& segment_id, bool pinned) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.by_segment_id.at(segment_id)->pinned = pinned;
}

void OnlineDiarizerProbe::set_participant(const std::string& segment_id,
                                          std::optional<std::string> participant) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.by_segment_id.at(segment_id)->participant_id = std::move(participant);
}

void OnlineDiarizerProbe::set_clusters(const std::vector<ClusterView>& clusters) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.clusters.clear();
    for (const ClusterView& cluster : clusters) {
        s.clusters.push_back(std::make_shared<detail::Cluster>(detail::Cluster{
            cluster.participant_id, cluster.centroid, cluster.count, cluster.pinned_segments}));
    }
}

std::vector<OnlineDiarizerProbe::ClusterView> OnlineDiarizerProbe::clusters() const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    std::vector<ClusterView> out;
    for (const auto& cluster : s.clusters) {
        out.push_back(view(*cluster));
    }
    return out;
}

std::optional<OnlineDiarizerProbe::ClusterView>
OnlineDiarizerProbe::find_cluster(const std::string& participant_id) const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    if (const auto cluster = detail::Algorithm{s}.find_cluster(participant_id)) {
        return view(*cluster);
    }
    return std::nullopt;
}

void OnlineDiarizerProbe::set_new_since_recluster(int count) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.new_since_recluster = count;
}

int OnlineDiarizerProbe::new_since_recluster() const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    return s.new_since_recluster;
}

RelabelOps OnlineDiarizerProbe::recluster() {
    return diarizer_.recluster();
}

RelabelOps OnlineDiarizerProbe::filter_stale_ops(const RelabelOps& ops) const {
    return diarizer_.filter_stale_ops(ops);
}

std::pair<std::vector<OnlineDiarizerProbe::Record>, std::vector<OnlineDiarizerProbe::AnchorView>>
OnlineDiarizerProbe::working_set() const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    std::vector<std::shared_ptr<detail::SegRecord>> embedded;
    for (const auto& record : s.records) {
        if (record->embedding) {
            embedded.push_back(record);
        }
    }
    const auto [records, anchors] = detail::Algorithm{s}.working_set(embedded);
    std::pair<std::vector<Record>, std::vector<AnchorView>> out;
    for (const auto& record : records) {
        out.first.push_back(view(*record));
    }
    for (const auto& anchor : anchors) {
        out.second.push_back({anchor.participant_id, anchor.centroid, anchor.weight});
    }
    return out;
}

std::optional<std::string> OnlineDiarizerProbe::nearest_temporal_pid(const Segment& segment) const {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    return detail::Algorithm{s}.nearest_temporal_pid(segment);
}

void OnlineDiarizerProbe::after_recluster(std::function<void(const RelabelOps&)> hook) {
    detail::DiarizerState& s = *diarizer_.state_;
    Lock lock(s.lock);
    s.after_recluster = std::move(hook);
}

bool OnlineDiarizerProbe::lock_held_by_this_thread() const {
    return diarizer_.state_->lock.held_by_this_thread();
}

} // namespace fv::diarize
