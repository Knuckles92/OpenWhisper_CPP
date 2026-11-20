#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "process/launcher.h"
#include "process/win/wide.h"

// A child's pipes, each served on a thread of its own until the run is done with it
// (programs.py _Pipe, _Input, _Output). A process the child leaves running may hold the other end
// for as long as it lives, so a pipe's end can't be waited for: finish() stops the thread by
// cancelling the read or write it is blocked in (CancelSynchronousIo), and the thread then closes
// its end (closing a pipe another thread is blocked on would block too).
namespace fv::process_win {

class PipeThread {
public:
    virtual ~PipeThread();
    // Waits until the thread is done or `deadline` passes, then cancels its I/O until it leaves.
    void finish(std::chrono::steady_clock::time_point deadline);
    bool done() const;

protected:
    struct State;
    PipeThread();
    std::shared_ptr<State> state_;
};

// Reads `pipe` (owned) to its end, handing every chunk to `sink` and then an empty span.
class PipeReader final : public PipeThread {
public:
    PipeReader(HANDLE pipe, std::wstring name, ByteSink sink);
};

// Writes what write() queues to `pipe` (owned), and closes it after close().
class PipeWriter final : public PipeThread {
public:
    PipeWriter(HANDLE pipe, std::wstring name);
    void write(std::string_view data);
    void close();
};

} // namespace fv::process_win
