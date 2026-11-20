// Ported from tests/test_meeting_diarize.py (the clustering classes), plus the golden vectors of
// meeting.diarize.clustering.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "base/log.h"
#include "diarize/clustering.h"
#include "diarize/detail/average_linkage.h"
#include "diarize_support.h"
#include "golden/golden.h"

using fv::diarize::Diarizer;
using fv::diarize::kMaxWorkingSet;
using fv::diarize::kMinSplitSegments;
using fv::diarize::kPinWeight;
using fv::diarize::kReclusterWindow;
using fv::diarize::OnlineDiarizer;
using fv::diarize::OnlineDiarizerProbe;
using fv::diarize::RelabelOps;
using fv::diarize::detail::average_linkage;
using fv::diarize::test::Embedding;
using fv::diarize::test::optional_all;
using fv::diarize::test::Rig;
using fv::diarize::test::segment;
using fv::diarize::test::silence;
using fv::diarize::test::synthetic_speakers;
using fv::diarize::test::unit;

namespace {

// np.clip(1 - E @ E.T, 0, 2) in float32, as the diarizer builds it.
std::vector<double> distances(const std::vector<Embedding>& vectors) {
    const std::size_t n = vectors.size();
    std::vector<double> out(n * n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            double dot = 0.0;
            for (std::size_t k = 0; k < vectors[i].size(); ++k) {
                dot += static_cast<double>(vectors[i][k]) * vectors[j][k];
            }
            out[i * n + j] = std::clamp(1.0f - static_cast<float>(dot), 0.0f, 2.0f);
        }
    }
    return out;
}

std::vector<std::vector<std::size_t>> sorted_groups(std::vector<std::vector<std::size_t>> groups) {
    for (auto& group : groups) {
        std::sort(group.begin(), group.end());
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

bool targets(const RelabelOps& ops, const std::string& segment_id) {
    return std::any_of(ops.begin(), ops.end(),
                       [&](const auto& op) { return op.first == segment_id; });
}

class CaptureSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex);
        if (record.logger == "meeting.diarize.clustering" &&
            record.level >= fv::log::Level::Warning) {
            failures += 1;
        }
    }
    std::mutex mutex;
    int failures = 0;
};

// A diarizer whose records all say p_A although they are two speakers, so a re-cluster wants to
// move segments (TestPinRaceProtection._racing_diarizer).
std::unique_ptr<Rig> racing_diarizer() {
    const auto vectors = synthetic_speakers(8, 2);
    auto rig = std::make_unique<Rig>(optional_all(vectors));
    const auto records = rig->prefill(vectors, [](std::size_t) { return std::string("p_A"); });
    rig->probe->set_clusters({{"p_A", vectors[0], static_cast<int>(records.size()), {}}});
    return rig;
}

} // namespace

// from tests/test_meeting_diarize.py::TestAverageLinkage::test_separates_distant_pairs
TEST_CASE("TestAverageLinkage::test_separates_distant_pairs", "[test_meeting_diarize]") {
    const std::vector<Embedding> emb{unit({1, 0, 0}), unit({0.99, 0.1, 0}), unit({0, 1, 0}),
                                     unit({0.1, 0.99, 0})};
    const auto groups = average_linkage(distances(emb), emb.size(), 0.38);
    REQUIRE(groups.size() == 2);
    CHECK(sorted_groups(groups) == std::vector<std::vector<std::size_t>>{{0, 1}, {2, 3}});
}

