#include "clipboard/temporary_clipboard.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <QClipboard>
#include <QList>
#include <QMimeData>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "clipboard/html.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::clipboard {

bool TemporaryClipboard::markerPresent(std::string_view marker) {
    try {
        if (!backend_)
            return false;
        const QMimeData* mime = backend_->mimeData();
        return mime && mime->data(kAutoPasteMarkerFormat) ==
                           QByteArray(marker.data(), static_cast<qsizetype>(marker.size()));
    } catch (const std::exception&) {
        return false;
    }
}

const QString kAutoPasteMarkerFormat = QStringLiteral("application/x-") +
                                       qt::toQString(py::lower(identity().product)) +
                                       QStringLiteral("-auto-paste");

namespace {

constexpr log::Logger logger("ui_qt.clipboard");

// secrets.token_bytes(16).
constexpr std::size_t kTokenBytes = 16;

QByteArray new_token() {
    std::array<quint32, kTokenBytes / sizeof(quint32)> words{};
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
    QByteArray token(static_cast<qsizetype>(kTokenBytes), '\0');
    std::memcpy(token.data(), words.data(), kTokenBytes);
    return token;
}

// str(exc) or "unknown clipboard error".
std::string error_message(const std::exception* error) {
    if (error != nullptr && error->what() != nullptr && error->what()[0] != '\0') {
        return error->what();
    }
    return "unknown clipboard error";
}

int clamp_ms(std::chrono::milliseconds delay) {
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(delay.count(), 0, INT_MAX));
}

// os.path.abspath: absolute and normalized without resolving links; "" is the current
// directory, a trailing separator is dropped (normpath), and it never raises (a path the OS
// cannot make absolute is only normalized, as _abspath_fallback joins it to the cwd lexically).
std::filesystem::path abspath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = path.empty() ? std::filesystem::current_path(error)
                                                  : std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    absolute = absolute.lexically_normal();
    if (!absolute.has_filename() && absolute.has_relative_path()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

} // namespace

TemporaryClipboard::TemporaryClipboard(QClipboard* clipboard, SequenceSource sequence,
                                       Renderer renderer, QObject* parent,
                                       std::optional<bool> sees_foreign_copies)
    : TemporaryClipboard(qt_clipboard_backend(clipboard), std::move(sequence), std::move(renderer),
                         parent, sees_foreign_copies) {}

TemporaryClipboard::TemporaryClipboard(std::unique_ptr<ClipboardBackend> backend,
                                       SequenceSource sequence, Renderer renderer, QObject* parent,
                                       std::optional<bool> sees_foreign_copies)
    : QObject(parent), backend_(std::move(backend)),
      sees_foreign_copies_(sees_foreign_copies ? *sees_foreign_copies : foreign_copies_visible()),
      sequence_(std::move(sequence)), renderer_(std::move(renderer)), clock_(&system_clock()),
      restore_timer_(new QTimer(this)), prefetch_timer_(new QTimer(this)),
      selection_timer_(new QTimer(this)) {
    if (backend_) {
        connect(backend_.get(), &ClipboardBackend::dataChanged, this,
                &TemporaryClipboard::onClipboardDataChanged);
    }
    restore_timer_->setSingleShot(true);
    connect(restore_timer_, &QTimer::timeout, this, &TemporaryClipboard::restorePending);
    prefetch_timer_->setSingleShot(true);
    connect(prefetch_timer_, &QTimer::timeout, this, &TemporaryClipboard::capturePrefetch);
    selection_timer_->setTimerType(Qt::PreciseTimer);
    selection_timer_->setInterval(kSelectionPoll);
    connect(selection_timer_, &QTimer::timeout, this, &TemporaryClipboard::pollSelection);
}

TemporaryClipboard::~TemporaryClipboard() = default;

void TemporaryClipboard::request_prefetch(std::chrono::milliseconds delay) {
    qt::post(this, [this, delay] { startPrefetch(delay); });
}

void TemporaryClipboard::discard_prefetch() {
    qt::post(this, [this] { discardPrefetchNow(); });
}

void TemporaryClipboard::capture_selection(std::function<void()> send_copy,
                                           std::function<void(std::optional<std::string>)> callback,
                                           std::chrono::milliseconds timeout) {
    if (QThread::currentThread() != thread()) {
        qt::post(this, [this, send_copy = std::move(send_copy), callback = std::move(callback),
                        timeout]() mutable {
            startSelectionCapture(std::move(send_copy), std::move(callback), timeout);
        });
        return;
    }
    startSelectionCapture(std::move(send_copy), std::move(callback), timeout);
}

