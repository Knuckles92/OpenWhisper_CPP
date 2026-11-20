// Meeting-state SQL writes (OpenWhisper's docs/performance-implementation-2026-10-03.md, "Measured
// changes", and docs/benchmarks/performance-implementation-2026-10-03.json "state_writes"): a
// 739,068-byte document with 210 card items over seven cards and six reports, in a fresh SQLite
// database, written through the real store and repository. Per operation, five trials of three
// warmups and fifteen timed writes, the median of the 75 samples, as OpenWhisper's probe ran them.
// OpenWhisper's probe script is not committed (it lived in .tmp/), so the three operations are
// OpenWhisper_CPP's nearest: a runtime field (update_runtime_fields), the rolling summary
// (set_rolling_summary) and a card item's text (update_item), each by the host.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "db/database.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/store.h"
#include "support/perf_support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::meeting_state;

constexpr std::size_t kDocumentBytes = 739'068;
constexpr int kItemsPerCard = 30; // 7 cards x 30 = 210 items
constexpr int kReports = 6;
constexpr int kTrials = 5;
constexpr int kWarmups = 3;
constexpr int kTimed = 15;

std::string filler(std::size_t length, char seed) {
    static const std::string words =
        "the team agreed to revisit the launch plan after the budget review and to "
        "share the draft with design before friday while support collects the open "
        "questions from customers ";
    std::string out;
    out.reserve(length);
    std::size_t at = static_cast<std::size_t>(seed) % words.size();
    while (out.size() < length) {
        out.push_back(words[at]);
        at = (at + 1) % words.size();
    }
    return out;
}

