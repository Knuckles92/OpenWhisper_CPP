#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/signal.h"
#include "walkthrough/shot_image.h"

// One walkthrough while it records (services/walkthrough/session.py): the screenshots kept so
// far, where they live, and the board the live column shows. Each kept screenshot closes a
// section, the words said since the previous one; moving a screenshot moves its words with it,
// removing one leaves its words to the section after it. The recording only ever runs forward,
// so the board says which pieces of it each section reads. Stretches held with Hold for note are
// for the AI write-up only.
namespace fv::walkthrough {

// How long the worker waits for each screenshot still being written (SAVE_WAIT_S).
inline constexpr std::chrono::milliseconds SAVE_WAIT{30'000};

struct Shot {
    int number = 0; // 1 for the first screenshot kept, and so on
    std::filesystem::path path;
    double kept_at_s = 0; // seconds into the recording when it was kept

    friend bool operator==(const Shot&, const Shot&) = default;
};

// Words the user held for the agent: they go to the write-up, never the document.
struct Note {
    std::string text;
    // Which screenshot they were said before, counting from 0, or the number of screenshots
    // when they came after the last one.
    int section = 0;

    friend bool operator==(const Note&, const Note&) = default;
};

// How a stopped walkthrough reads: its screenshots in order, and whose words are whose. The
// recording is cut at cuts_s into cuts_s.size() + 1 pieces, in time order; each section reads
// some of them, wherever the board moved them.
struct Storyboard {
    // The screenshots whose file was written, in the board's order, numbered from 1.
    std::vector<Shot> shots;
    // Where the recording is cut, in seconds, in time order.
    std::vector<double> cuts_s;
    // For each section, one per screenshot and then the words after the last: the pieces it
    // reads, in order.
    std::vector<std::vector<int>> sections{{0}};
    // The stretches held for a note, (start, end) in seconds, in time order; no piece holds
    // them. A note held to the end ends at +inf.
    std::vector<std::pair<double, double>> notes_s;

    // The section that reads the moment at_s of the recording. Throws std::out_of_range when no
    // section reads it (Python's StopIteration).
    int section_at(double at_s) const;

    friend bool operator==(const Storyboard&, const Storyboard&) = default;
};

// A finished walkthrough's document, as delivered and saved.
struct WalkthroughResult {
    std::filesystem::path folder;
    std::filesystem::path pdf;
    std::filesystem::path markdown;
    // The Markdown with absolute image paths: what a terminal agent gets.
    std::string clipboard_text;
    // The words with a marker where each screenshot sits, for History.
    std::string history_text;
    // The same before AI cleanup; nullopt when cleanup changed nothing.
    std::optional<std::string> history_raw;
    int shots = 0;
    double duration_s = 0;
    // The first screenshot, for the card that says it's ready.
    std::optional<std::filesystem::path> first_shot;
    // Whether pdf and markdown are an agent's write-up rather than the words as spoken.
    bool written_up = false;
    // Why a write-up that was asked for isn't there, or "".
    std::string write_up_error;
    // Screenshots kept during the recording whose file could not be written.
    int lost_shots = 0;

    // What the status and the card call the document: "Write-up" or "Walkthrough".
    std::string name() const;
    // The same document for Word, beside pdf.
    std::filesystem::path docx() const;
    // The files and the text a copy in `format` (a walkthrough_copy_format value: "pdf",
    // "agent_task", "both", "agent", "word") puts on the clipboard.
    std::pair<std::vector<std::filesystem::path>, std::string>
    clipboard(std::string_view format) const;

    friend bool operator==(const WalkthroughResult&, const WalkthroughResult&) = default;
};

// What an AI write-up reads once the job is done: the screenshots' files and the held notes.
struct WriteUpInput {
    std::vector<std::filesystem::path> shots;
    std::vector<Note> notes;
};

// The walkthrough's folder could not be created, so no screenshot can be kept; the recording
// goes on. what() is the system's reason ("Access is denied"), as Python's strerror.
class ScreenshotNotSaved : public std::runtime_error {
public:
    ScreenshotNotSaved(int error_code, const std::string& strerror)
        : std::runtime_error(strerror), error_code(error_code) {}
    int error_code; // errno
};

// The screenshots of a walkthrough being recorded. The Qt thread adds them and edits the board,
// the job worker reads them once the recording stops. PNGs are written, removed screenshots
// deleted and draft.json rewritten on the session's own thread ("fv-walkthrough-shots"), one
// task at a time in the order queued; the destructor waits for what is still queued there.
// Affinity: thread-safe (one mutex).
class WalkthroughSession : public std::enable_shared_from_this<WalkthroughSession> {
public:
    struct Options {
        // Local wall time; now (the clock's zone) when unset.
        std::optional<std::chrono::local_seconds> started_at;
        // The walkthrough's folder, created when first needed; else one is claimed under root
        // with new_capture_folder.
        std::optional<std::filesystem::path> folder;
        std::filesystem::path root; // the captures folder
        Clock* clock = nullptr;     // system_clock() when null
    };

    // A walkthrough found after a crash, as its draft.json reads (library.py recovery_session
    // fills Python's private lists the same way): its screenshots already written, in the order
    // kept, the stretches held for a note, and its folder.
    struct Recovered {
        std::chrono::local_seconds started_at;
        std::filesystem::path folder;
        // Each screenshot's file and kept_at_s, in the order kept; numbered from 1 in this order.
        std::vector<std::pair<std::filesystem::path, double>> shots;
        // (start, end) in seconds, in time order; +inf: held to the end.
        std::vector<std::pair<double, double>> notes_s;
    };

    // Throws std::invalid_argument when Options names neither a folder nor a root: the folder
    // would otherwise be claimed in the working directory.
    explicit WalkthroughSession(Options options);
    // The session of a recovered draft: closed, its screenshots counted as written, its folder
    // taken as it is (not created). Starts no writer thread.
    static std::unique_ptr<WalkthroughSession> recovered(Recovered draft);
    ~WalkthroughSession();
    WalkthroughSession(const WalkthroughSession&) = delete;
    WalkthroughSession& operator=(const WalkthroughSession&) = delete;

    // Whether screenshots can still be added: the recording hasn't stopped.
    bool open() const;
    // Take no more screenshots; a note still held runs to the end of the recording.
    void close();

    // Hold for note was pressed (or let go) at_s seconds into the recording.
    void hold_note(double at_s);
    void release_note(double at_s);

    // The walkthrough's folder, created the first time it is needed. Throws
    // std::filesystem::filesystem_error when it cannot be.
    std::filesystem::path folder() const;
    // The screenshots on the board: kept and not removed.
    int shot_count() const;

    // Keeps `image` as the next screenshot, numbered from 1 in the order kept, kept_at_s clamped
    // at 0; its PNG is written in the background. nullopt once the recording stopped. Throws
    // ScreenshotNotSaved when the folder cannot be created.
    std::optional<Shot> add_shot(ShotImagePtr image, double kept_at_s);
    // Screenshot `number` (as kept) leaves the board, its words joining the next section; its
    // file is deleted once written. False once stopped or when it is already gone.
    bool remove_shot(int number);
    // Removes the newest screenshot still on the board; its number, or nullopt.
    std::optional<int> undo_last();
    // Moves screenshot `number` and its words `step` sections (-1 up, 1 down). The words still
    // being spoken stay last. False when there is no room, once stopped, or when removed.
    bool move_shot(int number, int step);

    // Links this draft to the recorder's crash-safe journal: creates the folder (throws
    // std::filesystem::filesystem_error when it cannot) and queues a checkpoint. Off the Qt
    // thread.
    void checkpoint_recording(std::string journal_id);
    // Copies the recording to <folder>/audio.wav (through .audio.tmp), unless it is that file
    // already, then rewrites draft.json. Throws std::filesystem::filesystem_error.
    void preserve_audio(const std::filesystem::path& source);

    // Screenshots kept so far whose file could not be written.
    int lost_shots() const;
    // The board once its screenshots are written; worker thread. Waits up to `wait` for each
    // screenshot and each removal; one not written by then leaves the board like a removed one.
    Storyboard storyboard(std::chrono::milliseconds wait = SAVE_WAIT);
    // The screenshots whose PNG was written, in the board's order; worker thread.
    std::vector<Shot> saved_shots(std::chrono::milliseconds wait = SAVE_WAIT);

    // Deletes what the walkthrough wrote (its screenshots and the documents' fixed names) once
    // the writer is done, and its folder if that empties it.
    void discard();
    // Lets the writer finish the screenshots without waiting for it; the files stay. A session
    // owned by a shared_ptr then stays alive until its writer is done, so dropping the last
    // reference (on the UI thread, say) never waits for a PNG write, as Python's end() never
    // waits; the destructor then runs on the writer thread. Owned otherwise (a unique_ptr, the
    // stack), the destructor still waits for the writer.
    void end();

    // A screenshot's number when its file could not be written; emitted on the writer thread.
    Signal<int> save_failed;

    // Set by the job worker, read on the UI thread.
    void set_result(WalkthroughResult result);
    std::optional<WalkthroughResult> result() const;
    void set_write_up_input(WriteUpInput input);
    WriteUpInput write_up_input() const;
    // Why the document could not be written, when the words were kept without it.
    void set_document_error(std::string error);
    std::string document_error() const;

    std::chrono::local_seconds started_at() const;

private:
    struct BoardSection {
        // The recording's pieces it reads, in order: piece i is what was said before screenshot
        // i (0-based, as kept) was kept, the last piece what is said after the last screenshot.
        std::vector<int> pieces;
        // Which screenshot (0-based, as kept) closes it; nullopt while still being spoken.
        std::optional<int> shot;
    };
    struct Writer;

    std::filesystem::path folder_locked() const;
    std::vector<BoardSection>& board_locked() const;
    std::optional<std::size_t> place_locked(int number) const;
    void ensure_writer_locked();
    void checkpoint_later_locked();
    void checkpoint();
    bool write_shot(const ShotImagePtr& image, const Shot& shot);
    void delete_shot(const Shot& shot);
    static void drain(std::vector<std::unique_ptr<Writer>> writers);
    // `board` as the pieces the recording is cut into; `shots` are all those kept.
    static Storyboard make_storyboard(const std::vector<BoardSection>& board,
                                      const std::vector<Shot>& shots,
                                      std::vector<std::pair<double, double>> notes_s);
    // Takes the screenshot at `place` off `board`; its words open the next section.
    static void remove_at(std::vector<BoardSection>& board, std::size_t place);

    const std::chrono::local_seconds started_at_;
    const std::optional<std::filesystem::path> given_folder_;
    const std::filesystem::path root_;

    mutable std::mutex mutex_;
    mutable std::optional<std::filesystem::path> folder_;
    // Every screenshot kept, in the order kept, removed ones too: their moments still divide
    // the recording.
    std::vector<Shot> shots_;
    std::vector<std::shared_future<bool>> saves_;
    // Removed screenshots' files being deleted.
    std::vector<std::shared_future<bool>> deletes_;
    // The board once the user changed it; until then one section per screenshot (board_locked).
    mutable std::optional<std::vector<BoardSection>> board_;
    std::vector<std::pair<double, double>> notes_;
    std::optional<double> note_from_;
    std::unique_ptr<Writer> writer_;
    // Writers let go by end(): the destructor drains them (at once when it runs on their
    // thread, after end()'s keep-alive task).
    std::vector<std::unique_ptr<Writer>> retired_;
    bool open_ = true;
    std::string journal_id_;
    std::optional<WalkthroughResult> result_;
    WriteUpInput write_up_input_;
    std::string document_error_;
};

} // namespace fv::walkthrough
