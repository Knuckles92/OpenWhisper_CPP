#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QString>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "walkthrough/finish.h"
#include "walkthrough/renderer.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_renderer.h"
#include "walkthrough/testing/fake_shot_image.h"
#include "walkthrough/write_up.h"

// What the storyboard's tests share: OpenWhisper's `capture` and `noted_capture` fixtures
// (tests/test_walkthrough_library.py), built through a FakeRenderer; an in-memory settings file;
// a host whose copy_files is a Mock that records its calls and whose submit queues work on a
// ManualExecutor; a stand-in agent for write-ups.
namespace fvtest {

namespace wt = fv::walkthrough;

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void writeFile(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Installs a renderer for the fixture's lifetime, and removes it again.
class ScopedRenderer {
public:
    explicit ScopedRenderer(std::shared_ptr<wt::DocumentRenderer> renderer) {
        wt::set_document_renderer(std::move(renderer));
    }
    ~ScopedRenderer() { wt::set_document_renderer(nullptr); }
    ScopedRenderer(const ScopedRenderer&) = delete;
    ScopedRenderer& operator=(const ScopedRenderer&) = delete;
};

// captures/2026-10-21_14-32-05 with two 120 x 80 screenshots kept at 1 s and 2 s and the words
// "first", "second", "last" (or `texts` and `notes`), built by build_documents.
struct Capture {
    fv::testkit::TempDataRoot tmp;
    std::shared_ptr<wt::FakeRenderer> renderer = std::make_shared<wt::FakeRenderer>();
    ScopedRenderer installed{renderer};
    std::filesystem::path folder;
    std::filesystem::path root;
    wt::WalkthroughResult result;

    explicit Capture(std::vector<std::string> texts = {"first", "second", "last"},
                     std::vector<wt::Note> notes = {}) {
        using namespace std::chrono_literals;
        folder = tmp.path() / "captures" / "2026-10-21_14-32-05";
        root = folder.parent_path();
        std::filesystem::create_directories(folder);
        wt::WalkthroughSession::Options options;
        options.started_at =
            std::chrono::local_days{std::chrono::year{2026} / 10 / 21} + 14h + 32min + 5s;
        options.folder = folder;
        wt::WalkthroughSession session(std::move(options));
        session.add_shot(std::make_shared<wt::FakeShotImage>(120, 80, 0xf84f30), 1.0);
        session.add_shot(std::make_shared<wt::FakeShotImage>(120, 80, 0xf84f30), 2.0);
        const auto shots = session.saved_shots();
        result = wt::build_documents(session, shots, texts, texts, 3.0, notes, fv::Json::object());
        session.end();
    }
};

// noted_capture: two screenshots, a note held before the second and one after the last, said
// alone.
inline std::unique_ptr<Capture> notedCapture() {
    return std::make_unique<Capture>(
        std::vector<std::string>{"first", "second", ""},
        std::vector<wt::Note>{{"make it purple", 1}, {"sign off warmly", 2}});
}

// One call of the host's copy_files.
struct CopyCall {
    std::vector<std::filesystem::path> files;
    QString text;
    QImage image;
};

// The host's callbacks as OpenWhisper's tests mock them.
struct Host {
    std::vector<CopyCall> copies;
    bool copyWorks = true;
    int acquires = 0;
    int releases = 0;
    std::optional<QString> refusal;
    std::vector<QString> recovered;
    fv::ManualExecutor executor;

    fv::ReviewHost callbacks(bool withCopy = true, bool withRecover = false) {
        fv::ReviewHost host;
        if (withCopy) {
            host.copyFiles = [this](const std::vector<std::filesystem::path>& files,
                                    const QString& text, const QImage& image) {
                copies.push_back({files, text, image});
                return copyWorks;
            };
        }
        if (withRecover) {
            host.recover = [this](const QString& folder) {
                recovered.push_back(folder);
            };
        }
        host.acquire = [this]() -> std::optional<QString> {
            ++acquires;
            return refusal;
        };
        host.release = [this] {
            ++releases;
        };
        host.submit = [this](fv::Task task) {
            executor.post(std::move(task));
            return true;
        };
        return host;
    }
};

// A stand-in agent: answers every write-up with `reply` and records the requests, or fails
// with `error` when it is set.
struct Agent {
    std::string reply;
    std::string error;
    std::vector<wt::write_up::AgentTaskRequest> calls;

    wt::write_up::AgentTaskRunner runner() {
        return [this](const wt::write_up::AgentTaskRequest& request) {
            calls.push_back(request);
            fv::agents::PassOutcome outcome;
            outcome.ok = error.empty();
            outcome.text = reply;
            outcome.error = error;
            return outcome;
        };
    }
};

// A storyboard bound to `host`, its questions answered by `answer` and its dialogs never shown;
// changed() shows the folder it names, as OpenWhisper's review fixture connects it.
struct Review {
    std::unique_ptr<fv::SettingsStore> settings =
        std::make_unique<fv::SettingsStore>(std::make_unique<fv::MemorySettingsFile>());
    Host host;
    Agent agent;
    QMessageBox::StandardButton answer = QMessageBox::Yes;
    int questions = 0;
    std::unique_ptr<fv::WalkthroughReview> widget;

    explicit Review(bool withCopy = true, bool withRecover = false, bool closable = false) {
        widget = std::make_unique<fv::WalkthroughReview>(
            *settings, host.callbacks(withCopy, withRecover), closable);
        widget->setWriteUpRunner(agent.runner());
        widget->setOpener([this](const QUrl& url) {
            opened.push_back(url);
            return true;
        });
        widget->setQuestion([this](QWidget*, const QString&, const QString&,
                                   QMessageBox::StandardButtons, QMessageBox::StandardButton) {
            ++questions;
            return answer;
        });
        widget->setDialogRunner([](QDialog&) { return 0; });
        QObject::connect(widget.get(), &fv::WalkthroughReview::changed, widget.get(),
                         [this](const QString& folder) {
                             if (folder.isEmpty()) {
                                 widget->showFolder(std::nullopt);
                             } else {
                                 widget->showFolder(fv::path_from_utf8(folder.toStdString()));
                             }
                         });
    }
    ~Review() {
        // Nothing asks about the edits a test leaves behind.
        widget->setDirty(false);
        widget->close();
        widget.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    Review(const Review&) = delete;
    Review& operator=(const Review&) = delete;

    fv::WalkthroughReview* operator->() const { return widget.get(); }
    fv::WalkthroughReview& operator*() const { return *widget; }

    // Runs the queued work (OpenWhisper's _wait); the review is idle after it.
    void wait() { host.executor.run_all(); }
    // wait(), then the work must have succeeded (test_walkthrough_storyboard.py's _wait).
    void waitOk() {
        wait();
        const QString notice = widget->notice()->text();
        INFO(notice.toStdString());
        CHECK_FALSE(notice.startsWith(QStringLiteral("Couldn't finish:")));
    }

    std::vector<QString> texts() const {
        std::vector<QString> out;
        for (auto* row : widget->rows()) {
            out.push_back(row->editor()->toPlainText());
        }
        return out;
    }

    std::vector<QUrl> opened;
};

// Python's str(Path): the platform's separators.
inline std::filesystem::path preferred(std::filesystem::path path) {
    path.make_preferred();
    return path;
}

} // namespace fvtest