MeetingState largeDocument(const std::string& meetingId) {
    MeetingState state;
    state.meeting_id = meetingId;
    state.title = "Quarterly planning";
    state.rolling_summary = filler(1200, 'a');
    for (std::size_t card = 0; card < CARD_KEYS.size(); ++card) {
        for (int index = 0; index < kItemsPerCard; ++index) {
            CardItem item;
            item.card = std::string(CARD_KEYS[card]);
            item.id = "it_" + item.card + "_" + std::to_string(index);
            item.text = filler(220, static_cast<char>(card * 31 + index));
            item.data = Json{{"detail", filler(900, static_cast<char>(index))},
                             {"owner", "Sam"},
                             {"due", "2026-11-30"}};
            item.evidence = {"sg_" + std::to_string(index), "sg_" + std::to_string(index + 1)};
            item.created_at = "2026-11-19T09:00:00.000000Z";
            item.updated_at = item.created_at;
            state.cards[card].push_back(std::move(item));
        }
    }
    for (int index = 0; index < kReports; ++index) {
        CustomReport report;
        report.id = "rp_" + std::to_string(index);
        report.request = "Summarise the risks for the steering group";
        report.status = "done";
        report.title = "Report " + std::to_string(index);
        report.markdown = filler(40'000, static_cast<char>(index));
        report.created_at = "2026-11-19T09:30:00.000000Z";
        report.updated_at = report.created_at;
        state.custom_reports.push_back(std::move(report));
    }
    // Pad the last report so the document is the size OpenWhisper's probe wrote.
    const std::size_t size = state.to_json().dump().size();
    if (size < kDocumentBytes) {
        state.custom_reports.back().markdown += filler(kDocumentBytes - size, 'z');
    }
    return state;
}

struct Measured {
    std::vector<double> runtime, summary, card;
    std::size_t bytes = 0;
    std::vector<std::string> errors;
};

Measured measure() {
    Measured out;
    fv::testkit::TempDataRoot root;
    auto database = std::make_unique<fv::Database>(
        fv::Database::open_or_create(root.path() / "openwhisper-cpp.db"));
    auto repo = std::make_shared<SqlMeetingRepository>(*database);
    const std::string meetingId = "m_perf";
    repo->create_meeting({{"id", meetingId},
                          {"title", "Quarterly planning"},
                          {"status", "active"},
                          {"started_at", "2026-11-19T09:00:00.000001"},
                          {"host_token", "host-token"},
                          {"guest_token", "guest-token"},
                          {"cloud_enabled", false},
                          {"spool_dir", (root.path() / "spool").string()}});
    MeetingState document = largeDocument(meetingId);
    out.bytes = document.to_json().dump().size();
    MeetingStateStore store(std::move(document), repo);

    int serial = 0;
    const auto runtimeWrite = [&] {
        return store.update_runtime_fields(Json{{"intelligence_online", ++serial % 2 == 0}});
    };
    const auto summaryWrite = [&] {
        const auto results =
            store.apply("host", std::nullopt,
                        Json::array({{{"op", "set_rolling_summary"},
                                      {"text", "Summary revision " + std::to_string(++serial)}}}));
        if (results.empty() || !results.front().ok) {
            out.errors.push_back("summary: " + (results.empty()
                                                    ? std::string("no result")
                                                    : results.front().reason.value_or("?")));
        }
        return !results.empty() && results.front().ok;
    };
    const auto cardWrite = [&] {
        ++serial;
        const std::string card(CARD_KEYS[static_cast<std::size_t>(serial) % CARD_KEYS.size()]);
        const std::string id = "it_" + card + "_" + std::to_string(serial % kItemsPerCard);
        const auto results =
            store.apply("host", std::nullopt,
                        Json::array({{{"op", "update_item"},
                                      {"id", id},
                                      {"set", {{"text", "Edited " + std::to_string(serial)}}}}}));
        if (results.empty() || !results.front().ok) {
            out.errors.push_back("card: " + (results.empty()
                                                 ? std::string("no result")
                                                 : results.front().reason.value_or("?")));
        }
        return !results.empty() && results.front().ok;
    };

    // Alternating trials, as OpenWhisper's probe alternated its before and after sources.
    for (int trial = 0; trial < kTrials; ++trial) {
        const std::pair<std::vector<double>*, std::function<bool()>> operations[] = {
            {&out.runtime, runtimeWrite}, {&out.summary, summaryWrite}, {&out.card, cardWrite}};
        for (const auto& [samples, write] : operations) {
            for (int warm = 0; warm < kWarmups; ++warm) {
                write();
            }
            for (int timed = 0; timed < kTimed; ++timed) {
                const auto started = fvperf::Clock::now();
                const bool ok = write();
                const double elapsed = fvperf::msSince(started);
                if (ok) {
                    samples->push_back(elapsed);
                }
            }
        }
    }
    database->close();
    return out;
}

} // namespace

TEST_CASE("meeting state SQL writes on a 739,068-byte document", "[perf]") {
    // Off the GUI thread: the database refuses blocking calls there in a debug build.
    const Measured measured = std::async(std::launch::async, measure).get();
    INFO((measured.errors.empty() ? std::string() : measured.errors.front()));
    CHECK(measured.errors.empty());
    CHECK(measured.bytes >= kDocumentBytes);
    const Json notes{{"document_bytes", measured.bytes},
                     {"card_items", 7 * kItemsPerCard},
                     {"custom_reports", kReports},
                     {"method", "5 trials x (3 warmups + 15 timed writes); p50 of 75 samples"}};
    const std::string test = "perf.meeting state SQL writes on a 739,068-byte document";
    const std::pair<const char*, const std::vector<double>*> metrics[] = {
        {"meeting_state_runtime_write_ms", &measured.runtime},
        {"meeting_state_summary_patch_ms", &measured.summary},
        {"meeting_state_card_edit_ms", &measured.card}};
    for (const auto& [metric, samples] : metrics) {
        CAPTURE(metric);
        REQUIRE(samples->size() == static_cast<std::size_t>(kTrials * kTimed));
        const auto recorded = fvperf::record(metric, "ms", *samples, test, notes);
        CHECK(fvperf::underCeiling(recorded, recorded.p50));
    }
}
