#pragma once

#include <cstdint>

#include <QObject>
#include <QString>

namespace fv::export_detail {

// export_dialog_base._ExportRelay: brings export progress and results to the GUI thread for
// whichever dialogs exist then. It lives for the session (never deleted), so an export worker
// never holds or emits on a dialog: holding one let the worker end it off the GUI thread, and
// emitting on one raced its parent deleting it. Qt drops a deleted dialog's connections.
// Affinity: created on the GUI thread; emitted from any thread.
class ExportRelay : public QObject {
    Q_OBJECT
public:
    static ExportRelay& instance();
    // Export tokens, unique across dialogs because they share the relay.
    static std::uint64_t nextToken();

Q_SIGNALS:
    // Each carries the export's token, then the dialog signal's arguments.
    void progress(quint64 token, int done, int total, const QString& title);
    void finished(quint64 token, int count, const QString& output);
    void failed(quint64 token, const QString& message);
    void canceled(quint64 token);

private:
    ExportRelay() = default;
};

} // namespace fv::export_detail
