// The golden vectors of meeting.diarize.clustering: average linkage over fixed matrices, and
// OnlineDiarizer replays of scripted embeddings and pins (tools/parity/cases/
// meeting.diarize.clustering.py describes both).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "diarize/clustering.h"
#include "diarize/detail/average_linkage.h"
#include "diarize_support.h"
#include "golden/golden.h"

using fv::Json;
using fv::diarize::Segment;
using fv::diarize::detail::average_linkage;
using fv::diarize::test::Embedding;
using fv::diarize::test::Rig;
using fv::diarize::test::silence;

namespace {

Json optional_text(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

} // namespace

TEST_CASE("average_linkage replays the golden vectors", "[golden][clustering]") {
    fv::golden::for_each("meeting.diarize.clustering", "_average_linkage",
                         [](const fv::golden::Case& c) {
                             const Json& matrix = c.args()[0];
                             const std::size_t n = matrix.size();
                             std::vector<double> dist;
                             for (const Json& row : matrix) {
                                 REQUIRE(row.size() == n);
                                 for (const Json& value : row) {
                                     dist.push_back(fv::golden::decode<double>(value));
                                 }
                             }
                             const auto groups = average_linkage(dist, n, c.arg<double>(1));
                             CHECK(Json(groups) == c.raw().at("result"));
                         });
}

TEST_CASE("OnlineDiarizer replays the golden scripts", "[golden][clustering]") {
    fv::golden::for_each("meeting.diarize.clustering", "replay", [](const fv::golden::Case& c) {
        const Json& events = c.args()[0];
        std::vector<std::optional<Embedding>> vectors;
        for (const Json& event : events) {
            if (event[0] == "assign") {
                vectors.push_back(event[4].is_null()
                                      ? std::nullopt
                                      : std::optional<Embedding>(
                                            fv::golden::decode<std::vector<float>>(event[4])));
            }
        }
        Rig rig(vectors);
        Json labels = Json::array();
        std::vector<std::string> named;
        for (const Json& event : events) {
            const auto id = event[1].get<std::string>();
            if (std::find(named.begin(), named.end(), id) == named.end()) {
                named.push_back(id);
            }
            if (event[0] == "assign") {
                Segment s{id,
                          fv::golden::decode<double>(event[2]),
                          fv::golden::decode<double>(event[3]),
                          {},
                          std::nullopt,
                          "channel"};
                labels.push_back(optional_text(rig.diarizer->assign(s, silence(), 16000)));
            } else {
                rig.diarizer->pin(id, event[2].get<std::string>());
            }
        }

        Json ops = Json::array();
        for (const auto& batch : rig.host.relabel_batches()) {
            Json pairs = Json::array();
            for (const auto& [segment_id, participant] : batch) {
                pairs.push_back(Json::array({segment_id, participant}));
            }
            ops.push_back(pairs);
        }
        Json final_labels = Json::array();
        for (const auto& id : named) {
            final_labels.push_back(
                Json::array({id, optional_text(rig.diarizer->current_label(id))}));
        }
        Json clusters = Json::array();
        for (const auto& cluster : rig.probe->clusters()) {
            clusters.push_back(
                Json::array({cluster.participant_id, cluster.count,
                             std::vector<std::string>(cluster.pinned_segments.begin(),
                                                      cluster.pinned_segments.end())}));
        }

        const Json& expected = c.raw().at("result");
        CHECK(labels == expected.at("labels"));
        CHECK(ops == expected.at("ops"));
        CHECK(Json(rig.host.created_names()) == expected.at("participants"));
        CHECK(final_labels == expected.at("final"));
        CHECK(clusters == expected.at("clusters"));
        CHECK(rig.diarizer->is_available());
    });
}