// from tests/test_meeting_diarize.py::TestPinPreservation
TEST_CASE("TestPinPreservation::test_pinned_segments_never_relabeled_on_recluster",
          "[test_meeting_diarize]") {
    Rig rig;
    // Speaker A and B axes; one A-like embedding is pinned to B, so a naive re-cluster would
    // flip it. The pin must win.
    const Embedding a1 = unit({1.0, 0.0, 0.0});
    const Embedding a2 = unit({0.95, 0.05, 0.0});
    const Embedding b1 = unit({0.0, 1.0, 0.0});
    const Embedding b2 = unit({0.05, 0.95, 0.0});
    const Embedding pinned = unit({0.98, 0.02, 0.0});
    rig.probe->set_records({
        {"sg_a1", a1, "p_A", false, 0.0, 1.0},
        {"sg_a2", a2, "p_A", false, 1.0, 2.0},
        {"sg_b1", b1, "p_B", false, 2.0, 3.0},
        {"sg_b2", b2, "p_B", false, 3.0, 4.0},
        {"sg_pin", pinned, "p_B", true, 4.0, 5.0},
    });
    rig.probe->set_clusters({{"p_A", a1, 2, {}}, {"p_B", b1, 3, {"sg_pin"}}});
    rig.probe->set_new_since_recluster(25);

    const RelabelOps ops = rig.probe->recluster();
    const auto record = rig.probe->record("sg_pin");
    REQUIRE(record);
    CHECK(record->participant_id == "p_B");
    CHECK(record->pinned);
    CHECK_FALSE(targets(ops, "sg_pin"));
}

TEST_CASE("TestPinPreservation::test_pin_api_marks_segment_and_blocks_relabel",
          "[test_meeting_diarize]") {
    Rig rig(
        optional_all({unit({1, 0, 0}), unit({0.9, 0.1, 0}), unit({0, 1, 0}), unit({0.1, 0.9, 0})}));
    std::vector<std::optional<std::string>> pids;
    for (int i = 0; i < 4; ++i) {
        pids.push_back(rig.assign("sg_" + std::to_string(i), static_cast<double>(i)));
    }
    // The first segment to the second speaker's cluster.
    const std::string target = pids[2].value();
    rig.diarizer->pin("sg_0", target);
    CHECK(rig.probe->record("sg_0")->pinned);
    CHECK(rig.label("sg_0") == target);

    rig.probe->set_new_since_recluster(25);
    const RelabelOps ops = rig.probe->recluster();
    CHECK(rig.label("sg_0") == target);
    CHECK_FALSE(targets(ops, "sg_0"));
}

TEST_CASE("TestPinPreservation::test_pin_weight_constant", "[test_meeting_diarize]") {
    STATIC_CHECK(kPinWeight == 100);
}

// from tests/test_meeting_diarize.py::TestPinRaceProtection: a pin landing between the ops'
// computation and their dispatch must win.
TEST_CASE("TestPinRaceProtection::test_stale_op_for_pinned_segment_is_filtered_before_dispatch",
          "[test_meeting_diarize]") {
    auto rig = racing_diarizer();
    std::optional<std::pair<std::string, std::string>> raced;
    rig->probe->after_recluster([&](const RelabelOps& ops) {
        REQUIRE_FALSE(ops.empty()); // the fixture must produce relabel ops
        // The human correction lands here: after the diarizer released its lock, before the
        // ops reach the store.
        raced = ops[0];
        rig->diarizer->pin(ops[0].first, "p_human");
    });
    rig->probe->set_new_since_recluster(25);
    rig->assign("sg_new", 99.0);

    REQUIRE(raced);
    CHECK(raced->second != "p_human");
    // The stale op never reaches the store...
    CHECK_FALSE(targets(rig->host.relabels(), raced->first));
    // ...and the record keeps the human's choice, still pinned, so the store and the diarizer
    // cannot diverge.
    const auto record = rig->probe->record(raced->first);
    CHECK(record->participant_id == "p_human");
    CHECK(record->pinned);
}

TEST_CASE("TestPinRaceProtection::test_filter_keeps_ops_that_are_still_valid",
          "[test_meeting_diarize]") {
    auto rig = racing_diarizer();
    const RelabelOps ops = rig->probe->recluster();
    REQUIRE_FALSE(ops.empty());
    CHECK(rig->probe->filter_stale_ops(ops) == ops);
}