bool TemporaryClipboard::write_text(std::string text, std::string html) {
    abortSelectionCapture();
    const bool written = writeText(qt::toQString(text), qt::toQString(html));
    if (written) {
        forgetLeases();
        discardPending();
    }
    return written;
}

bool TemporaryClipboard::write_files(focus::ClipboardFiles files) {
    std::optional<QImage> image;
    if (!files.png_image.empty()) {
        QImage decoded =
            QImage::fromData(QByteArray(reinterpret_cast<const char*>(files.png_image.data()),
                                        static_cast<qsizetype>(files.png_image.size())),
                             "PNG");
        // A null image would still claim an image format that no app can paste.
        if (!decoded.isNull()) {
            image = std::move(decoded);
        }
    }
    return writeFiles(files.paths, qt::toQString(files.text), image,
                      QByteArray(files.marker.data(), static_cast<qsizetype>(files.marker.size())));
}

bool TemporaryClipboard::writeFiles(const std::vector<std::filesystem::path>& paths,
                                    const QString& text, const std::optional<QImage>& image,
                                    const QByteArray& marker) {
    abortSelectionCapture();
    if (!backend_) {
        logger.error("No Qt clipboard available");
        return false;
    }
    auto mime_data = std::make_unique<QMimeData>();
    if (!paths.empty()) {
        QList<QUrl> urls;
        urls.reserve(static_cast<qsizetype>(paths.size()));
        for (const auto& path : paths) {
            urls.append(QUrl::fromLocalFile(qt::toQString(path_to_utf8(abspath(path)))));
        }
        mime_data->setUrls(urls);
    }
    if (!text.isEmpty()) {
        mime_data->setText(text);
    }
    if (image) {
        mime_data->setImageData(*image);
    }
    if (!marker.isEmpty()) {
        mime_data->setData(kAutoPasteMarkerFormat, marker);
    }
    try {
        backend_->setMimeData(std::move(mime_data));
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Failed to copy files to the clipboard: {}", error.what()));
        return false;
    }
    forgetLeases();
    discardPending();
    return true;
}

focus::StageResult TemporaryClipboard::stage_text(std::string text_utf8,
                                                  std::optional<std::string> html_utf8,
                                                  bool restore) {
    const QString text = qt::toQString(text_utf8);
    const QString html = qt::toQString(html_utf8.value_or(std::string()));
    abortSelectionCapture();
    resolvePendingBeforeStage();
    if (!backend_) {
        logger.error("No Qt clipboard available");
        return focus::StageResult{false, std::nullopt, false};
    }
    if (!restore) {
        discardPrefetchNow();
        return focus::StageResult{stageWithoutLease(text, html), std::nullopt, false};
    }

    // Only after the pending restore above: rewriting the clipboard bumps its sequence number,
    // so a prefetch taken while the previous transcript was staged can never pass for the user's
    // content.
    std::shared_ptr<const ClipboardSnapshot> snapshot = takePrefetchedSnapshot();
    if (snapshot) {
        logger.debug("Reusing the clipboard snapshot taken while recording");
    } else {
        try {
            snapshot = captureSnapshot();
        } catch (const std::exception& error) {
            logger.warning("Could not snapshot clipboard before auto-paste: {}", error.what());
            return focus::StageResult{stageWithoutLease(text, html), std::nullopt, true};
        }
    }

    if (snapshot->is_blank()) {
        return focus::StageResult{stageWithoutLease(text, html), std::nullopt, false};
    }

    const focus::ClipboardLease lease{++next_lease_id_, qt::toUtf8(html)};
    const QByteArray token = new_token();
    std::unique_ptr<QMimeData> mime_data = text_mime_data(text, html);
    mime_data->setData(kAutoPasteMarkerFormat, token);
    try {
        backend_->setMimeData(std::move(mime_data));
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Failed to stage clipboard for auto-paste: {}", error.what()));
        return focus::StageResult{false, std::nullopt, false};
    }

    // Every earlier lease's marker was just overwritten.
    forgetLeases();
    leases_[lease.id] = StagedLease{token, snapshot};
    if (!owns(lease)) {
        leases_.erase(lease.id);
        logger.error("Clipboard write could not be verified before auto-paste");
        return focus::StageResult{false, std::nullopt, false};
    }

    pending_ = lease;
    renderStagedText(!html.isEmpty());
    return focus::StageResult{true, lease, false};
}

