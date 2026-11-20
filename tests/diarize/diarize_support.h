#pragma once

// What the ports of tests/test_meeting_diarize.py share: OpenWhisper's _unit, _segment,
// _make_diarizer, _synthetic_speakers (read from the fixtures that file's own function wrote)
// and _prefill.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/testing/fake_clock.h"
#include "diarize/clustering.h"
#include "diarize/testing/diarizer_probe.h"
#include "diarize/testing/fake_embedding_model.h"
#include "testkit/testkit.h"

namespace fv::diarize::test {

using testing::Embedding;

inline Embedding unit(std::vector<double> values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value * value;
    }
    const double length = std::sqrt(sum);
    Embedding out;
    for (const double value : values) {
        out.push_back(static_cast<float>(value / length));
    }
    return out;
}

inline Segment segment(const std::string& id, double t0) {
    return Segment{id, t0, t0 + 1.0, {}, std::nullopt, "channel"};
}

// One second of silence at 16 kHz: long enough to embed (the fake model ignores it).
inline const std::vector<float>& silence() {
    static const std::vector<float> audio(16000, 0.0f);
    return audio;
}

// _synthetic_speakers(count, speakers=speakers, seed=seed) with dim 192 and sigma 0.035, from
// tests/fixtures/diarize (written by make_fixtures.py from OpenWhisper's own function). A call
// for fewer vectors returns a prefix of a longer one, so each fixture serves every shorter
// count. The four-speaker fixture holds 1200; longer runs repeat it, which keeps vector i a
// sample of speaker i % 4 (1200 is a multiple of 4).
inline std::vector<Embedding> synthetic_speakers(std::size_t count, int speakers = 4,
                                                 int seed = 7) {
    const std::string name = "s" + std::to_string(speakers) + "_seed" + std::to_string(seed);
    static std::map<std::string, std::vector<Embedding>> cache;
    auto found = cache.find(name);
    if (found == cache.end()) {
        const auto file =
            testkit::source_root() / "tests" / "fixtures" / "diarize" / (name + ".f32");
        found = cache.emplace(name, testing::load_embedding_fixture(file)).first;
    }
    const std::vector<Embedding>& fixture = found->second;
    if (count > fixture.size() && (speakers != 4 || seed != 7)) {
        throw std::invalid_argument("the fixture " + name + " holds only " +
                                    std::to_string(fixture.size()) + " vectors");
    }
    std::vector<Embedding> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(fixture[i % fixture.size()]);
    }
    return out;
}

// _make_diarizer: a diarizer over a fake model handing out `vectors` in order, a fake store
// (host) and a clock that only moves when told.
struct Rig {
    explicit Rig(std::vector<std::optional<Embedding>> vectors = {}) {
        auto fake = testing::make_fake_embedder(std::move(vectors));
        model = fake.model;
        diarizer =
            std::make_unique<OnlineDiarizer>(std::move(fake.embedder), host, "m_diar", clock);
        probe = std::make_unique<OnlineDiarizerProbe>(*diarizer);
    }

    std::optional<std::string> assign(const std::string& id, double t0) {
        Segment s = segment(id, t0);
        return diarizer->assign(s, silence(), 16000);
    }

    // _prefill: records sg_<i> with vector i, labelled pid_of(i), at times [i, i + 1).
    std::vector<OnlineDiarizerProbe::Record> prefill(
        const std::vector<Embedding>& vectors,
        const std::function<std::string(std::size_t)>& pid_of = [](std::size_t i) {
            return "p_" + std::to_string(i % 4);
        }) {
        std::vector<OnlineDiarizerProbe::Record> records;
        for (std::size_t i = 0; i < vectors.size(); ++i) {
            records.push_back({"sg_" + std::to_string(i), vectors[i], pid_of(i), false,
                               static_cast<double>(i), static_cast<double>(i) + 1.0});
        }
        probe->set_records(records);
        return records;
    }

    std::optional<std::string> label(const std::string& id) const {
        const auto record = probe->record(id);
        return record ? record->participant_id : std::nullopt;
    }

    FakeClock clock;
    testing::FakeDiarizerHost host;
    testing::FakeEmbeddingModel* model = nullptr;
    std::unique_ptr<OnlineDiarizer> diarizer;
    std::unique_ptr<OnlineDiarizerProbe> probe;
};

inline std::vector<std::optional<Embedding>> optional_all(const std::vector<Embedding>& vectors) {
    return {vectors.begin(), vectors.end()};
}

// 64-bit LCG, each step's top 16 bits as int16 (the golden case files' noise).
inline std::vector<std::int16_t> lcg_int16(std::size_t count, std::uint64_t seed) {
    std::vector<std::int16_t> out(count);
    std::uint64_t state = seed;
    for (auto& value : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        value = static_cast<std::int16_t>(static_cast<std::int64_t>(state >> 48) - 32768);
    }
    return out;
}

// Base64 of little-endian int16, as the golden vectors carry PCM.
inline std::vector<std::int16_t> pcm_from_base64(const std::string& text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const auto index = alphabet.find(c);
        if (index == std::string_view::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    std::vector<std::int16_t> pcm(bytes.size() / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[2 * i]) |
                                           (static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8));
    }
    return pcm;
}

} // namespace fv::diarize::test