TEST_CASE("TestPinRaceProtection::test_filter_drops_ops_for_unknown_segments",
          "[test_meeting_diarize]") {
    auto rig = racing_diarizer();
    CHECK(rig->probe->filter_stale_ops({{"sg_missing", "p_A"}}).empty());
}

// from tests/test_meeting_diarize.py::TestReclusterOwnSegment: the segment that triggers a
// re-cluster is not stored yet.
TEST_CASE("TestReclusterOwnSegment::test_assign_returns_post_recluster_label_without_an_own_op",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(12, 2, 11);
    std::vector<Embedding> speaker_a;
    std::vector<Embedding> speaker_b;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        (i % 2 == 0 ? speaker_a : speaker_b).push_back(vectors[i]);
    }
    Rig rig({speaker_b.back()});
    std::vector<OnlineDiarizerProbe::Record> records;
    for (std::size_t i = 0; i + 1 < speaker_a.size(); ++i) {
        records.push_back({"sg_a" + std::to_string(i), speaker_a[i], "p_A", false,
                           static_cast<double>(i), static_cast<double>(i) + 1.0});
    }
    for (std::size_t i = 0; i + 1 < speaker_b.size(); ++i) {
        records.push_back({"sg_b" + std::to_string(i), speaker_b[i], "p_B", false,
                           10.0 + static_cast<double>(i), 11.0 + static_cast<double>(i)});
    }
    rig.probe->set_records(records);
    // A stale centroid makes the online pass pick the wrong speaker; the re-cluster corrects it.
    rig.probe->set_clusters({{"p_A", speaker_b[0], static_cast<int>(records.size()), {}}});
    rig.probe->set_new_since_recluster(25);

    const auto pid = rig.assign("sg_new", 50.0);

    CHECK(pid == "p_B");
    CHECK(rig.diarizer->current_label("sg_new") == "p_B");
    CHECK_FALSE(targets(rig.host.relabels(), "sg_new"));
}

// from tests/test_meeting_diarize.py::TestPinOnUnknownSegment
TEST_CASE("TestPinOnUnknownSegment::test_pin_never_embedded_segment_does_not_raise",
          "[test_meeting_diarize]") {
    Rig rig;
    rig.diarizer->pin("sg_never_seen", "p_X"); // mic channel, before the diarizer, too short
    const auto record = rig.probe->record("sg_never_seen");
    REQUIRE(record);
    CHECK(record->pinned);
    CHECK(record->participant_id == "p_X");
    CHECK_FALSE(record->embedding);
}

TEST_CASE("TestPinOnUnknownSegment::test_pin_on_unknown_segment_is_not_a_temporal_donor",
          "[test_meeting_diarize]") {
    Rig rig;
    rig.diarizer->pin("sg_never_seen", "p_X");
    // -inf placeholder times keep it out of the inheritance window.
    CHECK_FALSE(rig.probe->nearest_temporal_pid(segment("sg_q", 0.0)));
}

TEST_CASE("TestPinOnUnknownSegment::test_pin_before_embedding_survives_later_assignment",
          "[test_meeting_diarize]") {
    Rig rig(optional_all(synthetic_speakers(4, 2)));
    rig.diarizer->pin("sg_0", "p_human");
    const auto pid = rig.assign("sg_0", 0.0);
    CHECK(pid == "p_human");
    const auto record = rig.probe->record("sg_0");
    CHECK(record->pinned);
    CHECK(record->embedding);
    CHECK(rig.host.created() == 0); // no phantom "Speaker N" participant
}

TEST_CASE("TestPinOnUnknownSegment::test_pin_moves_weight_between_clusters",
          "[test_meeting_diarize]") {
    Rig rig(optional_all({unit({1, 0, 0}), unit({0, 1, 0})}));
    const auto first = rig.assign("sg_0", 0.0);
    const auto second = rig.assign("sg_1", 2.0);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first != second);
    rig.diarizer->pin("sg_0", *second);
    const auto target = rig.probe->find_cluster(*second);
    REQUIRE(target);
    CHECK(target->pinned_segments.contains("sg_0"));
    CHECK_FALSE(rig.probe->find_cluster(*first)); // the emptied cluster is dropped
}

