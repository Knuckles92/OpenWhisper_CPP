#include "ui_shell/single_instance.h"

#include <format>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

#include "appkit/qt_bridge.h"
#include "base/data_lease.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.ui_instance"};

// A request line is read until its newline, the peer closing, or more than this many bytes.
constexpr qsizetype kMaxRequestBytes = 64;

} // namespace

std::string instanceKey(const std::filesystem::path& dataRoot) {
    return sha256_hex(path_to_utf8(normalize_data_root(dataRoot))).substr(0, 16);
}

std::string instanceName(const std::filesystem::path& dataRoot, const AppIdentity& identity) {
    return std::format("{}-{}-{}", identity.instance_key_prefix, instanceUserPart(),
                       instanceKey(dataRoot));
}

SingleInstance::SingleInstance(std::unique_ptr<QLockFile> lock, QString serverName,
                               std::filesystem::path lockPath)
    : lock_(std::move(lock)), serverName_(std::move(serverName)), lockPath_(std::move(lockPath)) {}

SingleInstance::~SingleInstance() {
    close();
}

// services/ui_instance.py:139-197.
std::unique_ptr<SingleInstance> SingleInstance::acquire(const std::filesystem::path& dataRoot,
                                                        const AppIdentity& identity,
                                                        SingleInstanceOptions options) {
    const std::string name = instanceName(dataRoot, identity);
    std::filesystem::path folder = options.lockFolder;
    if (folder.empty()) {
        folder = path_from_utf8(qt::toUtf8(QDir::tempPath()));
    }
    std::filesystem::create_directories(folder);
    std::filesystem::path lockPath = folder / path_from_utf8(name + ".lock");
    std::error_code ignored;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(lockPath, ignored))) {
        throw std::runtime_error("The UI instance lock is a symbolic link.");
    }

    auto lock = std::make_unique<QLockFile>(qt::toQString(path_to_utf8(lockPath)));
    // Never older than its holder: only a holder that died leaves a lock the next launch may
    // take, so a live first copy that stops answering still keeps a second one out.
    lock->setStaleLockTime(0);
    if (lock->tryLock(0)) {
        return std::unique_ptr<SingleInstance>(
            new SingleInstance(std::move(lock), qt::toQString(name), std::move(lockPath)));
    }
    if (lock->error() != QLockFile::LockFailedError) {
        throw std::runtime_error(
            std::format("The UI instance lock {} could not be created.", path_to_utf8(lockPath)));
    }

    const QString serverName = qt::toQString(name);
    const auto deadline = std::chrono::steady_clock::now() + options.handoff;
    while (std::chrono::steady_clock::now() < deadline) {
        if (requestShow(serverName, options.connectTimeout)) {
            kLog.info("{} is already running; bringing the existing window forward",
                      identity.product);
            return nullptr;
        }
        std::this_thread::sleep_for(options.retryInterval);
    }
    kLog.warning("{} is already running but did not accept the show request", identity.product);
    return nullptr;
}

bool SingleInstance::requestShow(const QString& serverName, std::chrono::milliseconds timeout) {
    QLocalSocket socket;
    socket.connectToServer(serverName, QIODevice::ReadWrite);
    const int ms = static_cast<int>(timeout.count());
    if (!socket.waitForConnected(ms)) {
        return false;
    }
    // QLocalServer's Windows pipes have no buffer: the write completes only once the server's
    // event loop has read it, so a request to a server on the calling thread always times out.
    constexpr qint64 size = sizeof(kShowRequest) - 1;
    if (socket.write(kShowRequest, size) != size) {
        return false;
    }
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(ms)) {
            return false;
        }
    }
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(ms);
    }
    return true;
}

void SingleInstance::listen() {
    if (server_ != nullptr) {
        return;
    }
    server_ = new QLocalServer(this);
    // Only this user may ask for the window: the pipe or socket refuses everyone else.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(serverName_)) {
        // A socket file left by a crash (not on Windows, whose pipes go with their process).
        QLocalServer::removeServer(serverName_);
        if (!server_->listen(serverName_)) {
            const std::string error = qt::toUtf8(server_->errorString());
            delete server_;
            server_ = nullptr;
            throw std::runtime_error(std::format("Could not listen for show requests on {}: {}",
                                                 qt::toUtf8(serverName_), error));
        }
    }
    connect(server_, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
}

void SingleInstance::onNewConnection() {
    while (QLocalSocket* socket = server_ != nullptr ? server_->nextPendingConnection() : nullptr) {
        struct Request {
            QByteArray data;
            bool done = false;
        };
        auto request = std::make_shared<Request>();
        const auto finish = [this, socket, request] {
            if (request->done) {
                return;
            }
            request->done = true;
            if (request->data.startsWith("show")) {
                pending_ = true;
                invoke();
            }
            socket->disconnectFromServer();
            socket->deleteLater();
        };
        const auto read = [socket, request, finish] {
            if (request->done) {
                return;
            }
            request->data += socket->readAll();
            if (request->data.contains('\n') || request->data.size() > kMaxRequestBytes) {
                finish();
            }
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        connect(socket, &QLocalSocket::disconnected, this, [read, finish] {
            read();
            finish();
        });
        read();
    }
}

void SingleInstance::attachActivate(std::function<void()> callback) {
    onActivate_ = std::move(callback);
    if (pending_) {
        invoke();
    }
}

void SingleInstance::invoke() {
    if (onActivate_) {
        onActivate_();
    }
}

void SingleInstance::close() {
    if (server_ != nullptr) {
        server_->close();
        delete server_;
        server_ = nullptr;
    }
    if (lock_ != nullptr) {
        lock_->unlock();
        lock_.reset();
    }
}

} // namespace fv