bool TemporaryClipboard::schedule_restore(const focus::ClipboardLease& lease,
                                          std::chrono::milliseconds delay) {
    if (!pending_ || !(*pending_ == lease) || !owns(lease)) {
        logger.info("Clipboard changed before restoration could be scheduled");
        discardPending();
        return false;
    }
    restore_timer_->start(clamp_ms(delay));
    return true;
}

bool TemporaryClipboard::commit_text(const focus::ClipboardLease& lease, std::string text) {
    if (!pending_ || !(*pending_ == lease) || !owns(lease)) {
        discardPending();
        return false;
    }
    const bool written = writeText(qt::toQString(text), qt::toQString(lease.html));
    if (written) {
        forgetLeases();
        discardPending();
    }
    return written;
}

focus::RestoreOutcome TemporaryClipboard::restore_now(const focus::ClipboardLease& lease) {
    if (pending_ && *pending_ == lease) {
        restore_timer_->stop();
        pending_.reset();
    }
    return restore(lease);
}

void TemporaryClipboard::cleanup() {
    abortSelectionCapture();
    discardPrefetchNow();
    const std::optional<focus::ClipboardLease> lease = std::exchange(pending_, std::nullopt);
    restore_timer_->stop();
    if (lease) {
        restore(*lease);
    }
}

void TemporaryClipboard::startPrefetch(std::chrono::milliseconds delay) {
    // Never let a previous recording's snapshot survive into this one.
    discardPrefetchNow();
    if (!sequence_ || !backend_) {
        return;
    }
    prefetch_timer_->start(clamp_ms(delay));
}

void TemporaryClipboard::capturePrefetch() {
    if (pending_) {
        // The previous transcript is staged until its restore runs, so the clipboard is not the
        // user's yet; stage_text will capture.
        logger.debug("Skipped clipboard prefetch while a restore is pending");
        return;
    }
    if (selection_) {
        // The clipboard holds the copied selection until it is restored.
        return;
    }
    const std::optional<std::uint64_t> sequence = readSequence();
    if (!sequence) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<const ClipboardSnapshot> snapshot;
    try {
        snapshot = captureSnapshot();
    } catch (const std::exception& error) {
        logger.debug("Clipboard prefetch failed; auto-paste will capture: {}", error.what());
        return;
    }
    prefetched_ = PrefetchedSnapshot{std::move(snapshot), *sequence};
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - started;
    logger.debug("Prefetched clipboard snapshot in {} ms", py::format_fixed(elapsed.count(), 0));
}

void TemporaryClipboard::discardPrefetchNow() {
    prefetch_timer_->stop();
    prefetched_.reset();
}

std::shared_ptr<const ClipboardSnapshot> TemporaryClipboard::takePrefetchedSnapshot() {
    std::optional<PrefetchedSnapshot> prefetched = std::exchange(prefetched_, std::nullopt);
    discardPrefetchNow();
    if (!prefetched) {
        return nullptr;
    }
    if (!sequence_ || sequence_() != prefetched->sequence) {
        logger.debug("Clipboard changed since the prefetch; capturing it again");
        return nullptr;
    }
    return prefetched->snapshot;
}

std::shared_ptr<const ClipboardSnapshot> TemporaryClipboard::captureSnapshot() {
    // A failed read throws before anything is captured.
    const QMimeData* mime_data = backend_->mimeData();
    ++captures_;
    return std::make_shared<const ClipboardSnapshot>(ClipboardSnapshot::capture(mime_data));
}

std::optional<std::uint64_t> TemporaryClipboard::readSequence() const {
    if (!sequence_) {
        return std::nullopt;
    }
    return sequence_();
}

void TemporaryClipboard::startSelectionCapture(
    std::function<void()> send_copy, std::function<void(std::optional<std::string>)> callback,
    std::chrono::milliseconds timeout) {
    if (selection_) {
        // A second copy now would read the first one's text.
        logger.info("Selection capture already running; ignoring another");
        callBack(callback, std::string());
        return;
    }
    if (!backend_) {
        callBack(callback, std::string());
        return;
    }
    if (!sees_foreign_copies_) {
        logger.info("This clipboard can't see other apps' copies; not copying the selection");
        callBack(callback, std::nullopt);
        return;
    }
    resolvePendingBeforeStage();
    std::shared_ptr<const ClipboardSnapshot> original = takePrefetchedSnapshot();
    if (!original) {
        try {
            original = captureSnapshot();
        } catch (const std::exception& error) {
            // Without a copy of the user's clipboard it could not be put back, so no copy is
            // sent at all.
            logger.warning("Could not snapshot clipboard before copying the selection: {}",
                           error.what());
            callBack(callback, std::string());
            return;
        }
    }
    auto capture = std::make_unique<SelectionCapture>();
    capture->original = std::move(original);
    capture->callback = std::move(callback);
    capture->deadline = clock_->steady() + std::max(timeout, std::chrono::milliseconds(0));
    capture->sequence = readSequence();
    selection_ = std::move(capture);
    const SelectionCapture* started = selection_.get();
    try {
        if (!send_copy) {
            throw std::bad_function_call();
        }
        send_copy();
    } catch (const std::exception& error) {
        logger.warning("Could not send the copy shortcut: {}", error.what());
        finishSelection(QString());
        return;
    } catch (...) {
        logger.warning("Could not send the copy shortcut: {}", error_message(nullptr));
        finishSelection(QString());
        return;
    }
    if (selection_.get() != started) {
        // Sending the copy ended this capture (a write in the copy's path).
        return;
    }
    selection_->sent = true;
    selection_timer_->start();
}

