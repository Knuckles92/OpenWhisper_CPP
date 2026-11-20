#include "process/win/pipes.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "base/log.h"

namespace fv::process_win {

namespace {

constexpr log::Logger logger("services.automation.programs");
// What one read or write moves at most (programs.py _Pipe.CHUNK).
constexpr std::size_t kChunk = 65536;
// How long finish() keeps cancelling a thread that doesn't leave before letting it go. A cancel
// that lands between two calls is lost; the next one isn't.
constexpr auto kCancelLimit = std::chrono::seconds(10);
constexpr auto kCancelEvery = std::chrono::milliseconds(10);

} // namespace

struct PipeThread::State {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool stop = false;
    // The writer's queue.
    std::string pending;
    bool close_requested = false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread thread;

    void finished() {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        changed.notify_all();
    }
    bool stopping() {
        std::lock_guard lock(mutex);
        return stop;
    }
};

PipeThread::PipeThread() : state_(std::make_shared<State>()) {}

PipeThread::~PipeThread() {
    if (state_->thread.joinable()) {
        finish(std::chrono::steady_clock::now());
    }
}

bool PipeThread::done() const {
    std::lock_guard lock(state_->mutex);
    return state_->done;
}

void PipeThread::finish(std::chrono::steady_clock::time_point deadline) {
    auto& state = *state_;
    if (!state.thread.joinable()) {
        return;
    }
    {
        std::unique_lock lock(state.mutex);
        state.changed.wait_until(lock, deadline, [&] { return state.done; });
        state.stop = true;
    }
    state.changed.notify_all();
    const auto give_up = std::chrono::steady_clock::now() + kCancelLimit;
    for (;;) {
        {
            std::unique_lock lock(state.mutex);
            if (state.done) {
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= give_up) {
            logger.warning("A program's pipe didn't stop; leaving its thread behind");
            state.thread.detach();
            return;
        }
        CancelSynchronousIo(state.thread.native_handle());
        std::unique_lock lock(state.mutex);
        state.changed.wait_for(lock, kCancelEvery, [&] { return state.done; });
    }
    state.thread.join();
}

PipeReader::PipeReader(HANDLE pipe, std::wstring name, ByteSink sink) {
    state_->pipe = pipe;
    state_->thread = std::thread([state = state_, name = std::move(name), sink = std::move(sink)] {
        SetThreadDescription(GetCurrentThread(), name.c_str());
        std::vector<char> buffer(kChunk);
        const auto deliver = [&](std::span<const char> bytes) {
            if (!sink) {
                return;
            }
            try {
                sink(bytes);
            } catch (const std::exception& error) {
                logger.error_with(error, "A pipe's reader failed");
            }
        };
        while (!state->stopping()) {
            DWORD got = 0;
            if (!ReadFile(state->pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                          nullptr)) {
                break; // the end of the pipe, or cancelled
            }
            if (got > 0) {
                deliver(std::span<const char>(buffer.data(), got));
            }
        }
        CloseHandle(state->pipe);
        deliver({});
        state->finished();
    });
}

PipeWriter::PipeWriter(HANDLE pipe, std::wstring name) {
    state_->pipe = pipe;
    state_->thread = std::thread([state = state_, name = std::move(name)] {
        SetThreadDescription(GetCurrentThread(), name.c_str());
        for (;;) {
            std::string chunk;
            {
                std::unique_lock lock(state->mutex);
                state->changed.wait(lock, [&] {
                    return state->stop || !state->pending.empty() || state->close_requested;
                });
                if (state->stop || state->pending.empty()) {
                    break;
                }
                chunk = state->pending.substr(0, std::min(kChunk, state->pending.size()));
            }
            DWORD written = 0;
            if (!WriteFile(state->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &written,
                           nullptr)) {
                break; // nobody reads any more, or cancelled
            }
            std::lock_guard lock(state->mutex);
            state->pending.erase(0, written);
        }
        CloseHandle(state->pipe);
        {
            std::lock_guard lock(state->mutex);
            state->pending.clear();
        }
        state->finished();
    });
}

void PipeWriter::write(std::string_view data) {
    if (data.empty()) {
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stop || state_->done || state_->close_requested) {
            return;
        }
        state_->pending.append(data);
    }
    state_->changed.notify_all();
}

void PipeWriter::close() {
    {
        std::lock_guard lock(state_->mutex);
        state_->close_requested = true;
    }
    state_->changed.notify_all();
}

} // namespace fv::process_win