// from tests/test_meeting_diarize.py::TestGracefulDegradation
TEST_CASE("TestGracefulDegradation::test_runtime_failure_degrades_once_and_never_blocks_asr",
          "[test_meeting_diarize]") {
    Rig rig;
    rig.model->fail_with("onnxruntime session crashed");
    auto sink = std::make_shared<CaptureSink>();
    fv::log::add_sink(sink);

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::optional<std::string>> results;
    for (int i = 0; i < 50; ++i) {
        results.push_back(rig.assign("sg_" + std::to_string(i), static_cast<double>(i)));
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    fv::log::remove_sink(sink);

    CHECK(results == std::vector<std::optional<std::string>>(50));
    CHECK_FALSE(rig.diarizer->is_available());
    // Degraded mode short-circuits: the model is called once, not 50 times, so the ASR thread
    // is never held up by a broken diarizer.
    CHECK(rig.model->calls() == 1);
    CHECK(elapsed.count() < 1.0);
    CHECK(sink->failures == 1);
}

TEST_CASE(
    "TestGracefulDegradation::test_is_available_latches_false_when_embedder_reports_unavailable",
    "[test_meeting_diarize]") {
    Rig rig({unit({1, 0, 0})});
    CHECK(rig.diarizer->is_available());
    rig.model->set_available(false);
    CHECK_FALSE(rig.diarizer->is_available());
    rig.model->set_available(true); // recovery must not undo the degradation
    CHECK_FALSE(rig.diarizer->is_available());
}

TEST_CASE("TestGracefulDegradation::test_is_available_false_when_availability_probe_raises",
          "[test_meeting_diarize]") {
    Rig rig;
    rig.model->fail_availability("boom");
    CHECK_FALSE(rig.diarizer->is_available());
}

TEST_CASE("TestGracefulDegradation::test_degraded_diarizer_still_satisfies_protocol",
          "[test_meeting_diarize]") {
    STATIC_CHECK(std::is_base_of_v<Diarizer, OnlineDiarizer>);
    Rig rig;
    rig.model->fail_with("onnxruntime session crashed");
    Diarizer& diarizer = *rig.diarizer;
    fv::diarize::Segment s = segment("sg_0", 0.0);
    diarizer.assign(s, silence(), 16000);
    CHECK_FALSE(diarizer.is_available());
    diarizer.pin("sg_0", "p_X"); // still accepts corrections without throwing
    CHECK(rig.label("sg_0") == "p_X");
}

// from tests/test_meeting_diarize.py::TestBoundedRecluster
TEST_CASE("TestBoundedRecluster::test_large_meeting_recluster_stays_fast",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(3000);
    Rig rig({vectors[0]});
    rig.prefill(vectors);
    rig.probe->set_new_since_recluster(25);

    const auto started = std::chrono::steady_clock::now();
    rig.assign("sg_new", 5000.0);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;

    // Average linkage over the whole history costs about 33 s at N=3000 in Python; the bound
    // keeps a pass to milliseconds, and this ceiling fails loudly if the O(N^3) cost returns.
    CHECK(elapsed.count() < 2.0);
}

TEST_CASE("TestBoundedRecluster::test_working_set_is_bounded_and_keeps_every_pin",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(2000);
    Rig rig;
    const auto records = rig.prefill(vectors);
    for (std::size_t i = 0; i < 20; ++i) { // old pins, far outside the recent window
        rig.probe->set_pinned(records[i].segment_id, true);
    }

    const auto [selected, anchors] = rig.probe->working_set();

    CHECK(selected.size() <= kMaxWorkingSet);
    CHECK(std::count_if(selected.begin(), selected.end(), [](const auto& r) {
              return !r.pinned;
          }) <= static_cast<std::ptrdiff_t>(kReclusterWindow));
    std::set<std::string> selected_ids;
    for (const auto& record : selected) {
        selected_ids.insert(record.segment_id);
    }
    for (std::size_t i = 0; i < 20; ++i) {
        CHECK(selected_ids.contains(records[i].segment_id));
    }
    // The history outside the window still votes, through one anchor per participant.
    std::set<std::string> anchor_pids;
    for (const auto& anchor : anchors) {
        anchor_pids.insert(anchor.participant_id);
        CHECK(anchor.weight < kPinWeight);
    }
    CHECK(anchor_pids == std::set<std::string>{"p_0", "p_1", "p_2", "p_3"});
}

TEST_CASE("TestBoundedRecluster::test_pinned_segments_keep_assignment_across_large_recluster",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(1200);
    Rig rig;
    rig.prefill(vectors);
    // Deliberately "wrong" pins: the embeddings look like p_0 but a human said p_pinned. An old
    // one and a recent one must both stay.
    for (const char* id : {"sg_4", "sg_1100"}) {
        rig.probe->set_pinned(id, true);
        rig.probe->set_participant(id, "p_pinned");
    }

    const RelabelOps ops = rig.probe->recluster();

    for (const char* id : {"sg_4", "sg_1100"}) {
        CHECK(rig.label(id) == "p_pinned");
        CHECK(rig.probe->record(id)->pinned);
        CHECK_FALSE(targets(ops, id));
    }
}

TEST_CASE("TestBoundedRecluster::test_cluster_quality_preserved_on_four_speaker_fixture",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(1000);
    Rig rig;
    rig.prefill(vectors);

    rig.probe->recluster();

    // Every speaker keeps exactly one participant and no two share one: the bounded pass must
    // not smear identities together.
    std::vector<std::set<std::string>> by_speaker(4);
    const auto records = rig.probe->records();
    for (std::size_t i = 0; i < records.size(); ++i) {
        by_speaker[i % 4].insert(records[i].participant_id.value_or("None"));
    }
    std::set<std::string> distinct;
    for (const auto& pids : by_speaker) {
        CHECK(pids.size() == 1);
        distinct.insert(*pids.begin());
    }
    CHECK(distinct.size() == 4);
}

TEST_CASE("TestBoundedRecluster::test_recent_window_is_reclustered_not_the_whole_history",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(1000);
    Rig rig;
    const auto records = rig.prefill(vectors);
    const std::string stale_old = records.front().segment_id;
    const std::string stale_recent = records.back().segment_id;
    rig.probe->set_participant(stale_old, "p_wrong");
    rig.probe->set_participant(stale_recent, "p_wrong");

    const RelabelOps ops = rig.probe->recluster();

    CHECK(targets(ops, stale_recent));    // inside the window: fixed
    CHECK_FALSE(targets(ops, stale_old)); // outside: left alone
}

// from tests/test_meeting_diarize.py::TestOverMergeSplit
TEST_CASE("TestOverMergeSplit::test_recluster_splits_an_over_merged_participant",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(40, 2);
    Rig rig;
    rig.prefill(vectors, [](std::size_t) { return std::string("p_A"); });

    const RelabelOps ops = rig.probe->recluster();

    CHECK(rig.host.created() == 1); // exactly one new participant
    const std::string new_pid = "p_speaker_1";
    std::set<std::string> first;
    std::set<std::string> second;
    const auto records = rig.probe->records();
    for (std::size_t i = 0; i < records.size(); ++i) {
        (i % 2 == 0 ? first : second).insert(records[i].participant_id.value_or("None"));
    }
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(first != second);
    std::set<std::string> both = first;
    both.insert(second.begin(), second.end());
    CHECK(both == std::set<std::string>{"p_A", new_pid});
    CHECK(ops.size() == 20);
    CHECK(
        std::all_of(ops.begin(), ops.end(), [&](const auto& op) { return op.second == new_pid; }));
}

TEST_CASE("TestOverMergeSplit::test_repeated_reclusters_converge", "[test_meeting_diarize]") {
    // Three voices merged into one participant: one pass separates them, and every later pass
    // changes nothing (no runaway "Speaker N" creation).
    const auto vectors = synthetic_speakers(60, 3);
    Rig rig;
    rig.prefill(vectors, [](std::size_t) { return std::string("p_A"); });

    CHECK_FALSE(rig.probe->recluster().empty());
    CHECK(rig.host.created() == 2);
    for (int i = 0; i < 5; ++i) {
        CHECK(rig.probe->recluster().empty());
    }
    CHECK(rig.host.created() == 2);
    CHECK(rig.probe->clusters().size() == 3);
}

TEST_CASE("TestOverMergeSplit::test_split_participant_is_created_outside_the_diarizer_lock",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(40, 2);
    Rig rig;
    rig.prefill(vectors, [](std::size_t) { return std::string("p_A"); });
    std::vector<bool> free;
    rig.host.on_call([&] { free.push_back(!rig.probe->lock_held_by_this_thread()); });

    rig.probe->recluster();

    REQUIRE_FALSE(free.empty());
    CHECK(std::all_of(free.begin(), free.end(), [](bool value) { return value; }));
}

TEST_CASE("TestOverMergeSplit::test_tiny_group_does_not_spawn_a_phantom_speaker",
          "[test_meeting_diarize]") {
    // One stray pair from a second voice: below kMinSplitSegments, so re-clustering must not
    // invent a participant for it.
    auto vectors = synthetic_speakers(20, 1);
    const auto stray = synthetic_speakers(2, 1, 99);
    vectors.insert(vectors.end(), stray.begin(), stray.end());
    Rig rig;
    rig.prefill(vectors, [](std::size_t) { return std::string("p_A"); });

    STATIC_CHECK(kMinSplitSegments > 2);
    rig.probe->recluster();
    CHECK(rig.host.created() == 0);
}

TEST_CASE("TestOverMergeSplit::test_split_failure_leaves_the_group_merged",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(40, 2);
    Rig rig;
    rig.prefill(vectors, [](std::size_t) { return std::string("p_A"); });
    rig.host.reject_participants(true);

    const RelabelOps ops = rig.probe->recluster(); // must not throw or degrade

    CHECK(ops.empty());
    for (const auto& record : rig.probe->records()) {
        CHECK(record.participant_id == "p_A");
    }
    CHECK(rig.diarizer->is_available());
}

// from tests/test_meeting_diarize.py::TestAverageLinkageScaling
TEST_CASE("TestAverageLinkageScaling::test_linkage_matches_reference_on_small_input",
          "[test_meeting_diarize]") {
    const auto vectors = synthetic_speakers(24, 3);
    const auto groups = average_linkage(distances(vectors), vectors.size(), 0.38);
    REQUIRE(groups.size() == 3);
    for (const auto& group : groups) {
        std::set<std::size_t> speakers;
        for (const std::size_t i : group) {
            speakers.insert(i % 3);
        }
        CHECK(speakers.size() == 1);
    }
}

TEST_CASE("a segment assigned twice keeps its older record out of the anchors", "[clustering]") {
    // Python excludes history by segment id: the older record of a segment whose newer record
    // is in the working set lends no anchor.
    const auto vectors = synthetic_speakers(402);
    Rig rig;
    std::vector<OnlineDiarizerProbe::Record> records;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        const bool duplicate = i == 0 || i + 1 == vectors.size();
        records.push_back({duplicate ? "sg_dup" : "sg_" + std::to_string(i), vectors[i],
                           i == 0 ? "p_old" : "p_" + std::to_string(i % 4), false,
                           static_cast<double>(i), static_cast<double>(i) + 1.0});
    }
    rig.probe->set_records(records);

    const auto [selected, anchors] = rig.probe->working_set();

    CHECK(selected.size() == kReclusterWindow);
    REQUIRE(anchors.size() == 1);
    CHECK(anchors[0].participant_id == "p_1"); // sg_1, the only other record out of the window
    CHECK(anchors[0].weight == 1.0);
}