void TemporaryClipboard::pollSelection() {
    SelectionCapture* capture = selection_.get();
    if (capture == nullptr) {
        selection_timer_->stop();
        return;
    }
    const bool expired = clock_->steady() >= capture->deadline;
    capture->polls += 1;
    const QString original_text = capture->original->text.value_or(QString());
    if (!capture->sequence && !capture->changed &&
        (expired || capture->polls % kSelectionTextPolls == 0)) {
        // dataChanged alone misses other apps' copies where Qt does not signal them; the text
        // is read at a fraction of the poll rate.
        capture->changed = clipboardText() != original_text;
    }
    if (capture->sequence) {
        const std::optional<std::uint64_t> current = readSequence();
        if (current && *current != *capture->sequence) {
            capture->changed = true;
            // Apps empty the clipboard and then fill it, each a change; read once the number
            // has held still for one poll.
            const bool settled = current == capture->settling;
            capture->settling = current;
            if (!settled && !expired) {
                return;
            }
        }
    }
    if (capture->changed) {
        const QString text = clipboardText();
        // Without a counter the change may be the user's own clipboard handed over again (a
        // clipboard manager taking ownership), so only different text ends the wait early.
        const bool is_new = capture->sequence.has_value() || text != original_text;
        if ((!text.isEmpty() && is_new) || expired) {
            finishSelection(text);
        }
        return;
    }
    if (expired) {
        finishSelection(QString());
    }
}

void TemporaryClipboard::onClipboardDataChanged() {
    // Only consulted without a sequence number.
    if (selection_ && !selection_->sequence) {
        selection_->changed = true;
    }
}

void TemporaryClipboard::abortSelectionCapture() {
    if (selection_) {
        finishSelection(QString());
    }
}

void TemporaryClipboard::finishSelection(const QString& text) {
    std::unique_ptr<SelectionCapture> capture = std::move(selection_);
    selection_timer_->stop();
    if (!capture) {
        return;
    }
    bool changed = capture->changed;
    if (!changed && capture->sequence) {
        const std::optional<std::uint64_t> current = readSequence();
        changed = current && *current != *capture->sequence;
    } else if (!capture->sequence && capture->sent) {
        // Only a counter proves the copy never landed, and the text compare misses one of the
        // original's own text, so the original always goes back.
        changed = true;
    }
    bool holds_original = true;
    if (changed) {
        std::string message;
        try {
            backend_->setMimeData(capture->original->to_mime_data());
        } catch (const std::exception& error) {
            holds_original = false;
            message = error_message(&error);
            logger.error_with(
                error, std::format("Failed to restore clipboard after copying the selection: {}",
                                   message));
        }
        if (!holds_original) {
            emitRestoreFailed(message);
        }
    }
    if (holds_original && sequence_) {
        // The clipboard holds exactly this snapshot again, so the next paste can reuse it
        // instead of capturing the clipboard twice.
        if (const std::optional<std::uint64_t> sequence = sequence_()) {
            prefetched_ = PrefetchedSnapshot{capture->original, *sequence};
        }
    }
    callBack(capture->callback, qt::toUtf8(text));
}

QString TemporaryClipboard::clipboardText() {
    try {
        const QMimeData* mime_data = backend_->mimeData();
        if (mime_data == nullptr || !mime_data->hasText()) {
            return QString();
        }
        return mime_data->text();
    } catch (const std::exception& error) {
        logger.debug("Could not read the copied selection: {}", error.what());
        return QString();
    }
}

