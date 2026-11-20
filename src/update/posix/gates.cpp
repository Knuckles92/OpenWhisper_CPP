#include "update/gates.h"

#include <cstdio>
#include <string>
#include <thread>

namespace fv::update {

namespace {

// No Inno setup runs here: every mutex can be "created" and none exists.
class PosixMutexApi final : public MutexApi {
public:
    std::optional<MutexHandle> create(std::string_view) override { return MutexHandle{1}; }
    bool exists(std::string_view) override { return false; }
    void release(MutexHandle) override {}
    void wait(std::chrono::milliseconds delay) override { std::this_thread::sleep_for(delay); }
};

} // namespace

MutexApi& system_mutex_api() {
    static PosixMutexApi api;
    return api;
}

void native_message_box(std::string_view message) {
    const std::string line = std::string(message) + "\n";
    std::fputs(line.c_str(), stderr);
}

} // namespace fv::update