TEST_CASE("audio at 0 Hz degrades the diarizer instead of resampling", "[clustering]") {
    Rig rig(optional_all({unit({1, 0})}));
    fv::diarize::Segment s = segment("sg_0", 0.0);
    CHECK_FALSE(rig.diarizer->assign(s, silence(), 0));
    CHECK(rig.model->calls() == 0);
    CHECK_FALSE(rig.diarizer->is_available());
    // No audio at all is not resampled, so the rate is never divided by (as in Python).
    Rig empty(optional_all({unit({1, 0})}));
    fv::diarize::Segment t = segment("sg_0", 0.0);
    CHECK_FALSE(empty.diarizer->assign(t, {}, 0));
    CHECK(empty.diarizer->is_available());
}

TEST_CASE("a split participant whose creation throws anything leaves the group merged",
          "[clustering]") {
    class ThrowingHost final : public fv::diarize::testing::FakeDiarizerHost {
    public:
        std::expected<std::string, std::string> create_participant(std::string) override {
            throw 42; // not a std::exception: Python's `except Exception` still catches it
        }
    };
    ThrowingHost host;
    auto fake = fv::diarize::testing::make_fake_embedder();
    OnlineDiarizer diarizer(std::move(fake.embedder), host, "m_diar");
    OnlineDiarizerProbe probe(diarizer);
    const auto vectors = synthetic_speakers(40, 2);
    std::vector<OnlineDiarizerProbe::Record> records;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        records.push_back({"sg_" + std::to_string(i), vectors[i], "p_A", false,
                           static_cast<double>(i), static_cast<double>(i) + 1.0});
    }
    probe.set_records(records);

    CHECK(probe.recluster().empty());
    for (const auto& record : probe.records()) {
        CHECK(record.participant_id == "p_A");
    }
    CHECK(diarizer.is_available());
}

