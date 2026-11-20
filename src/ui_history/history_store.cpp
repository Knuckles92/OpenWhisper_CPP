#include "ui_history/history_store.h"

#include "history/manager.h"

namespace fv {
namespace {

class ManagerStore final : public HistoryStore {
public:
    explicit ManagerStore(HistoryManager& manager) : manager_(manager) {}

    std::vector<HistoryEntry> get_history(std::optional<std::int64_t> limit) override {
        return manager_.get_history(limit);
    }
    std::vector<HistoryEntry> search(std::string_view query,
                                     std::optional<std::int64_t> limit) override {
        return manager_.search(query, limit);
    }
    std::optional<HistoryEntry> get_entry(std::string_view entry_id) override {
        return manager_.get_entry(entry_id);
    }
    std::optional<HistoryEntry> last_dictation() override { return manager_.last_dictation(); }
    void forget_dictation(std::string_view entry_id) override {
        manager_.forget_dictation(entry_id);
    }
    std::optional<HistoryEntry> use_version(std::string_view entry_id,
                                            std::string_view version) override {
        return manager_.use_version(entry_id, version);
    }
    bool delete_entry(std::string_view entry_id, bool delete_audio_file) override {
        return manager_.delete_entry(entry_id, delete_audio_file);
    }
    void clear_history() override { manager_.clear_history(); }
    void clear_history_and_recordings() override { manager_.clear_history_and_recordings(); }
    std::optional<std::filesystem::path> get_recording_path(std::string_view filename) override {
        return manager_.get_recording_path(filename);
    }
    std::filesystem::path recordings_folder() const override {
        return manager_.recordings_folder();
    }

private:
    HistoryManager& manager_;
};

} // namespace

std::shared_ptr<HistoryStore> make_history_store(HistoryManager& manager) {
    return std::make_shared<ManagerStore>(manager);
}

} // namespace fv
