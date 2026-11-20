#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace fv::remote_records {

class RecordsBusy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Holds off new requests while waiting for active transfers to drain for a backup or recovery.
class RecordGate {
public:
    class Request {
    public:
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        ~Request();

    private:
        friend class RecordGate;
        explicit Request(RecordGate& gate) : gate_(gate) {}
        RecordGate& gate_;
    };

    class Pause {
    public:
        Pause(Pause&& other) noexcept;
        Pause(const Pause&) = delete;
        Pause& operator=(const Pause&) = delete;
        ~Pause();

    private:
        friend class RecordGate;
        explicit Pause(RecordGate& gate) : gate_(&gate) {}
        RecordGate* gate_;
    };

    Request request();
    Pause paused(std::chrono::milliseconds timeout = std::chrono::seconds(30));

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int active_ = 0;
    bool paused_ = false;
};

} // namespace fv::remote_records
