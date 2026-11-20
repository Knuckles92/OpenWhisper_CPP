#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "diarize/clustering.h"
#include "diarize/embedder.h"

namespace fv::diarize::testing {

using Embedding = std::vector<float>;

// An EmbeddingModel that returns fixture embeddings instead of running a model.
//
// A segment is recognised by its length: the frame count the diarizer computes from a segment's
// 16 kHz audio (frames_for_samples), so a test that gives each speaker's segments their own
// durations keys embeddings by segment with by_frames. Otherwise the embeddings of the sequence
// come back in call order, the last one repeating once the sequence is used up, as
// OpenWhisper's FakeEmbedder; a nullopt entry stands for audio that gives no embedding. With
// neither, embed throws EmbedderUnavailable.
// Affinity: thread-safe.
class FakeEmbeddingModel final : public EmbeddingModel {
public:
    FakeEmbeddingModel() = default;
    explicit FakeEmbeddingModel(std::vector<std::optional<Embedding>> sequence);

    // The fbank frame count of `samples` samples at 16 kHz (0 below one frame).
    static int frames_for_samples(std::size_t samples);

    void by_frames(int frames, Embedding embedding);
    void append(std::optional<Embedding> embedding);
    // Every later embed throws EmbedderUnavailable(message), as a crashed session does.
    void fail_with(std::string message);
    void set_available(bool available);
    // available() throws std::runtime_error(message), as a probe of a broken session can.
    void fail_availability(std::string message);

    std::optional<Embedding> embed(std::span<const float> fbank, int frames) override;
    bool available() const override;

    int calls() const;
    // The frame counts embed was called with, in order.
    std::vector<int> frames_seen() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::optional<Embedding>> sequence_;
    std::size_t next_ = 0;
    std::map<int, Embedding> by_frames_;
    std::optional<std::string> failure_;
    bool available_ = true;
    std::optional<std::string> availability_failure_;
    std::vector<int> frames_seen_;
};

// A SpeakerEmbedder over a fake model the caller keeps a pointer to.
struct FakeEmbedder {
    std::unique_ptr<SpeakerEmbedder> embedder;
    FakeEmbeddingModel* model = nullptr;
};
FakeEmbedder make_fake_embedder(std::vector<std::optional<Embedding>> sequence = {});

// A fixture of tests/fixtures/diarize: little-endian float32 rows of `dim` values.
std::vector<Embedding> load_embedding_fixture(const std::filesystem::path& file,
                                              std::size_t dim = 192);

// A DiarizerHost that numbers participants "p_speaker_<n>" and records what it was asked, as
// OpenWhisper's FakeStore does. A rejecting host answers its reason.
// Affinity: thread-safe; a probe set with on_call runs before each call is recorded.
class FakeDiarizerHost : public DiarizerHost {
public:
    std::expected<std::string, std::string> create_participant(std::string display_name) override;
    void relabel(const RelabelOps& segment_to_participant) override;
    std::map<std::string, std::string> participant_names() override;

    void reject_participants(bool reject, std::string reason = "human_edited");
    void on_call(std::function<void()> probe);

    int created() const;
    std::vector<std::string> created_names() const;
    std::vector<RelabelOps> relabel_batches() const;
    // Every relabel op, in order across batches.
    RelabelOps relabels() const;

private:
    mutable std::mutex mutex_;
    int created_ = 0;
    bool reject_ = false;
    std::string reject_reason_;
    std::vector<std::string> names_;
    std::map<std::string, std::string> participants_;
    std::vector<RelabelOps> batches_;
    std::function<void()> probe_;
};

} // namespace fv::diarize::testing