TEST_CASE("an empty current label does not replace the label assign arrived at", "[clustering]") {
    // Python's `if current:` after the re-cluster: an empty participant id is falsy.
    const auto vectors = synthetic_speakers(12, 2, 11);
    std::vector<Embedding> speaker_a;
    std::vector<Embedding> speaker_b;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        (i % 2 == 0 ? speaker_a : speaker_b).push_back(vectors[i]);
    }
    Rig rig({speaker_b.back()});
    std::vector<OnlineDiarizerProbe::Record> records;
    for (std::size_t i = 0; i + 1 < speaker_a.size(); ++i) {
        records.push_back({"sg_a" + std::to_string(i), speaker_a[i], "p_A", false,
                           static_cast<double>(i), static_cast<double>(i) + 1.0});
    }
    for (std::size_t i = 0; i + 1 < speaker_b.size(); ++i) {
        records.push_back({"sg_b" + std::to_string(i), speaker_b[i], "p_B", false,
                           10.0 + static_cast<double>(i), 11.0 + static_cast<double>(i)});
    }
    rig.probe->set_records(records);
    rig.probe->set_clusters({{"p_A", speaker_b[0], static_cast<int>(records.size()), {}}});
    rig.probe->set_new_since_recluster(25);
    rig.probe->after_recluster([&](const RelabelOps&) { rig.diarizer->pin("sg_new", ""); });

    CHECK(rig.assign("sg_new", 50.0) == "p_A");
    CHECK(rig.diarizer->current_label("sg_new") == "");
}

TEST_CASE("TestAverageLinkageScaling::test_singleton_and_empty_inputs", "[test_meeting_diarize]") {
    CHECK(average_linkage({}, 0, 0.38).empty());
    const std::vector<double> one{0.0};
    CHECK(average_linkage(one, 1, 0.38) == std::vector<std::vector<std::size_t>>{{0}});
}
