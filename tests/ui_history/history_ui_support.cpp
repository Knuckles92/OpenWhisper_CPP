#include "history_ui_support.h"

#include <chrono>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>

#include "audio/wav_writer.h"

namespace fvtest {

void Io::drain() {
    for (int round = 0; round < 50; ++round) {
        const bool queued = executor_.queued() > 0;
        if (queued) {
            offGui([this] { executor_.run_all(); });
        }
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        if (!queued && executor_.queued() == 0) {
            return;
        }
    }
}

LocalHistory::LocalHistory()
    : database_(std::make_unique<fv::Database>(
          offGui([this] { return fv::Database::open_or_create(root_.path() / "test.db"); }))),
      manager_(std::make_unique<fv::HistoryManager>(*database_, root_.path() / "recordings",
                                                    fv::RecordingRetention{})),
      store_(fv::make_history_store(*manager_)) {}

LocalHistory::~LocalHistory() {
    offGui([this] { database_->close(); });
}

fv::HistoryEntry LocalHistory::add(const fv::AddEntry& fields) {
    return *offGui([&] { return manager_->add_entry(fields); });
}

fv::HistoryEntry LocalHistory::addCleaned(fv::AddEntry fields) {
    if (fields.text.empty()) {
        fields.text = "Hello, world.";
    }
    if (!fields.raw_text) {
        fields.raw_text = "um hello world";
    }
    if (fields.model.empty()) {
        fields.model = "base";
    }
    return add(fields);
}

std::optional<fv::HistoryEntry> LocalHistory::get(const std::string& id) {
    return offGui([&] { return manager_->get_entry(id); });
}

std::vector<fv::HistoryEntry> FakeStore::get_history(std::optional<std::int64_t> limit) {
    std::function<std::vector<fv::HistoryEntry>(std::optional<std::int64_t>)> hook;
    {
        const std::lock_guard lock(mutex_);
        hook = onGetHistory;
    }
    if (hook) {
        return hook(limit);
    }
    const std::lock_guard lock(mutex_);
    std::vector<fv::HistoryEntry> out = history;
    if (limit && *limit > 0 && out.size() > static_cast<std::size_t>(*limit)) {
        out.resize(static_cast<std::size_t>(*limit));
    }
    return out;
}

std::vector<fv::HistoryEntry> FakeStore::search(std::string_view /*query*/,
                                                std::optional<std::int64_t> limit) {
    return get_history(limit);
}

std::optional<fv::HistoryEntry> FakeStore::get_entry(std::string_view entry_id) {
    const std::lock_guard lock(mutex_);
    const auto found = entries.find(std::string(entry_id));
    if (found == entries.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<fv::HistoryEntry> FakeStore::last_dictation() {
    return std::nullopt;
}

void FakeStore::forget_dictation(std::string_view entry_id) {
    std::lock_guard lock(mutex_);
    forgotten_.emplace_back(entry_id);
}

std::vector<std::string> FakeStore::forgotten() const {
    std::lock_guard lock(mutex_);
    return forgotten_;
}

std::optional<fv::HistoryEntry> FakeStore::use_version(std::string_view /*entry_id*/,
                                                       std::string_view /*version*/) {
    return std::nullopt;
}

bool FakeStore::delete_entry(std::string_view entry_id, bool delete_audio_file) {
    const std::lock_guard lock(mutex_);
    deletes_.emplace_back(std::string(entry_id), delete_audio_file);
    return deleteResult;
}

void FakeStore::clear_history() {
    const std::lock_guard lock(mutex_);
    ++clears_;
}

void FakeStore::clear_history_and_recordings() {
    const std::lock_guard lock(mutex_);
    ++clears_;
}

std::optional<std::filesystem::path> FakeStore::get_recording_path(std::string_view filename) {
    const std::lock_guard lock(mutex_);
    const auto found = recordings.find(std::string(filename));
    if (found == recordings.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::filesystem::path FakeStore::recordings_folder() const {
    return folder;
}

std::vector<std::pair<std::string, bool>> FakeStore::deletes() const {
    const std::lock_guard lock(mutex_);
    return deletes_;
}

int FakeStore::clears() const {
    const std::lock_guard lock(mutex_);
    return clears_;
}

FakeDevice::FakeDevice()
    : backend_(std::make_shared<fv::audio::testing::FakeBackend>()),
      previous_(fv::audio::exchange_player(std::make_shared<fv::audio::Player>(backend_))) {}

FakeDevice::~FakeDevice() {
    fv::audio::stop_playback();
    fv::audio::exchange_player(previous_);
}

std::filesystem::path writeRecording(const std::filesystem::path& path, double seconds) {
    std::filesystem::create_directories(path.parent_path());
    const auto frames = static_cast<std::size_t>(44100 * seconds);
    std::vector<std::int16_t> samples(frames);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(static_cast<int>(i % 2000) + 1200);
    }
    offGui([&] {
        fv::audio::WavWriter writer(path, {.rate = 44100, .channels = 1, .sample_width = 2});
        writer.write(std::span<const std::int16_t>(samples));
        writer.commit(std::nullopt);
    });
    return path;
}

bool waitFor(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

fv::HistoryEntry makeEntry(std::string text, std::string model) {
    fv::NewEntry fields;
    fields.text = std::move(text);
    fields.model = std::move(model);
    return fv::create_entry(fields);
}

QString q(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

} // namespace fvtest