void TemporaryClipboard::callBack(const std::function<void(std::optional<std::string>)>& callback,
                                  std::optional<std::string> text) {
    try {
        callback(std::move(text));
    } catch (const std::exception& error) {
        logger.error_with(error, "Selection callback failed");
    } catch (...) {
        logger.error("Selection callback failed");
    }
}

void TemporaryClipboard::restorePending() {
    const std::optional<focus::ClipboardLease> lease = std::exchange(pending_, std::nullopt);
    if (lease) {
        restore(*lease);
    }
}

void TemporaryClipboard::resolvePendingBeforeStage() {
    if (!pending_) {
        return;
    }
    restore_timer_->stop();
    const focus::ClipboardLease lease = *std::exchange(pending_, std::nullopt);
    restore(lease);
}

focus::RestoreOutcome TemporaryClipboard::restore(const focus::ClipboardLease& lease) {
    if (!owns(lease)) {
        logger.info("Clipboard changed; skipped auto-paste restoration");
        return focus::RestoreOutcome::Skipped;
    }
    const std::shared_ptr<const ClipboardSnapshot> snapshot = leases_.at(lease.id).snapshot;
    std::string message;
    try {
        backend_->setMimeData(snapshot->to_mime_data());
        logger.info("Clipboard restored after auto-paste");
        leases_.erase(lease.id);
        return focus::RestoreOutcome::Restored;
    } catch (const std::exception& error) {
        message = error_message(&error);
        logger.error_with(error,
                          std::format("Failed to restore clipboard after auto-paste: {}", message));
    }
    // The lease may have been forgotten by a write a subscriber made; nothing to mark then.
    if (const auto record = leases_.find(lease.id); record != leases_.end()) {
        record->second.restore_failed = true;
    }
    emitRestoreFailed(message);
    return focus::RestoreOutcome::Failed;
}

bool TemporaryClipboard::owns(const focus::ClipboardLease& lease) {
    if (!backend_) {
        return false;
    }
    const auto record = leases_.find(lease.id);
    if (record == leases_.end()) {
        return false;
    }
    try {
        const QMimeData* mime_data = backend_->mimeData();
        if (mime_data == nullptr || !mime_data->hasFormat(kAutoPasteMarkerFormat)) {
            return false;
        }
        return mime_data->data(kAutoPasteMarkerFormat) == record->second.token;
    } catch (const std::exception&) {
        // Treated as "not ours": callers then skip the restore or report the unverified write.
        logger.debug("Could not read the clipboard to check its auto-paste marker");
        return false;
    }
}

bool TemporaryClipboard::writeText(const QString& text, const QString& html) {
    if (!backend_) {
        logger.error("No Qt clipboard available");
        return false;
    }
    try {
        if (!html.isEmpty()) {
            backend_->setMimeData(text_mime_data(text, html));
        } else {
            backend_->setText(text);
        }
        return true;
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to copy to clipboard: {}", error.what()));
        return false;
    }
}

bool TemporaryClipboard::stageWithoutLease(const QString& text, const QString& html) {
    const bool written = writeText(text, html);
    if (written) {
        forgetLeases();
        renderStagedText(!html.isEmpty());
    }
    return written;
}

void TemporaryClipboard::renderStagedText(bool rich) {
    const auto render = [](std::string_view name, const std::function<void()>& renderer) {
        if (!renderer) {
            return;
        }
        try {
            renderer();
        } catch (const std::exception& error) {
            logger.warning("Could not pre-render the staged clipboard {}: {}", name, error.what());
        }
    };
    render("text", renderer_.text);
    if (rich) {
        render("rich text", renderer_.html);
    }
}

void TemporaryClipboard::discardPending() {
    restore_timer_->stop();
    pending_.reset();
}

void TemporaryClipboard::forgetLeases() {
    std::erase_if(leases_, [](const auto& item) { return !item.second.restore_failed; });
    // Ids grow with time, so the oldest go first.
    while (leases_.size() > kKeptFailedLeases) {
        leases_.erase(leases_.begin());
    }
}

void TemporaryClipboard::emitRestoreFailed(const std::string& message) {
    Q_EMIT restoreFailed(qt::toQString(message));
    // A port subscriber on this thread runs inline, and one that throws would otherwise escape
    // into a restore timer's slot or skip a selection callback; PyQt reports a raising slot and
    // goes on.
    try {
        restore_failed.emit(message);
    } catch (const std::exception& error) {
        logger.error_with(error, "A restore_failed subscriber failed");
    } catch (...) {
        logger.error("A restore_failed subscriber failed");
    }
}

} // namespace fv::clipboard
