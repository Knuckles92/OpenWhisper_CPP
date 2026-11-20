#pragma once

#include <memory>

#include <QEvent>
#include <QMetaObject>
#include <QMimeData>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include "audio/file_preview.h"

// Fixtures the upload widget tests share (tests/test_upload_file_tab.py's _preview and
// _drop_event).
namespace fvtest {

// Sends an event to a widget the way QApplication would. QWidget makes event()
// protected, so it is called through QObject.
inline void deliver(QWidget& widget, QEvent* event) {
    static_cast<QObject&>(widget).event(event);
}

// Runs a timer's timeout now (QTimer::timeout cannot be emitted from outside the class).
inline void expire(QTimer* timer) {
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
}

// _preview: sample.wav, 1 MiB, one minute, 44.1 kHz stereo, one pass.
inline fv::audio::AudioFilePreview samplePreview() {
    fv::audio::AudioFilePreview preview;
    preview.file_path = "sample.wav";
    preview.file_name = "sample.wav";
    preview.file_size_mb = 1.0;
    preview.duration_seconds = 60.0;
    preview.sample_rate = 44100;
    preview.channels = 2;
    preview.needs_splitting = false;
    preview.estimated_chunks = 1;
    return preview;
}

// A drag's payload: the given local paths as file URLs, then the given URLs as they are. The
// event does not own its mime data, so keep this alive while the event is handled.
inline std::unique_ptr<QMimeData> mimeOfPaths(const QStringList& paths,
                                              const QList<QUrl>& extraUrls = {}) {
    auto mime = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    for (const QString& path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    urls.append(extraUrls);
    mime->setUrls(urls);
    return mime;
}

} // namespace fvtest
