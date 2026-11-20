#include "ui_record/upload_presenter.h"

#include <exception>
#include <utility>

#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/log.h"
#include "base/utf8.h"
#include "dictation/transcription_runtime.h"
#include "dictation/upload_progress.h"
#include "focus/clipboard_port.h"
#include "ui_record/upload_file_tab.h"
#include "ui_shell/main_window.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

} // namespace

UploadPresenter::UploadPresenter(ScreenRegistry& screens, Ports ports, QObject* parent)
    : QObject(parent), screens_(screens), ports_(std::move(ports)) {
    connect(&screens_, &ScreenRegistry::screenCreated, this,
            [this](const QString& key, QWidget* screen) {
                if (key == QLatin1String(screen_key::upload)) {
                    attach(qobject_cast<UploadFileTab*>(screen));
                }
            });
    attach(tab());
}

UploadPresenter::~UploadPresenter() = default;

UploadPresenter::Ports UploadPresenter::runtimePorts(dictation::TranscriptionRuntime& runtime) {
    Ports ports;
    ports.uploadFile = [&runtime](const std::filesystem::path& path,
                                  std::optional<double> seconds) {
        runtime.upload_audio_file(path, seconds);
    };
    ports.uploadFiles = [&runtime](const dictation::BatchUploadRequest& request) {
        runtime.upload_audio_files(request);
    };
    ports.cancel = [&runtime] {
        runtime.cancel();
    };
    return ports;
}

UploadFileTab* UploadPresenter::tab() const {
    if (!screens_.isDefined(screen_key::upload)) {
        return nullptr;
    }
    return qobject_cast<UploadFileTab*>(screens_.existing(screen_key::upload));
}

UploadFileTab* UploadPresenter::ensureTab() {
    if (UploadFileTab* existing = tab()) {
        return existing;
    }
    if (!ports_.tabParent || !screens_.isDefined(screen_key::upload)) {
        return nullptr;
    }
    try {
        // Attached through screenCreated.
        return qobject_cast<UploadFileTab*>(
            screens_.ensure(screen_key::upload, ports_.tabParent()));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not open the Upload File tab");
        return nullptr;
    }
}

void UploadPresenter::attach(UploadFileTab* tab) {
    if (tab == nullptr || tab == attached_) {
        return;
    }
    attached_ = tab;
    connect(tab, &UploadFileTab::uploadRequested, this, &UploadPresenter::requestUpload);
    connect(tab, &UploadFileTab::uploadFilesRequested, this, &UploadPresenter::requestUploads);
    connect(tab, &UploadFileTab::cancelRequested, this, &UploadPresenter::cancelUpload);
    connect(tab, &UploadFileTab::copyRequested, this, &UploadPresenter::copy);
}

bool UploadPresenter::isUploadJobActive() const {
    const UploadFileTab* upload = tab();
    return uploadSource_ && upload != nullptr && upload->isTranscribing();
}

bool UploadPresenter::routeProgress(appcore::OverlayState state) {
    if (!isUploadJobActive()) {
        return false;
    }
    UploadFileTab* upload = tab();
    if (state == appcore::OverlayState::None) {
        upload->setProgressState(state);
        return false;
    }
    if (!dictation::stage_for_overlay_state(state)) {
        return false;
    }
    upload->setProgressState(state);
    return true;
}

bool UploadPresenter::showLargeFileState(double fileSizeMb) {
    if (!isUploadJobActive()) {
        return false;
    }
    tab()->setLargeFileStage(fileSizeMb);
    return true;
}

void UploadPresenter::setBatchProgress(int position, int total, const QString& sourceName) {
    if (isUploadJobActive()) {
        tab()->setBatchProgress(position, total, sourceName);
    }
}

void UploadPresenter::setBatchItemFinished(int position, bool success, const QString& transcript) {
    if (isUploadJobActive()) {
        tab()->setBatchItemFinished(position, success, transcript);
    }
}

void UploadPresenter::setBatchResult(const dictation::BatchResult& result) {
    if (uploadSource_) {
        if (UploadFileTab* upload = ensureTab()) {
            upload->setBatchResult(result);
        }
    }
}

bool UploadPresenter::displayTranscript(const QString& text, const std::optional<QString>& raw) {
    UploadFileTab* upload = uploadSource_ ? ensureTab() : nullptr;
    if (upload == nullptr) {
        return false;
    }
    upload->setTranscript(text, raw);
    upload->expandTranscription();
    return true;
}

bool UploadPresenter::displayTranscriptionError(const QString& message) {
    UploadFileTab* upload = uploadSource_ ? ensureTab() : nullptr;
    if (upload == nullptr) {
        return false;
    }
    upload->showJobError(message);
    return true;
}

bool UploadPresenter::setTranscriptionStats(const dictation::TranscriptionStats& stats) {
    UploadFileTab* upload = uploadSource_ ? ensureTab() : nullptr;
    if (upload == nullptr) {
        return false;
    }
    upload->setTranscriptionStats(stats.transcription_time, stats.audio_duration,
                                  stats.file_size > 0 ? std::optional<std::int64_t>(stats.file_size)
                                                      : std::nullopt,
                                  stats.cleanup_time.value_or(0.0), stats.remote);
    return true;
}

void UploadPresenter::applyStatus(const QString& status) {
    if (ports_.quickRecordLoading && ports_.quickRecordLoading()) {
        if (ports_.windowStatus) {
            ports_.windowStatus(status);
        }
        // Kept for the tab when it is built later, as its engine readout.
        screens_.update(screen_key::upload, "set_engine_message", [status](QWidget* screen) {
            if (auto* upload = qobject_cast<UploadFileTab*>(screen)) {
                upload->setEngineMessage(status);
            }
        });
        return;
    }
    if (uploadSource_) {
        if (UploadFileTab* upload = ensureTab()) {
            upload->setStatus(status);
        }
        return;
    }
    if (ports_.windowStatus) {
        ports_.windowStatus(status);
    }
}

void UploadPresenter::requestUpload(const QString& path, double durationSeconds) {
    uploadSource_ = true;
    kLog.info("Upload tab transcription started: {}", qt::toUtf8(path));
    if (ports_.uploadFile) {
        ports_.uploadFile(base_utf8::path_from_utf8(qt::toUtf8(path)), durationSeconds);
    }
}

void UploadPresenter::requestUploads(const dictation::BatchUploadRequest& request) {
    uploadSource_ = true;
    kLog.info("Upload tab batch transcription started: {} files", request.items.size());
    if (ports_.uploadFiles) {
        ports_.uploadFiles(request);
    }
}

void UploadPresenter::cancelUpload() {
    if (!isUploadJobActive()) {
        return;
    }
    if (ports_.cancel) {
        ports_.cancel();
    }
}

void UploadPresenter::copy(const QString& text) {
    UploadFileTab* upload = tab();
    const bool copied =
        ports_.clipboard != nullptr && ports_.clipboard->write_text(qt::toUtf8(text));
    if (copied) {
        if (upload != nullptr) {
            upload->setStatus(QStringLiteral("Copied to clipboard"));
        }
        Q_EMIT copiedAnimationRequested();
    } else if (upload != nullptr) {
        upload->setStatus(QStringLiteral("Copy failed"));
    }
}

} // namespace fv
