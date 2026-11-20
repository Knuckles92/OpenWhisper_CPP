#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "base/executor.h"

namespace fv {

// Writes the newest Scratchpad text on a strand, so a slow disk never stalls typing
// (ui_qt/widgets/scratchpad.py _Saver and _write_atomically). Every text gets a sequence number
// and an older one is never written over a newer one, whether it came from the strand or from
// writeNow. The file is replaced atomically (base/atomic_file.h): UTF-8, the text's own line
// ends (the editor's are LF).
//
// submit keeps one pending slot, so a burst of edits costs one write of the last text. A task
// still queued when the saver is destroyed finishes on its own state, never touching the saver.
// Affinity: thread-safe. writeNow blocks on the disk, as Python's does on hide and quit.
class ScratchpadSaver {
public:
    // Where the notes live, asked at every write.
    using PathFn = std::function<std::filesystem::path()>;

    // `strand` is where submitted texts are written (the persistence strand); it outlives the
    // tasks queued on it.
    ScratchpadSaver(Executor& strand, PathFn path);
    ~ScratchpadSaver();
    ScratchpadSaver(const ScratchpadSaver&) = delete;
    ScratchpadSaver& operator=(const ScratchpadSaver&) = delete;

    // Queues text for the strand, replacing a text still waiting there.
    void submit(std::string text);
    // Writes text now on this thread; a text still waiting is dropped, and one already being
    // written lands first and is superseded.
    void writeNow(std::string text);
    // Nothing is queued or being written by the strand.
    bool idle() const;
    // The strand's step: writes (sequence, text) unless a newer text was written. A failed
    // write is logged by exception type and leaves the file as it was. Public so a test can
    // replay a stale text, which only a race between the strand and writeNow produces.
    void write(std::uint64_t sequence, std::string text);

private:
    struct State;
    // Writes whatever is pending until nothing is left (the strand's task).
    static void drain(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    Executor& strand_;
};

} // namespace fv
