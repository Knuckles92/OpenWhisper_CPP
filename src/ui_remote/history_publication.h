#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/cancel.h"
#include "base/json.h"
#include "remote/control.h"

class QString;
class QWidget;

namespace fv {
class Executor;
namespace agentapi {
class HistoryStore;
}
namespace remote {
class RemoteConnection;
}

// One connection and one grant. Closing its dialog or losing the connection ends publication.
namespace remote_history {

Json publication_reply(agentapi::HistoryStore&, const Json& request);

class Publication : public std::enable_shared_from_this<Publication> {
public:
    using CurrentPeer = std::function<std::optional<remote::ClientPairing>()>;
    using Connect =
        std::function<std::shared_ptr<remote::RemoteConnection>(const remote::ClientPairing&)>;
    using Status = std::function<void(std::string, bool finished)>;

    Publication(Executor& worker, std::filesystem::path database, CurrentPeer, Connect);
    std::optional<std::uint64_t> start(std::string fingerprint, Status);
    void stop(std::optional<std::uint64_t> generation = {});

private:
    void run(std::string fingerprint, Status);

    Executor& worker_;
    std::filesystem::path database_;
    CurrentPeer current_;
    Connect connect_;
    std::mutex mutex_;
    std::shared_ptr<remote::RemoteConnection> connection_;
    CancelSource cancel_;
    std::uint64_t generation_ = 0;
    bool active_ = false;
};

void openSession(QWidget&, const QString& host, std::string fingerprint,
                 std::shared_ptr<Publication>, Executor& ui);

} // namespace remote_history
} // namespace fv
