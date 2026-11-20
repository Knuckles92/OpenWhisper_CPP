// PcmTape: the append-only store a recording's PCM lives in (architecture 3.10).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include "audio/pcm_tape.h"

namespace {

std::vector<std::int16_t> ramp(std::size_t count, std::int16_t first = 0) {
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<std::int16_t>(first + static_cast<std::int16_t>(i % 30000));
    }
    return samples;
}

std::vector<std::int16_t> copy(const fv::audio::TapeView& view) {
    std::vector<std::int16_t> out(static_cast<std::size_t>(view.frames() * view.channels()));
    view.copy_to(out);
    return out;
}

} // namespace

TEST_CASE("a tape keeps every appended frame across its one second chunks") {
    fv::audio::PcmTape tape(1000);
    const auto samples = ramp(3500);
    tape.append(std::span(samples).first(700));
    tape.append(std::span(samples).subspan(700, 2000));
    tape.append(std::span(samples).subspan(2700));
    CHECK(tape.frames() == 3500);
    CHECK(tape.rate() == 1000);

    const auto view = tape.view(0, 3500);
    CHECK(copy(view) == samples);
    const auto middle = tape.view(999, 2001);
    CHECK(middle.first() == 999);
    CHECK(middle.last() == 2001);
    CHECK(middle.frames() == 1002);
    CHECK(copy(middle) == std::vector<std::int16_t>(samples.begin() + 999, samples.begin() + 2001));
    CHECK(tape.view(1000, 1000).frames() == 0);
}

TEST_CASE("a view outlives growth and the tape itself") {
    std::vector<std::int16_t> expected;
    fv::audio::TapeView view;
    {
        fv::audio::PcmTape tape(100, 2);
        const auto samples = ramp(300);
        tape.append(samples);
        view = tape.view(10, 140);
        expected.assign(samples.begin() + 20, samples.begin() + 280);
        tape.append(ramp(1000, 5));
    }
    CHECK(view.channels() == 2);
    CHECK(copy(view) == expected);
}

TEST_CASE("a tape refuses partial frames and views past its end") {
    fv::audio::PcmTape tape(100, 2);
    CHECK_THROWS_AS(tape.append(std::vector<std::int16_t>(3, 0)), std::invalid_argument);
    tape.append(std::vector<std::int16_t>(4, 0));
    CHECK_THROWS_AS(tape.view(0, 3), std::out_of_range);
    CHECK_THROWS_AS(tape.view(2, 1), std::out_of_range);
    CHECK_THROWS_AS(fv::audio::PcmTape(0), std::invalid_argument);
}

TEST_CASE("readers see whole frames while the writer appends") {
    fv::audio::PcmTape tape(1600);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int block = 0; block < 400; ++block) {
            std::vector<std::int16_t> samples(160, static_cast<std::int16_t>(block));
            tape.append(samples);
        }
        done = true;
    });
    bool consistent = true;
    while (!done) {
        const std::int64_t frames = tape.frames();
        const auto samples = copy(tape.view(0, frames));
        for (std::size_t i = 0; i < samples.size(); ++i) {
            consistent = consistent && samples[i] == static_cast<std::int16_t>(i / 160);
        }
    }
    writer.join();
    CHECK(consistent);
    CHECK(tape.frames() == 64000);
}
