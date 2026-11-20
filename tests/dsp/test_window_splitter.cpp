// split_point and WindowSplitter against the golden vectors of services.local_asr.audio, and the
// splitter's promise that the windows do not depend on how the stream is cut into frames.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "dsp/window_splitter.h"
#include "golden/golden.h"
#include "signals.h"

using fv::dsp::Window;
using fv::dsp::WindowSplitter;

namespace {

const std::vector<std::int16_t>& stream(const std::string& kind, std::size_t length,
                                        std::int64_t seed) {
    // Every frame size of a case replays the same stream; build each one once.
    static std::map<std::tuple<std::string, std::size_t, std::int64_t>, std::vector<std::int16_t>>
        cache;
    const auto key = std::make_tuple(kind, length, seed);
    auto found = cache.find(key);
    if (found == cache.end()) {
        found = cache.emplace(key, fv::dsp::test::stream_signal(kind, length, seed)).first;
    }
    return found->second;
}

std::vector<float> as_float(const std::vector<std::int16_t>& samples) {
    std::vector<float> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        out[i] = static_cast<float>(samples[i]) / 32768.0f;
    }
    return out;
}

std::string digest(const std::vector<float>& samples) {
    static_assert(std::endian::native == std::endian::little,
                  "the vectors hash float32 little-endian bytes");
    fv::Sha256 hash;
    hash.update(std::as_bytes(std::span(samples)));
    return hash.hexdigest();
}

struct Emitted {
    std::int64_t frame; // index of the push that returned the window, -1 for finish
    Window window;
};

// Pushes the stream in frames of the given sizes, cycling through them.
std::vector<Emitted> split(const std::vector<std::int16_t>& samples,
                           const std::vector<std::size_t>& frame_sizes, WindowSplitter& splitter) {
    std::vector<Emitted> out;
    std::size_t start = 0;
    for (std::int64_t frame = 0; start < samples.size(); ++frame) {
        const std::size_t size = frame_sizes[static_cast<std::size_t>(frame) % frame_sizes.size()];
        const std::size_t count = std::min(size, samples.size() - start);
        for (Window& window : splitter.push(std::span(samples).subspan(start, count))) {
            out.push_back({frame, std::move(window)});
        }
        start += count;
    }
    for (Window& window : splitter.finish()) {
        out.push_back({-1, std::move(window)});
    }
    return out;
}

} // namespace

TEST_CASE("split_point replays the golden vectors", "[golden][window_splitter]") {
    fv::golden::for_each("services.local_asr.audio", "split_point", [](const fv::golden::Case& c) {
        const auto audio =
            as_float(stream(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::int64_t>(2)));
        CHECK(fv::dsp::split_point(audio) == c.result<std::size_t>());
    });
}

TEST_CASE("WindowSplitter replays the golden vectors", "[golden][window_splitter]") {
    fv::golden::for_each(
        "services.local_asr.audio", "WindowSplitter", [](const fv::golden::Case& c) {
            const auto& samples =
                stream(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::int64_t>(2));
            WindowSplitter splitter;
            const auto got = split(samples, {c.arg<std::size_t>(3)}, splitter);

            const fv::Json& expected = c.raw().at("result");
            const fv::Json& windows = expected.at("windows");
            REQUIRE(got.size() == windows.size());
            for (std::size_t i = 0; i < got.size(); ++i) {
                INFO("window " << i);
                CHECK(got[i].frame == fv::golden::decode<std::int64_t>(windows[i][0]));
                CHECK(got[i].window.start_s == fv::golden::decode<double>(windows[i][1]));
                CHECK(got[i].window.samples.size() ==
                      fv::golden::decode<std::size_t>(windows[i][2]));
                CHECK(digest(got[i].window.samples) ==
                      fv::golden::decode<std::string>(windows[i][3]));
            }
            CHECK(splitter.total() == fv::golden::decode<std::int64_t>(expected.at("total")));
            CHECK(splitter.offset() == fv::golden::decode<std::int64_t>(expected.at("offset")));
        });
}

TEST_CASE("the windows are the same for every frame size", "[window_splitter]") {
    const auto [kind, seed] = GENERATE(
        std::make_tuple(std::string("constant"), 1000), std::make_tuple(std::string("sine"), 5),
        std::make_tuple(std::string("silence"), 0), std::make_tuple(std::string("gaps"), 11),
        std::make_tuple(std::string("noise"), 7));
    const std::size_t length = GENERATE(480000u, 1000000u, 2000003u);
    const auto& samples = stream(kind, length, seed);

    WindowSplitter whole;
    const auto reference = split(samples, {samples.size()}, whole);
    REQUIRE(reference.size() >= 2);

    const std::vector<std::vector<std::size_t>> framings{
        {160}, {1024}, {4800}, {16000}, {1, 7, 3999, 480000, 2}, {479999, 1}};
    for (const auto& framing : framings) {
        WindowSplitter splitter;
        const auto got = split(samples, framing, splitter);
        REQUIRE(got.size() == reference.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].window.start_s == reference[i].window.start_s);
            CHECK(got[i].window.samples == reference[i].window.samples);
        }
        CHECK(splitter.total() == static_cast<std::int64_t>(samples.size()));
        CHECK(splitter.offset() == static_cast<std::int64_t>(samples.size()));
    }
}

