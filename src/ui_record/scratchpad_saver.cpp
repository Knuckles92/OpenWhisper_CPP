#include "ui_record/scratchpad_saver.h"

#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

#include "base/atomic_file.h"
#include "base/log.h"
#include "ui_record/python_exception_name.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.scratchpad"};

} // namespace

struct ScratchpadSaver::State {
    explicit State(PathFn pathFn) : path(std::move(pathFn)) {}

    // Python's _write: nothing else touches the file while one write runs.
    void write(std::uint64_t number, const std::string& text) {
        std::string failure;
        {
            std::lock_guard writing(writeMutex);
            if (number <= written) {
                return;
            }
            try {
                const std::filesystem::path target = path();
                std::error_code ignored;
                if (target.has_parent_path()) {
                    std::filesystem::create_directories(target.parent_path(), ignored);
                }
                write_file_atomic(target, text);
                written = number;
                return;
            } catch (const std::exception& error) {
                // The type only, as Python's type(exc).__name__: a message may quote a path or
                // the notes.
                failure = pythonExceptionName(error);
            }
        }
        kLog.warning("Couldn't save the Scratchpad ({})", failure);
    }

    PathFn path;
    std::mutex mutex;
    std::mutex writeMutex;
    std::uint64_t sequence = 0;
    std::uint64_t written = 0;
    std::optional<std::pair<std::uint64_t, std::string>> pending;
    bool busy = false;
};

ScratchpadSaver::ScratchpadSaver(Executor& strand, PathFn path)
    : state_(std::make_shared<State>(std::move(path))), strand_(strand) {}

ScratchpadSaver::~ScratchpadSaver() = default;

void ScratchpadSaver::submit(std::string text) {
    {
        std::lock_guard lock(state_->mutex);
        state_->pending.emplace(++state_->sequence, std::move(text));
        if (state_->busy) {
            return;
        }
        state_->busy = true;
    }
    strand_.post([state = state_] { drain(state); });
}

void ScratchpadSaver::writeNow(std::string text) {
    std::uint64_t sequence = 0;
    {
        std::lock_guard lock(state_->mutex);
        sequence = ++state_->sequence;
        state_->pending.reset();
    }
    state_->write(sequence, text);
}

bool ScratchpadSaver::idle() const {
    std::lock_guard lock(state_->mutex);
    return !state_->busy;
}

void ScratchpadSaver::write(std::uint64_t sequence, std::string text) {
    state_->write(sequence, text);
}

void ScratchpadSaver::drain(const std::shared_ptr<State>& state) {
    for (;;) {
        std::optional<std::pair<std::uint64_t, std::string>> next;
        {
            std::lock_guard lock(state->mutex);
            next.swap(state->pending);
            if (!next) {
                state->busy = false;
                return;
            }
        }
        state->write(next->first, next->second);
    }
}

} // namespace fv
