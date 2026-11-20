#include "diarize/testing/fake_embedding_model.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "base/utf8.h"
#include "dsp/fbank.h"

namespace fv::diarize::testing {

FakeEmbeddingModel::FakeEmbeddingModel(std::vector<std::optional<Embedding>> sequence)
    : sequence_(std::move(sequence)) {}

int FakeEmbeddingModel::frames_for_samples(std::size_t samples) {
    if (samples < static_cast<std::size_t>(dsp::kFbankFrameLength)) {
        return 0;
    }
    return static_cast<int>((samples - dsp::kFbankFrameLength) / dsp::kFbankFrameShift + 1);
}

void FakeEmbeddingModel::by_frames(int frames, Embedding embedding) {
    std::lock_guard lock(mutex_);
    by_frames_[frames] = std::move(embedding);
}

void FakeEmbeddingModel::append(std::optional<Embedding> embedding) {
    std::lock_guard lock(mutex_);
    sequence_.push_back(std::move(embedding));
}

void FakeEmbeddingModel::fail_with(std::string message) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(message);
}

void FakeEmbeddingModel::set_available(bool available) {
    std::lock_guard lock(mutex_);
    available_ = available;
}

void FakeEmbeddingModel::fail_availability(std::string message) {
    std::lock_guard lock(mutex_);
    availability_failure_ = std::move(message);
}

std::optional<Embedding> FakeEmbeddingModel::embed(std::span<const float> fbank, int frames) {
    std::lock_guard lock(mutex_);
    frames_seen_.push_back(frames);
    if (fbank.size() != static_cast<std::size_t>(frames) * dsp::kFbankMelBins) {
        throw EmbedderUnavailable("FakeEmbeddingModel: the features do not match the frames");
    }
    if (failure_) {
        throw EmbedderUnavailable(*failure_);
    }
    if (const auto keyed = by_frames_.find(frames); keyed != by_frames_.end()) {
        return keyed->second;
    }
    if (sequence_.empty()) {
        throw EmbedderUnavailable("FakeEmbeddingModel has no embedding for " +
                                  std::to_string(frames) + " frames");
    }
    if (next_ >= sequence_.size()) {
        return sequence_.back();
    }
    return sequence_[next_++];
}

bool FakeEmbeddingModel::available() const {
    std::lock_guard lock(mutex_);
    if (availability_failure_) {
        throw std::runtime_error(*availability_failure_);
    }
    return available_;
}

int FakeEmbeddingModel::calls() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(frames_seen_.size());
}

std::vector<int> FakeEmbeddingModel::frames_seen() const {
    std::lock_guard lock(mutex_);
    return frames_seen_;
}

FakeEmbedder make_fake_embedder(std::vector<std::optional<Embedding>> sequence) {
    auto model = std::make_unique<FakeEmbeddingModel>(std::move(sequence));
    FakeEmbeddingModel* raw = model.get();
    return {std::make_unique<SpeakerEmbedder>(std::move(model)), raw};
}

std::vector<Embedding> load_embedding_fixture(const std::filesystem::path& file, std::size_t dim) {
    static_assert(std::endian::native == std::endian::little,
                  "the fixtures are little-endian float32");
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open the embedding fixture " + path_to_utf8(file));
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t row_bytes = dim * sizeof(float);
    if (dim == 0 || bytes.size() % row_bytes != 0) {
        throw std::runtime_error("The embedding fixture " + path_to_utf8(file) +
                                 " is not whole rows of " + std::to_string(dim) + " floats");
    }
    std::vector<Embedding> rows(bytes.size() / row_bytes, Embedding(dim));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::memcpy(rows[r].data(), bytes.data() + r * row_bytes, row_bytes);
    }
    return rows;
}

std::expected<std::string, std::string>
FakeDiarizerHost::create_participant(std::string display_name) {
    std::function<void()> probe;
    {
        std::lock_guard lock(mutex_);
        probe = probe_;
    }
    if (probe) {
        probe();
    }
    std::lock_guard lock(mutex_);
    if (reject_) {
        return std::unexpected(reject_reason_);
    }
    ++created_;
    std::string id = "p_speaker_" + std::to_string(created_);
    names_.push_back(display_name);
    participants_[id] = std::move(display_name);
    return id;
}

void FakeDiarizerHost::relabel(const RelabelOps& segment_to_participant) {
    std::function<void()> probe;
    {
        std::lock_guard lock(mutex_);
        probe = probe_;
    }
    if (probe) {
        probe();
    }
    std::lock_guard lock(mutex_);
    batches_.push_back(segment_to_participant);
}

std::map<std::string, std::string> FakeDiarizerHost::participant_names() {
    std::lock_guard lock(mutex_);
    return participants_;
}

void FakeDiarizerHost::reject_participants(bool reject, std::string reason) {
    std::lock_guard lock(mutex_);
    reject_ = reject;
    reject_reason_ = std::move(reason);
}

void FakeDiarizerHost::on_call(std::function<void()> probe) {
    std::lock_guard lock(mutex_);
    probe_ = std::move(probe);
}

int FakeDiarizerHost::created() const {
    std::lock_guard lock(mutex_);
    return created_;
}

std::vector<std::string> FakeDiarizerHost::created_names() const {
    std::lock_guard lock(mutex_);
    return names_;
}

std::vector<RelabelOps> FakeDiarizerHost::relabel_batches() const {
    std::lock_guard lock(mutex_);
    return batches_;
}

RelabelOps FakeDiarizerHost::relabels() const {
    std::lock_guard lock(mutex_);
    RelabelOps all;
    for (const RelabelOps& batch : batches_) {
        all.insert(all.end(), batch.begin(), batch.end());
    }
    return all;
}

} // namespace fv::diarize::testing
