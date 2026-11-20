#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "audio/testing/fake_backend.h"
#include "testkit/testkit.h"

namespace fv::audio::test {

inline std::filesystem::path fixture(const std::string& name) {
    return testkit::source_root() / "tests" / "fixtures" / "audio" / name;
}

// tests/test_personalize_s7_audio_devices.py windows_sd(): a measured Windows layout.
inline std::shared_ptr<testing::FakeBackend> windows_backend() {
    return testing::FakeBackend::from_fixture(fixture("devices-windows.json"));
}

} // namespace fv::audio::test
