// Sweep S2 (open-items A25, A26): the Dictionary page's engine line names the API server the
// Voice model page selected, and a dictionary save that fails is logged as
// settings_dictionary._mutate logs it.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "base/log.h"
#include "pages_support.h"
#include "ui_personalize/dictionary_page.h"
#include "ui_personalize/personalize_host.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

// The records the log facade gets while the test lives.
class LogCapture final : public fv::log::Sink {
public:
    LogCapture() : self_(std::shared_ptr<Sink>(std::shared_ptr<Sink>(), this)) {
        fv::log::add_sink(self_);
    }
    ~LogCapture() override { fv::log::remove_sink(self_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        text_ += std::string(record.level == fv::log::Level::Warning ? "WARNING " : "OTHER ") +
                 std::string(record.logger) + ": " + std::string(record.message) + "\n";
    }
    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
    std::shared_ptr<Sink> self_;
};

} // namespace

// dictionary_page::engine_info with the API engine: the page passes the selected server, so the
// line names it and says the words go there.
TEST_CASE("the Dictionary page names the API server the words are sent to", "[widget]") {
    PagesRig rig(Json{{"selected_model", "api"}, {"api_transcription_endpoint", "groq"}});
    fv::DictionaryPage& page = rig.dictionary();
    const QString line = page.steerTile()->descriptionLabel()->text();
    CHECK(line.contains(QStringLiteral("Groq")));
    CHECK(line.contains(QStringLiteral("sent to Groq")));
}

// settings_dictionary._mutate: logger.warning("Couldn't save the dictionary", exc_info=True).
TEST_CASE("a dictionary save that fails is logged as the dictionary's", "[widget]") {
    PagesRig rig;
    fv::DialogPersonalizeHost host(rig.dialog());
    LogCapture log;
    QString error = QStringLiteral("stale");
    CHECK_FALSE(host.mutate([](Json&) { throw std::runtime_error("the disk is full"); }, &error));
    CHECK(error.isEmpty());
    const std::string text = log.text();
    CHECK(text.find("ui_qt.dialogs.settings_dictionary: Couldn't save the dictionary") !=
          std::string::npos);
    CHECK(text.find("WARNING") != std::string::npos);
    // The failure's text may hold an entry: it is not logged.
    CHECK(text.find("the disk is full") == std::string::npos);
}
