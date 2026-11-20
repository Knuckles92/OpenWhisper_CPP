#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <QObject>
#include <QString>

class QLocalServer;
class QLockFile;

namespace fv {

struct AppIdentity;

// services/ui_instance._data_key: the first 16 hex digits of the SHA-256 of the data root as
// normcase(realpath(abspath(root))) spells it, UTF-8. Two spellings of one root share a key;
// different roots get separate instances.
std::string instanceKey(const std::filesystem::path& dataRoot);

// The account's part of the lock and server names (services/ui_instance.runtime_paths puts
// os.getuid() in both): the uid on POSIX, a short hash of the account name on Windows. Two
// accounts sharing a data folder get separate instances, as OpenWhisper's did.
std::string instanceUserPart();

// "<prefix>-<user part>-<key>": the lock file's stem and the local server's name.
std::string instanceName(const std::filesystem::path& dataRoot, const AppIdentity& identity);

struct SingleInstanceOptions {
    // Where the lock file goes; empty is the user's temporary folder.
    std::filesystem::path lockFolder;
    // How long a second launch keeps asking the first to show its window.
    std::chrono::milliseconds handoff{2000};
    // Each attempt's connect and write timeout, and the pause between attempts.
    std::chrono::milliseconds connectTimeout{400};
    std::chrono::milliseconds retryInterval{50};
};

// One desktop UI per data folder, on every platform (OpenWhisper did this on Linux only;
// architecture 12.3). The first launch holds an exclusive lock file and, once Qt's application
// object exists, a local server; a later launch for the same folder finds the lock held, sends
// "show\n" to the server so the first window comes forward, and exits. A lock whose holder is
// alive is never taken over, even when its holder does not answer; a holder that died leaves a
// stale lock, which the next launch takes.
// Affinity: GUI thread.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    // The newline-ended request a later launch writes.
    static constexpr char kShowRequest[] = "show\n";

    ~SingleInstance() override;

    // Takes the lock and returns the primary instance; or, when another process (or another
    // instance in this one) holds it, asks it to show its window for up to options.handoff and
    // returns null (the requests block, as requestShow's do). Needs no QApplication and creates
    // none. Throws std::runtime_error when the
    // lock file is a symbolic link or cannot be created.
    static std::unique_ptr<SingleInstance> acquire(const std::filesystem::path& dataRoot,
                                                   const AppIdentity& identity,
                                                   SingleInstanceOptions options = {});

    // Starts answering show requests. Call once a QCoreApplication exists (the server runs on
    // its event loop); a later launch keeps retrying until then. Throws std::runtime_error when
    // the server cannot listen.
    void listen();
    // Calls callback, on this object's thread, when a later launch asks to show the window. A
    // request that came before is replayed now.
    void attachActivate(std::function<void()> callback);
    // Stops the server and releases the lock (also on destruction).
    void close();

    QString serverName() const { return serverName_; }
    const std::filesystem::path& lockPath() const { return lockPath_; }
    bool showRequested() const { return pending_; }

    // services/ui_instance.request_show: writes the show request to serverName; true when it
    // was sent. Blocks until the server has read it or the timeout passes, so it never reaches
    // a server on the calling thread.
    static bool requestShow(const QString& serverName,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(400));

private:
    SingleInstance(std::unique_ptr<QLockFile> lock, QString serverName,
                   std::filesystem::path lockPath);
    void onNewConnection();
    void invoke();

    std::unique_ptr<QLockFile> lock_;
    QLocalServer* server_ = nullptr;
    QString serverName_;
    std::filesystem::path lockPath_;
    std::function<void()> onActivate_;
    bool pending_ = false;
};

} // namespace fv
