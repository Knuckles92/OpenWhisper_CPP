// SpeakerEmbedder over a fake model, the ONNX Runtime loader's refusals, create_diarizer's
// fallbacks, and tests/test_meeting_diarize.py::test_embedding_parity_against_reference_vectors
// as a [decode] test against the golden embeddings of meeting.diarize.embedder.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "diarize/clustering.h"
#include "diarize/embedder.h"
#include "diarize/ort_api.h"
#include "diarize_support.h"
#include "dsp/fbank.h"
#include "golden/golden.h"

namespace fs = std::filesystem;
using fv::diarize::create_diarizer;
using fv::diarize::EmbedderUnavailable;
using fv::diarize::OrtEmbeddingModel;
using fv::diarize::OrtRuntime;
using fv::diarize::SpeakerEmbedder;
using fv::diarize::test::pcm_from_base64;
using fv::diarize::testing::FakeDiarizerHost;
using fv::diarize::testing::FakeEmbeddingModel;
using fv::diarize::testing::make_fake_embedder;

namespace {

// A fresh folder under the system temp folder, removed with everything in it at the end.
struct TempFolder {
    TempFolder() {
        static std::atomic<int> counter{0};
        std::random_device seed;
        path = fs::temp_directory_path() /
               ("fv-diarize-test-" + std::to_string(seed()) + "-" + std::to_string(++counter));
        fs::create_directories(path);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

void write_file(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

std::vector<float> noise(std::size_t count) {
    const auto pcm = fv::diarize::test::lcg_int16(count, 3);
    std::vector<float> out(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return out;
}

} // namespace

TEST_CASE("audio shorter than 0.8 s gives no embedding and never reaches the model", "[embedder]") {
    auto fake = make_fake_embedder({fv::diarize::test::unit({1, 0})});
    CHECK_FALSE(fake.embedder->embed(noise(12799)));
    CHECK(fake.model->calls() == 0);
    CHECK(fake.embedder->embed(noise(12800)));
    CHECK(fake.model->calls() == 1);
}

TEST_CASE("the model gets mean-normalised features and the embedding comes back unit length",
          "[embedder]") {
    auto fake = make_fake_embedder({std::vector<float>{3.0f, 4.0f}});
    const auto embedding = fake.embedder->embed(noise(16000));
    REQUIRE(embedding);
    CHECK(std::abs((*embedding)[0] - 0.6f) < 1e-6f);
    CHECK(std::abs((*embedding)[1] - 0.8f) < 1e-6f);
    CHECK(fake.model->frames_seen() == std::vector<int>{98});
}

TEST_CASE("an embedding without a usable norm is no embedding", "[embedder]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto fake = make_fake_embedder(
        {std::vector<float>{0.0f, 0.0f}, std::vector<float>{nan, 1.0f}, std::nullopt});
    CHECK_FALSE(fake.embedder->embed(noise(16000)));
    CHECK_FALSE(fake.embedder->embed(noise(16000)));
    CHECK_FALSE(fake.embedder->embed(noise(16000)));
}

TEST_CASE("a failing model reaches the caller as EmbedderUnavailable", "[embedder]") {
    auto fake = make_fake_embedder({fv::diarize::test::unit({1, 0})});
    fake.model->fail_with("onnxruntime session crashed");
    CHECK_THROWS_AS(fake.embedder->embed(noise(16000)), EmbedderUnavailable);
    CHECK(fake.embedder->available());
    fake.model->set_available(false);
    CHECK_FALSE(fake.embedder->available());
}

TEST_CASE("ONNX Runtime that is not there is refused with a sentence", "[embedder]") {
    TempFolder temp;
    const auto empty = OrtRuntime::load({});
    REQUIRE_FALSE(empty);
    CHECK(empty.error() == "No ONNX Runtime library is set.");

    const auto missing = OrtRuntime::load(temp.path / "onnxruntime.dll");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().starts_with("ONNX Runtime was not found at "));
    CHECK(missing.error().ends_with("onnxruntime.dll."));
}

TEST_CASE("a file that is not a library is refused, without a system dialog",
          "[embedder][platform]") {
    TempFolder temp;
    const fs::path fake_dll = temp.path / "onnxruntime.dll";
    write_file(fake_dll, "not a library");
    const auto loaded = OrtRuntime::load(fake_dll);
    REQUIRE_FALSE(loaded);
#ifdef _WIN32
    CHECK(loaded.error().starts_with("Could not load ONNX Runtime from "));
#else
    CHECK(loaded.error() == "Speaker identification is not supported on this platform.");
#endif
    CHECK_THROWS_AS(OrtEmbeddingModel::load(fake_dll, temp.path / "model.onnx"),
                    EmbedderUnavailable);
}

TEST_CASE("create_diarizer gives no diarizer without a model, a model file or a runtime",
          "[embedder]") {
    TempFolder temp;
    FakeDiarizerHost host;
    CHECK_FALSE(create_diarizer({}, temp.path / "onnxruntime.dll", host));
    CHECK_FALSE(create_diarizer(temp.path / "missing.onnx", temp.path / "onnxruntime.dll", host));
    const fs::path model = temp.path / "model.onnx";
    write_file(model, "not a model");
    CHECK_FALSE(create_diarizer(model, temp.path / "onnxruntime.dll", host));
    CHECK(host.created() == 0);
}

// from tests/test_meeting_diarize.py::test_embedding_parity_against_reference_vectors: the
// filterbank and ONNX path must match Python's. It guards the whole front end (window,
// pre-emphasis, mel band edges, CMN) against drift that would degrade every embedding.
TEST_CASE("test_embedding_parity_against_reference_vectors", "[decode][test_meeting_diarize]") {
    const auto runtime = fv::testkit::get_env("FIORIVOCE_TEST_ONNXRUNTIME");
    const auto model = fv::testkit::get_env("FIORIVOCE_TEST_SPEAKER_MODEL");
    if (!runtime || !model) {
        SKIP("Set FIORIVOCE_TEST_ONNXRUNTIME and FIORIVOCE_TEST_SPEAKER_MODEL to run the "
             "embedding parity gate.");
    }
    const fs::path runtime_path = fv::path_from_utf8(*runtime);
    const fs::path model_path = fv::path_from_utf8(*model);

    auto loaded = OrtEmbeddingModel::load(runtime_path, model_path);
    CHECK(loaded->available());
    CHECK_FALSE(loaded->input_name().empty());
    SpeakerEmbedder embedder(std::move(loaded));

    fv::golden::for_each("meeting.diarize.embedder", "embed", [&](const fv::golden::Case& c) {
        const fv::Json& expected = c.raw().at("result");
        const auto pcm = pcm_from_base64(expected.at("pcm").get<std::string>());
        REQUIRE(pcm.size() == expected.at("samples").get<std::size_t>());
        std::vector<float> audio(pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            audio[i] = static_cast<float>(pcm[i]) / 32768.0f;
        }
        auto reference = fv::golden::decode<std::vector<double>>(expected.at("embedding"));
        double length = 0.0;
        for (const double value : reference) {
            length += value * value;
        }
        length = std::sqrt(length);

        const auto produced = embedder.embed(audio);
        REQUIRE(produced); // an embedding for every clip
        REQUIRE(produced->size() == reference.size());
        double cosine = 0.0;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            cosine += static_cast<double>((*produced)[i]) * reference[i] / length;
        }
        INFO(c.arg<std::string>(0) << ": cosine " << cosine);
        CHECK(cosine >= 0.999);
    });

    FakeDiarizerHost host;
    const auto diarizer = create_diarizer(model_path, runtime_path, host, "m_decode");
    REQUIRE(diarizer);
    CHECK(diarizer->is_available());
}
