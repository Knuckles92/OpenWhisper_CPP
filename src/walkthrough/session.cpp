#include "walkthrough/session.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <system_error>

#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "walkthrough/files.h"
#include "walkthrough/storage.h"
#include "walkthrough/write_up_names.h"

namespace fv::walkthrough {

namespace {

const log::Logger logger("services.walkthrough.session");

// How long discard() and the destructor wait for the writer's queue; Python waits without limit.
constexpr std::chrono::minutes kDrainBudget{10};

constexpr double kInf = std::numeric_limits<double>::infinity();

std::chrono::local_seconds now_local(const Clock& clock) {
    const auto utc = std::chrono::floor<std::chrono::seconds>(clock.utc());
    if (const auto* zone = clock.zone()) {
        return zone->to_local(utc);
    }
    return std::chrono::local_seconds{utc.time_since_epoch()};
}

// datetime.isoformat() of a naive local time with no microseconds.
std::string naive_isoformat(std::chrono::local_seconds when) {
    const auto day = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{when - day};
    py::DateTime value;
    value.year = static_cast<int>(date.year());
    value.month = static_cast<int>(static_cast<unsigned>(date.month()));
    value.day = static_cast<int>(static_cast<unsigned>(date.day()));
    value.hour = static_cast<int>(time.hours().count());
    value.minute = static_cast<int>(time.minutes().count());
    value.second = static_cast<int>(time.seconds().count());
    return value.isoformat();
}

// Python's OSError.strerror for a Windows error: FormatMessage's text without the trailing
// full stop and line break.
std::string strerror_of(const std::error_code& error) {
    std::string text = error.message();
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) <= ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

int errno_of(const std::error_code& error) {
    const auto condition = error.default_error_condition();
    return condition.category() == std::generic_category() ? condition.value() : error.value();
}

} // namespace

struct WalkthroughSession::Writer {
    ThreadPool pool{"fv-walkthrough-shots", 1};
};

// --- Storyboard ------------------------------------------------------------------------------

int Storyboard::section_at(double at_s) const {
    const auto piece =
        static_cast<int>(std::upper_bound(cuts_s.begin(), cuts_s.end(), at_s) - cuts_s.begin());
    for (std::size_t index = 0; index < sections.size(); ++index) {
        if (std::ranges::find(sections[index], piece) != sections[index].end()) {
            return static_cast<int>(index);
        }
    }
    throw std::out_of_range("No section reads that moment");
}

// --- WalkthroughResult -----------------------------------------------------------------------

std::string WalkthroughResult::name() const {
    return written_up ? "Write-up" : "Walkthrough";
}

std::filesystem::path WalkthroughResult::docx() const {
    auto path = pdf;
    path.replace_extension(".docx");
    return path;
}

std::pair<std::vector<std::filesystem::path>, std::string>
WalkthroughResult::clipboard(std::string_view format) const {
    if (format == "agent_task") {
        std::error_code error;
        auto resolved = std::filesystem::weakly_canonical(pdf, error);
        if (error) {
            resolved = std::filesystem::absolute(pdf);
        }
        std::string prompt =
            "Read the attached walkthrough PDF, including its screenshots, and carry out "
            "the tasks and changes I describe in it. Use the words and screenshots "
            "together to understand the intended result. Implement the requested work "
            "rather than only summarizing the document. Ask for clarification if a "
            "required detail is missing; otherwise proceed, verify the result, and "
            "report what you changed and anything still unresolved.\n\n"
            "PDF file (if the attachment is unavailable): " +
            path_to_utf8(resolved.make_preferred());
        return {{}, std::move(prompt)};
    }
    std::vector<std::filesystem::path> files;
    if (format == "pdf" || format == "both") {
        files.push_back(pdf);
    } else if (format == "word") {
        files.push_back(docx());
    }
    const bool text = format == "both" || format == "agent";
    return {std::move(files), text ? clipboard_text : std::string()};
}

// --- WalkthroughSession ----------------------------------------------------------------------

WalkthroughSession::WalkthroughSession(Options options)
    : started_at_(options.started_at ? *options.started_at
                                     : now_local(options.clock ? *options.clock : system_clock())),
      given_folder_(std::move(options.folder)), root_(std::move(options.root)) {
    if (!given_folder_ && root_.empty()) {
        throw std::invalid_argument(
            "A walkthrough session needs its folder or the captures folder");
    }
}

std::unique_ptr<WalkthroughSession> WalkthroughSession::recovered(Recovered draft) {
    Options options;
    options.started_at = draft.started_at;
    options.folder = draft.folder;
    auto session = std::make_unique<WalkthroughSession>(std::move(options));
    {
        std::lock_guard lock(session->mutex_);
        session->folder_ = std::move(draft.folder);
        for (auto& [path, kept_at_s] : draft.shots) {
            const int number = static_cast<int>(session->shots_.size()) + 1;
            session->shots_.push_back({number, std::move(path), kept_at_s});
            std::promise<bool> saved;
            saved.set_value(true);
            session->saves_.push_back(saved.get_future().share());
        }
        session->notes_ = std::move(draft.notes_s);
    }
    session->close();
    return session;
}

WalkthroughSession::~WalkthroughSession() {
    std::vector<std::unique_ptr<Writer>> writers;
    {
        std::lock_guard lock(mutex_);
        writers = std::move(retired_);
        if (writer_) {
            writers.push_back(std::move(writer_));
        }
    }
    drain(std::move(writers));
}

void WalkthroughSession::drain(std::vector<std::unique_ptr<Writer>> writers) {
    for (auto& writer : writers) {
        writer->pool.shutdown(kDrainBudget);
    }
}

bool WalkthroughSession::open() const {
    std::lock_guard lock(mutex_);
    return open_;
}

void WalkthroughSession::close() {
    std::lock_guard lock(mutex_);
    open_ = false;
    if (note_from_) {
        notes_.emplace_back(*note_from_, kInf);
        note_from_.reset();
    }
}

void WalkthroughSession::hold_note(double at_s) {
    std::lock_guard lock(mutex_);
    if (open_ && !note_from_) {
        note_from_ = std::max(0.0, at_s);
        checkpoint_later_locked();
    }
}

void WalkthroughSession::release_note(double at_s) {
    std::lock_guard lock(mutex_);
    const auto start = note_from_;
    note_from_.reset();
    if (start && at_s > *start) {
        notes_.emplace_back(*start, at_s);
    }
    if (start) {
        checkpoint_later_locked();
    }
}

void WalkthroughSession::checkpoint_later_locked() {
    // Recovery must leave a held note out of the document too.
    if (writer_) {
        writer_->pool.post([this] { checkpoint(); });
    }
}

std::filesystem::path WalkthroughSession::folder() const {
    std::lock_guard lock(mutex_);
    return folder_locked();
}

std::filesystem::path WalkthroughSession::folder_locked() const {
    if (!folder_) {
        if (given_folder_) {
            std::filesystem::create_directories(*given_folder_);
            folder_ = *given_folder_;
        } else {
            folder_ = new_capture_folder(root_, started_at_);
        }
    }
    return *folder_;
}

int WalkthroughSession::shot_count() const {
    std::lock_guard lock(mutex_);
    const auto& board = board_locked();
    return static_cast<int>(std::ranges::count_if(
        board, [](const BoardSection& section) { return section.shot.has_value(); }));
}

std::vector<WalkthroughSession::BoardSection>& WalkthroughSession::board_locked() const {
    if (!board_) {
        const int count = static_cast<int>(shots_.size());
        std::vector<BoardSection> board;
        for (int index = 0; index < count; ++index) {
            board.push_back({{index}, index});
        }
        board.push_back({{count}, std::nullopt});
        board_ = std::move(board);
    }
    return *board_;
}

std::optional<std::size_t> WalkthroughSession::place_locked(int number) const {
    const auto& board = board_locked();
    for (std::size_t place = 0; place < board.size(); ++place) {
        if (board[place].shot && *board[place].shot == number - 1) {
            return place;
        }
    }
    return std::nullopt;
}

void WalkthroughSession::remove_at(std::vector<BoardSection>& board, std::size_t place) {
    BoardSection removed = std::move(board[place]);
    board.erase(board.begin() + static_cast<std::ptrdiff_t>(place));
    auto& next = board[place].pieces;
    next.insert(next.begin(), removed.pieces.begin(), removed.pieces.end());
}

Storyboard WalkthroughSession::make_storyboard(const std::vector<BoardSection>& board,
                                               const std::vector<Shot>& shots,
                                               std::vector<std::pair<double, double>> notes_s) {
    // Pieces that follow each other in time within one section are one stretch of speech: no
    // cut between them, so no word is split there.
    std::vector<bool> joined(shots.size() + 1, false);
    for (const auto& section : board) {
        for (std::size_t at = 1; at < section.pieces.size(); ++at) {
            const int a = section.pieces[at - 1];
            if (section.pieces[at] == a + 1 && a >= 0 &&
                static_cast<std::size_t>(a) < joined.size()) {
                joined[static_cast<std::size_t>(a)] = true;
            }
        }
    }
    std::vector<int> cuts;
    for (std::size_t index = 0; index < shots.size(); ++index) {
        if (!joined[index]) {
            cuts.push_back(static_cast<int>(index));
        }
    }
    const auto piece = [&](int clip) {
        return static_cast<int>(std::ranges::lower_bound(cuts, clip) - cuts.begin());
    };

    Storyboard storyboard;
    storyboard.sections.clear();
    for (const auto& section : board) {
        std::vector<int> pieces;
        for (const int clip : section.pieces) {
            if (pieces.empty() || pieces.back() != piece(clip)) {
                pieces.push_back(piece(clip));
            }
        }
        storyboard.sections.push_back(std::move(pieces));
    }
    int number = 0;
    for (const auto& section : board) {
        if (section.shot) {
            const Shot& shot = shots[static_cast<std::size_t>(*section.shot)];
            storyboard.shots.push_back({++number, shot.path, shot.kept_at_s});
        }
    }
    for (const int index : cuts) {
        storyboard.cuts_s.push_back(shots[static_cast<std::size_t>(index)].kept_at_s);
    }
    storyboard.notes_s = std::move(notes_s);
    return storyboard;
}

void WalkthroughSession::ensure_writer_locked() {
    if (!writer_) {
        writer_ = std::make_unique<Writer>();
    }
}

bool WalkthroughSession::remove_shot(int number) {
    std::lock_guard lock(mutex_);
    const auto place = open_ ? place_locked(number) : std::nullopt;
    if (!place) {
        return false;
    }
    remove_at(board_locked(), *place);
    // After its own write: the writer runs one task at a time, in order.
    ensure_writer_locked();
    auto done = std::make_shared<std::promise<bool>>();
    deletes_.push_back(done->get_future().share());
    const Shot shot = shots_[static_cast<std::size_t>(number - 1)];
    writer_->pool.post([this, done, shot] {
        try {
            delete_shot(shot);
            done->set_value(true);
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    return true;
}

std::optional<int> WalkthroughSession::undo_last() {
    std::optional<int> number;
    {
        std::lock_guard lock(mutex_);
        std::optional<int> newest;
        for (const auto& section : board_locked()) {
            if (section.shot && (!newest || *section.shot > *newest)) {
                newest = section.shot;
            }
        }
        if (newest && open_) {
            number = *newest + 1;
        }
    }
    if (!number || !remove_shot(*number)) {
        return std::nullopt;
    }
    return number;
}

bool WalkthroughSession::move_shot(int number, int step) {
    std::lock_guard lock(mutex_);
    auto& board = board_locked();
    const auto place = open_ ? place_locked(number) : std::nullopt;
    if (!place) {
        return false;
    }
    const auto target = static_cast<std::int64_t>(*place) + step;
    if (target < 0 || target >= static_cast<std::int64_t>(board.size()) - 1) {
        return false;
    }
    std::swap(board[*place], board[static_cast<std::size_t>(target)]);
    return true;
}

void WalkthroughSession::delete_shot(const Shot& shot) {
    std::filesystem::remove(shot.path);
    checkpoint();
}

std::optional<Shot> WalkthroughSession::add_shot(ShotImagePtr image, double kept_at_s) {
    std::lock_guard lock(mutex_);
    if (!open_) {
        return std::nullopt;
    }
    std::filesystem::path folder;
    try {
        folder = folder_locked();
    } catch (const std::system_error& error) {
        logger.error("Could not create the walkthrough folder: {}", strerror_of(error.code()));
        throw ScreenshotNotSaved(errno_of(error.code()), strerror_of(error.code()));
    }
    const int index = static_cast<int>(shots_.size());
    Shot shot{index + 1, folder / path_from_utf8(std::format("shot-{}.png", index + 1)),
              std::max(0.0, kept_at_s)};
    shots_.push_back(shot);
    if (board_) {
        board_->back().shot = index;
        board_->push_back({{index + 1}, std::nullopt});
    }
    ensure_writer_locked();
    auto done = std::make_shared<std::promise<bool>>();
    saves_.push_back(done->get_future().share());
    writer_->pool.post([this, done, image = std::move(image), shot] {
        try {
            const bool written = write_shot(image, shot);
            done->set_value(written);
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    return shot;
}

bool WalkthroughSession::write_shot(const ShotImagePtr& image, const Shot& shot) {
    bool written = false;
    try {
        image->save_png(shot.path);
        written = true;
    } catch (const std::exception&) {
        logger.error("Could not write walkthrough screenshot {}",
                     path_to_utf8(shot.path.filename()));
        // A full disk leaves a truncated file behind; it is not a screenshot.
        std::error_code error;
        std::filesystem::remove(shot.path, error);
        if (error) {
            logger.debug("Could not remove the partial screenshot {}",
                         path_to_utf8(shot.path.filename()));
        }
    }
    checkpoint();
    if (!written) {
        try {
            save_failed.emit(shot.number);
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not report an unsaved walkthrough screenshot");
        }
    }
    return written;
}

void WalkthroughSession::checkpoint_recording(std::string journal_id) {
    std::lock_guard lock(mutex_);
    journal_id_ = std::move(journal_id);
    folder_locked();
    ensure_writer_locked();
    writer_->pool.post([this] { checkpoint(); });
}

void WalkthroughSession::checkpoint() {
    std::optional<std::filesystem::path> folder;
    Json value;
    {
        std::lock_guard lock(mutex_);
        folder = folder_;
        // In the order kept, which recovery needs; a removed screenshot's words go to the next
        // one's section there.
        std::vector<int> on_board;
        for (const auto& section : board_locked()) {
            if (section.shot) {
                on_board.push_back(*section.shot);
            }
        }
        std::ranges::sort(on_board);
        Json shots = Json::array();
        for (const int index : on_board) {
            const Shot& shot = shots_[static_cast<std::size_t>(index)];
            Json item = Json::object();
            item["shot"] = path_to_utf8(shot.path.filename());
            item["kept_at_s"] = shot.kept_at_s;
            shots.push_back(std::move(item));
        }
        // A note still held when the app stops runs to the end (null).
        Json notes = Json::array();
        const auto add_note = [&](double start, std::optional<double> end) {
            Json pair = Json::array();
            pair.push_back(start);
            pair.push_back(end && *end != kInf ? Json(*end) : Json(nullptr));
            notes.push_back(std::move(pair));
        };
        for (const auto& [start, end] : notes_) {
            add_note(start, end);
        }
        if (note_from_) {
            add_note(*note_from_, std::nullopt);
        }
        value = Json::object();
        value["version"] = 1;
        value["started_at"] = naive_isoformat(started_at_);
        value["journal_id"] = journal_id_;
        value["shots"] = std::move(shots);
        value["notes_s"] = std::move(notes);
    }
    if (folder) {
        try {
            atomic_json(*folder / path_from_utf8(DRAFT_NAME), value);
        } catch (const std::system_error& error) {
            logger.error_with(error, "Could not checkpoint walkthrough recovery metadata");
        }
    }
}

void WalkthroughSession::preserve_audio(const std::filesystem::path& source) {
    const auto folder = this->folder();
    const auto destination = folder / path_from_utf8(AUDIO_NAME);
    std::error_code error;
    const bool same = std::filesystem::exists(destination, error) &&
                      std::filesystem::equivalent(source, destination, error);
    if (!same) {
        const auto temporary = folder / ".audio.tmp";
        try {
            std::filesystem::copy_file(source, temporary,
                                       std::filesystem::copy_options::overwrite_existing);
            std::filesystem::rename(temporary, destination);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }
    checkpoint();
}

int WalkthroughSession::lost_shots() const {
    std::vector<std::shared_future<bool>> saves;
    {
        std::lock_guard lock(mutex_);
        saves = saves_;
    }
    int lost = 0;
    for (const auto& save : saves) {
        if (save.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        try {
            lost += save.get() ? 0 : 1;
        } catch (...) {
            ++lost;
        }
    }
    return lost;
}

Storyboard WalkthroughSession::storyboard(std::chrono::milliseconds wait) {
    std::vector<Shot> shots;
    std::vector<std::shared_future<bool>> saves;
    std::vector<std::shared_future<bool>> deletes;
    std::vector<BoardSection> board;
    std::vector<std::pair<double, double>> notes;
    {
        std::lock_guard lock(mutex_);
        shots = shots_;
        saves = saves_;
        deletes = deletes_;
        board = board_locked();
        notes = notes_;
    }
    for (std::size_t index = 0; index < shots.size(); ++index) {
        bool written = false;
        try {
            if (saves[index].wait_for(wait) != std::future_status::ready) {
                throw std::runtime_error("timed out");
            }
            written = saves[index].get();
        } catch (...) {
            logger.error("Walkthrough screenshot {} could not be written", shots[index].number);
        }
        if (!written) {
            for (std::size_t place = 0; place < board.size(); ++place) {
                if (board[place].shot && *board[place].shot == static_cast<int>(index)) {
                    remove_at(board, place);
                    break;
                }
            }
        }
    }
    for (const auto& removal : deletes) {
        try {
            if (removal.wait_for(wait) != std::future_status::ready) {
                throw std::runtime_error("timed out");
            }
            removal.get();
        } catch (...) {
            logger.warning("A removed walkthrough screenshot could not be deleted");
        }
    }
    return make_storyboard(board, shots, std::move(notes));
}

std::vector<Shot> WalkthroughSession::saved_shots(std::chrono::milliseconds wait) {
    return storyboard(wait).shots;
}

void WalkthroughSession::discard() {
    close();
    std::vector<std::unique_ptr<Writer>> writers;
    std::vector<Shot> shots;
    std::optional<std::filesystem::path> folder;
    {
        std::lock_guard lock(mutex_);
        writers = std::move(retired_);
        retired_.clear();
        if (writer_) {
            writers.push_back(std::move(writer_));
        }
        shots = shots_;
        folder = folder_;
    }
    drain(std::move(writers));
    if (!folder) {
        return;
    }
    const std::string_view names[] = {MARKDOWN_NAME,      PDF_NAME,
                                      MANIFEST_NAME,      write_up::MARKDOWN_NAME,
                                      write_up::PDF_NAME, write_up::DOCX_NAME,
                                      DOCX_NAME,          DRAFT_NAME,
                                      AUDIO_NAME,         "walkthrough.original.json"};
    for (const auto& shot : shots) {
        std::filesystem::remove(shot.path);
    }
    for (const auto name : names) {
        std::filesystem::remove(*folder / path_from_utf8(name));
    }
    std::error_code error;
    std::filesystem::remove(*folder, error);
    if (error) {
        logger.debug("Walkthrough folder kept: it isn't empty");
    }
}

void WalkthroughSession::end() {
    close();
    // Not shared-owned (weak_from_this is empty, or this is already being destroyed): no
    // keep-alive, and the destructor waits for the writer as before.
    std::shared_ptr<WalkthroughSession> self = weak_from_this().lock();
    std::lock_guard lock(mutex_);
    if (writer_) {
        // The writer's tasks use this session. Its last task holds a reference, so the session
        // outlives them; when that reference is the last one, the session is destroyed on the
        // writer thread, whose own pool its destructor shuts down without waiting.
        if (self) {
            writer_->pool.post([keep = std::move(self)]() mutable { keep.reset(); });
        }
        retired_.push_back(std::move(writer_));
    }
}

void WalkthroughSession::set_result(WalkthroughResult result) {
    std::lock_guard lock(mutex_);
    result_ = std::move(result);
}

std::optional<WalkthroughResult> WalkthroughSession::result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void WalkthroughSession::set_write_up_input(WriteUpInput input) {
    std::lock_guard lock(mutex_);
    write_up_input_ = std::move(input);
}

WriteUpInput WalkthroughSession::write_up_input() const {
    std::lock_guard lock(mutex_);
    return write_up_input_;
}

void WalkthroughSession::set_document_error(std::string error) {
    std::lock_guard lock(mutex_);
    document_error_ = std::move(error);
}

std::string WalkthroughSession::document_error() const {
    std::lock_guard lock(mutex_);
    return document_error_;
}

std::chrono::local_seconds WalkthroughSession::started_at() const {
    return started_at_;
}

} // namespace fv::walkthrough
