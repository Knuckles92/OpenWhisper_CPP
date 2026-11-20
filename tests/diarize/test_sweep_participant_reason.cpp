// A rejected participant keeps the store's reason: clustering.py _create_participant raises
// RuntimeError(f"Participant creation rejected: {reason}").
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/log.h"
#include "diarize/clustering.h"
#include "diarize_support.h"

namespace {

class Failures final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        if (record.exception != nullptr) {
            const std::lock_guard lock(mutex_);
            reasons_.emplace_back(record.exception->what());
        }
    }
    std::vector<std::string> reasons() const {
        const std::lock_guard lock(mutex_);
        return reasons_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> reasons_;
};

} // namespace

TEST_CASE("a rejected speaker names the store's reason", "[clustering]") {
    fv::diarize::test::Rig rig(
        fv::diarize::test::optional_all(fv::diarize::test::synthetic_speakers(1, 1)));
    rig.host.reject_participants(true, "human_edited");
    const auto failures = std::make_shared<Failures>();
    fv::log::add_sink(failures);
    const auto pid = rig.assign("sg_0", 0.0);
    fv::log::remove_sink(failures);

    CHECK_FALSE(pid.has_value());
    CHECK_FALSE(rig.diarizer->is_available());
    CHECK(failures->reasons() ==
          std::vector<std::string>{"Participant creation rejected: human_edited"});
}
