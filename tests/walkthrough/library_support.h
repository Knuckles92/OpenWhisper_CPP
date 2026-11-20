#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/json.h"
#include "support.h"
#include "walkthrough/finish.h"
#include "walkthrough/renderer.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_renderer.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace fv::walkthrough::test {

// Installs a renderer for the test's lifetime, and removes it again.
class ScopedRenderer {
public:
    explicit ScopedRenderer(std::shared_ptr<DocumentRenderer> renderer) {
        set_document_renderer(std::move(renderer));
    }
    ~ScopedRenderer() { set_document_renderer(nullptr); }
    ScopedRenderer(const ScopedRenderer&) = delete;
    ScopedRenderer& operator=(const ScopedRenderer&) = delete;
};

inline std::chrono::local_seconds capture_start() {
    using namespace std::chrono_literals;
    return std::chrono::local_days{std::chrono::year{2026} / 10 / 21} + 14h + 32min + 5s;
}

inline ShotImagePtr red_image(int width = 120, int height = 80) {
    return std::make_shared<FakeShotImage>(width, height, 0xf84f30);
}

// OpenWhisper's `capture` fixture: captures/2026-10-21_14-32-05 with two screenshots kept at 1 s
// and 2 s and the words "first", "second", "last", built by build_documents through a
// FakeRenderer that stays installed while the Capture lives.
struct Capture {
    TempFolder tmp;
    std::shared_ptr<FakeRenderer> renderer = std::make_shared<FakeRenderer>();
    ScopedRenderer installed{renderer};
    std::filesystem::path folder;
    std::filesystem::path root;
    WalkthroughResult result;

    explicit Capture(std::vector<std::string> texts = {"first", "second", "last"},
                     std::vector<Note> notes = {}) {
        folder = tmp / "captures" / "2026-10-21_14-32-05";
        root = folder.parent_path();
        std::filesystem::create_directories(folder);
        WalkthroughSession::Options options;
        options.started_at = capture_start();
        options.folder = folder;
        WalkthroughSession session(std::move(options));
        session.add_shot(red_image(), 1.0);
        session.add_shot(red_image(), 2.0);
        const auto shots = session.saved_shots();
        result = build_documents(session, shots, texts, texts, 3.0, notes, Json::object());
        session.end();
    }
};

// noted_capture: two screenshots, a note held before the second and one after the last, said
// alone.
inline std::unique_ptr<Capture> noted_capture() {
    return std::make_unique<Capture>(
        std::vector<std::string>{"first", "second", ""},
        std::vector<Note>{{"make it purple", 1}, {"sign off warmly", 2}});
}

// Every file directly in `folder` and its bytes.
inline std::map<std::string, std::string> snapshot(const std::filesystem::path& folder) {
    std::map<std::string, std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        files[path_to_utf8(entry.path().filename())] =
            entry.is_regular_file() ? read_file(entry.path()) : std::string("<folder>");
    }
    return files;
}

// _audio: a mono 16-bit 16 kHz WAV of 48,000 silent frames.
inline void write_audio(const std::filesystem::path& path) {
    write_wav(path, std::vector<std::int16_t>(48'000, 0));
}

} // namespace fv::walkthrough::test