TEST_CASE("61 s of constant audio splits at 24.1 s and 48.2 s", "[window_splitter]") {
    // 0.1 full scale, a14's example: every block has the same energy, so each cut is one block
    // past the first candidate.
    const std::vector<std::int16_t> samples(61 * 16000, 3277);
    WindowSplitter splitter;
    const auto windows = split(samples, {1024}, splitter);
    REQUIRE(windows.size() == 3);
    CHECK(windows[0].window.start_s == 0.0);
    CHECK(windows[1].window.start_s == 24.1);
    CHECK(windows[2].window.start_s == 48.2);
    CHECK(windows[0].window.samples.size() == 385600);
    CHECK(windows[1].window.samples.size() == 385600);
    CHECK(windows[2].window.samples.size() == 61 * 16000 - 2 * 385600);
    CHECK(windows[2].frame == -1);
    CHECK(windows[0].window.samples.front() == 3277.0f / 32768.0f);
}

TEST_CASE("push holds audio until 30 s are pending", "[window_splitter]") {
    WindowSplitter splitter;
    const std::vector<std::int16_t> almost(fv::dsp::MAX_SAMPLES - 1, 100);
    CHECK(splitter.push(almost).empty());
    CHECK(splitter.push({}).empty());
    CHECK(splitter.total() == fv::dsp::MAX_SAMPLES - 1);
    CHECK(splitter.offset() == 0);

    const std::vector<std::int16_t> one(1, -32768);
    const auto ready = splitter.push(one);
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].start_s == 0.0);
    CHECK(ready[0].samples.size() == 385600);
    CHECK(splitter.offset() == 385600);
    CHECK(splitter.total() == fv::dsp::MAX_SAMPLES);

    const auto rest = splitter.finish();
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].start_s == 24.1);
    CHECK(rest[0].samples.size() == static_cast<std::size_t>(fv::dsp::MAX_SAMPLES - 385600));
    CHECK(rest[0].samples.back() == -1.0f);
    CHECK(splitter.finish().empty());
    CHECK(splitter.offset() == fv::dsp::MAX_SAMPLES);
    CHECK(splitter.total() == fv::dsp::MAX_SAMPLES);
}

TEST_CASE("split_point keeps short audio whole and cuts after the quietest block",
          "[window_splitter]") {
    CHECK(fv::dsp::split_point({}) == 0);
    std::vector<float> audio(fv::dsp::MAX_SAMPLES - 1, 0.5f);
    CHECK(fv::dsp::split_point(audio) == audio.size());

    audio.assign(fv::dsp::MAX_SAMPLES + 5000, 0.5f);
    // Two equally quiet blocks: np.argmin takes the first.
    for (std::size_t block : {17u, 40u}) {
        const std::size_t start = fv::dsp::MIN_SAMPLES + block * 1600;
        std::fill(audio.begin() + start, audio.begin() + start + 1600, 0.01f);
    }
    CHECK(fv::dsp::split_point(audio) == fv::dsp::MIN_SAMPLES + 18 * 1600);

    // Audio past MAX_SAMPLES is never looked at.
    std::fill(audio.begin() + fv::dsp::MAX_SAMPLES, audio.end(), 0.0f);
    CHECK(fv::dsp::split_point(audio) == fv::dsp::MIN_SAMPLES + 18 * 1600);

    // Audio before MIN_SAMPLES is never looked at either.
    std::fill(audio.begin(), audio.begin() + fv::dsp::MIN_SAMPLES, 0.0f);
    CHECK(fv::dsp::split_point(audio) == fv::dsp::MIN_SAMPLES + 18 * 1600);
}

TEST_CASE("split_point follows np.argmin on NaN energies", "[window_splitter]") {
    // np.argmin returns the first NaN, ahead of any smaller number before or after it.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> audio(fv::dsp::MAX_SAMPLES, 0.5f);
    const auto block_start = [](std::size_t block) {
        return static_cast<std::size_t>(fv::dsp::MIN_SAMPLES) + block * 1600;
    };
    std::fill_n(audio.begin() + block_start(3), 1600, 0.0f);
    audio[block_start(20) + 5] = nan;
    audio[block_start(30) + 7] = nan;
    std::fill_n(audio.begin() + block_start(50), 1600, 0.0f);
    CHECK(fv::dsp::split_point(audio) == block_start(21));

    // A NaN in the first block wins outright.
    audio[block_start(0)] = nan;
    CHECK(fv::dsp::split_point(audio) == block_start(1));
}
