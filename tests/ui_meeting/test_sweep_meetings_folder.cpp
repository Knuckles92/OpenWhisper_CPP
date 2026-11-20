// openMeetingsFolder when the folder cannot be made: OpenWhisper's os.makedirs raises out of
// _on_open_meetings_folder, so the folder is not shown; OpenWhisper_CPP logs the reason and returns
// false.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/log.h"
#include "past_meetings_support.h"
#include "testkit/testkit.h"
#include "ui_meeting/past_meetings_panel.h"

namespace {

class Errors final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        if (record.level >= fv::log::Level::Error) {
            const std::lock_guard lock(mutex_);
            messages_.emplace_back(record.message);
        }
    }
    std::vector<std::string> messages() const {
        const std::lock_guard lock(mutex_);
        return messages_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

} // namespace

TEST_CASE("a meetings folder that cannot be made is not opened", "[past_meetings_panel]") {
    fv::testkit::TempDataRoot root;
    // A file where the folder's parent should be: no platform can make a folder under it.
    const std::filesystem::path blocker = root.path() / "blocker";
    std::ofstream(blocker) << "not a folder";
    const std::filesystem::path folder = blocker / "meetings";
    fvtest::RecordedLaunches launches;
    const auto errors = std::make_shared<Errors>();
    fv::log::add_sink(errors);

    const bool opened = fv::openMeetingsFolder(folder);
    fv::log::remove_sink(errors);

    CHECK_FALSE(opened);
    CHECK(launches.urls.empty());
    CHECK_FALSE(std::filesystem::exists(folder));
    REQUIRE(errors->messages().size() == 1);
    CHECK(errors->messages()[0].starts_with("Could not create the meetings folder "));
}
