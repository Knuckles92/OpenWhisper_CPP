// from tests/test_meeting_polish_batches.py: bound cleanup work without losing speech or changing
// segment identity.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "meeting/finalization.h"

using fv::Json;
using namespace fv::meeting;

namespace {

using Blocks = std::vector<std::pair<std::size_t, std::size_t>>;

std::size_t text_size(const Json& row) {
    return row.at("text").get<std::string>().size();
}

// Every block is bounded, contiguous and reaches into an unseen segment, and together the blocks
// cover every row in order.
void assert_complete_bounded_coverage(const std::vector<Json>& rows, const Blocks& blocks) {
    std::set<std::size_t> seen;
    std::size_t covered_to = 0;
    for (const auto& [start, end] : blocks) {
        REQUIRE(start < end);
        const std::size_t length = end - start;
        CHECK(length <= kPolishMaxSegments);
        std::size_t chars = 0;
        bool reaches_unseen = false;
        for (std::size_t i = start; i < end; ++i) {
            chars += text_size(rows[i]);
            reaches_unseen = reaches_unseen || !seen.contains(i);
        }
        CHECK((length == 1 || chars <= kPolishMaxTextChars));
        CHECK(reaches_unseen);
        // Blocks only overlap backwards: each starts no later than where coverage ended.
        CHECK(start <= covered_to);
        for (std::size_t i = start; i < end; ++i) {
            seen.insert(i);
        }
        covered_to = end;
    }
    CHECK(seen.size() == rows.size());
    CHECK(covered_to == rows.size());
}

std::vector<Json> short_rows(std::size_t count) {
    std::vector<Json> rows;
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back({{"id", "sg_" + std::to_string(i)}, {"text", "short line"}});
    }
    return rows;
}

} // namespace

TEST_CASE("short segments have complete coverage without context only tail",
          "[test_meeting_polish_batches]") {
    // from
    // tests/test_meeting_polish_batches.py::test_short_segments_have_complete_coverage_without_context_only_tail
    const std::size_t count = GENERATE(0u, 1u, 79u, 80u, 81u, 144u, 152u, 160u, 435u);
    const std::vector<Json> rows = short_rows(count);
    const Blocks blocks = polish_blocks(rows);
    assert_complete_bounded_coverage(rows, blocks);
    if (count == 435) {
        CHECK(blocks.size() == 6);
        std::size_t longest = 0;
        for (const auto& [start, end] : blocks) {
            longest = std::max(longest, end - start);
        }
        CHECK(longest == 80);
        // blocks[0][-8:] == blocks[1][:8]
        CHECK(blocks[0].second - 8 == blocks[1].first);
    }
    if (count == 152) {
        CHECK(blocks.size() == 2);
    }
}

TEST_CASE("long segments use text budget even below segment limit",
          "[test_meeting_polish_batches]") {
    // from
    // tests/test_meeting_polish_batches.py::test_long_segments_use_text_budget_even_below_segment_limit
    std::vector<Json> rows;
    for (int i = 0; i < 12; ++i) {
        rows.push_back({{"id", "sg_" + std::to_string(i)}, {"text", std::string(2'000, 'x')}});
    }
    const Blocks blocks = polish_blocks(rows);
    CHECK(blocks.size() > 1);
    assert_complete_bounded_coverage(rows, blocks);
}

TEST_CASE("oversized segment is preserved and does not block following speech",
          "[test_meeting_polish_batches]") {
    // from
    // tests/test_meeting_polish_batches.py::test_oversized_segment_is_preserved_and_does_not_block_following_speech
    const std::vector<Json> rows = {
        {{"id", "sg_before"}, {"text", "Before."}},
        {{"id", "sg_large"}, {"text", std::string(kPolishMaxTextChars + 1, 'x')}},
        {{"id", "sg_after"}, {"text", "After."}},
    };
    const Blocks blocks = polish_blocks(rows);
    assert_complete_bounded_coverage(rows, blocks);
    REQUIRE(blocks.size() >= 2);
    // The block is the row itself, by index: segment identity cannot change.
    CHECK(blocks[1] == std::pair<std::size_t, std::size_t>{1, 2});
}

TEST_CASE("mixed lengths always advance and preserve text", "[test_meeting_polish_batches]") {
    // from
    // tests/test_meeting_polish_batches.py::test_mixed_lengths_always_advance_and_preserve_text
    // Python draws from random.Random(419); the property holds for any draw, and the exact
    // Python draw is replayed by the meeting.finalization golden vectors.
    std::mt19937 rng(419);
    const std::size_t lengths[] = {0, 30, 200, 2'000, 7'000, 9'000};
    std::uniform_int_distribution<std::size_t> pick(0, std::size(lengths) - 1);
    std::vector<Json> rows;
    for (int i = 0; i < 435; ++i) {
        rows.push_back(
            {{"id", "sg_" + std::to_string(i)}, {"text", std::string(lengths[pick(rng)], 'x')}});
    }
    assert_complete_bounded_coverage(rows, polish_blocks(rows));
}

TEST_CASE("polish blocks measure text in code points", "[meeting][polish]") {
    // 8,000 two-byte characters fit one block: the budget counts characters, not UTF-8 bytes.
    std::string text;
    for (int i = 0; i < 4'000; ++i) {
        text += "é";
    }
    const std::vector<Json> rows = {{{"text", text}}, {{"text", text}}};
    CHECK(polish_blocks(rows) == Blocks{{0, 2}});
}
