#include "ui_meeting/past_meetings_panel.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_history/calendar_glyph_button.h"
#include "ui_meeting/detail/payload.h"
#include "ui_meeting/meeting_delete_dialog.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_model.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/file_reveal.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.past_meetings_panel"};

// PastMeetingsPanel._apply_style.
const QString kPanelStyle = QStringLiteral(R"qss(
            QWidget#pastMeetingsContent {
                background-color: @bg;
                border-left: 1px solid rgba(@overlay-rgb, 0.08);
            }
            QLabel#pastMeetingsHeader {
                color: @text-heading;
                font-weight: 700;
                background-color: transparent;
            }
            QLabel#sectionHeader {
                color: @text-secondary-strong;
                padding-top: 4px;
                font-size: 11px;
                font-weight: 600;
                background-color: transparent;
            }
            QPushButton#pastMeetingsMenuBtn {
                background-color: transparent;
                color: @text-secondary;
                border: none;
                border-radius: 14px;
                padding: 0px;
                font-size: 15px;
            }
            QPushButton#pastMeetingsMenuBtn:hover {
                background-color: rgba(@overlay-rgb, 0.1);
                color: @text-heading;
            }
            QLineEdit#historySearchInput {
                background-color: rgba(@surface-rgb, 0.8);
                color: @text;
                border: 1px solid rgba(@overlay-rgb, 0.08);
                border-radius: 8px;
                padding: 4px 10px;
                font-size: 12px;
            }
            QLineEdit#historySearchInput:focus {
                border: 1px solid @accent;
                background-color: rgba(@surface-rgb, 1.0);
            }
            QLineEdit#historySearchInput::placeholder {
                color: @text-muted;
            }
            QLabel#pastMeetingsEmpty {
                color: @text-muted;
                font-size: 12px;
                padding: 8px 0px;
                background-color: transparent;
            }
            QScrollArea#pastMeetingsScrollArea {
                background-color: transparent;
                border: none;
            }
            QScrollArea#pastMeetingsScrollArea > QWidget > QWidget {
                background-color: transparent;
            }
            QScrollArea#pastMeetingsScrollArea QScrollBar:vertical {
                background: transparent;
                width: 8px;
                margin: 0px;
            }
            QScrollArea#pastMeetingsScrollArea QScrollBar::handle:vertical {
                background: rgba(@overlay-rgb, 0.15);
                border-radius: 4px;
                min-height: 30px;
            }
            QScrollArea#pastMeetingsScrollArea QScrollBar::handle:vertical:hover {
                background: rgba(@overlay-rgb, 0.3);
            }
            QScrollArea#pastMeetingsScrollArea QScrollBar::add-line:vertical,
            QScrollArea#pastMeetingsScrollArea QScrollBar::sub-line:vertical {
                height: 0px;
            }
            QScrollArea#pastMeetingsScrollArea QScrollBar::add-page:vertical,
            QScrollArea#pastMeetingsScrollArea QScrollBar::sub-page:vertical {
                background: transparent;
            }
            QFrame#pastMeetingItem {
                background-color: rgba(@surface-rgb, 0.5);
                border-radius: 12px;
                border: 1px solid rgba(@overlay-rgb, 0.05);
            }
            QFrame#pastMeetingItem:hover {
                background-color: rgba(@surface-hover-rgb, 0.6);
                border: 1px solid rgba(@accent-rgb, 0.35);
            }
            QFrame#pastMeetingItem[selected="true"] {
                background-color: rgba(@accent-rgb, 0.16);
                border: 1px solid rgba(@accent-rgb, 0.55);
            }
            QLabel#pastMeetingTitle {
                color: @text;
                background-color: transparent;
            }
            QLabel#pastMeetingMeta {
                color: @text-secondary-strong;
                font-size: 10px;
                background-color: transparent;
            }
            QLabel#pastMeetingPreview {
                color: @text-body;
                background-color: transparent;
            }
            QLabel#pastMeetingContentWarning {
                color: @warning-text;
                font-size: 11px;
                background-color: transparent;
            }
            /* The shared meeting pill: fully rounded, tinted, no outline
               (see the meeting badge rules in theme.qss). */
            QLabel#pastMeetingInsightsPill {
                color: @text-secondary-strong;
                background-color: rgba(@overlay-rgb, 0.08);
                border: none;
                border-radius: 10px;
                padding: 0px 8px;
                font-size: 11px;
                font-weight: 600;
            }
            QLabel#pastMeetingInsightsPill[pillTone="warning"] {
                color: @warning-text;
                background-color: rgba(@warning-rgb, 0.15);
            }
            QLabel#pastMeetingInsightsPill[pillTone="success"] {
                color: @success-text;
                background-color: rgba(@success-rgb, 0.15);
            }
            QLabel#pastMeetingInsightsPill[pillTone="location"] {
                color: @accent-soft;
                background-color: rgba(@accent-rgb, 0.15);
            }
            QLabel#pastMeetingInsightsPill[pillTone="neutral"] {
                color: @text-secondary-strong;
                background-color: rgba(@overlay-rgb, 0.08);
            }
        )qss");

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

} // namespace

PastMeetingsPanel::PastMeetingsPanel(Provider provider, Executor& io,
                                     const PastMeetingsOptions& options, QWidget* parent)
    : QWidget(parent), runDeleteDialog([](MeetingDeleteDialog& dialog) { return dialog.exec(); }),
      // Parented to the panel, as Python's QMessageBox.question(self, ...): over its window and
      // modal to it.
      confirm([this](const QString& title, const QString& question) {
          QMessageBox message(QMessageBox::Question, title, question,
                              QMessageBox::Yes | QMessageBox::No, this);
          message.setTextFormat(Qt::PlainText);
          message.setDefaultButton(QMessageBox::No);
          return message.exec() == QMessageBox::Yes;
      }),
      hostClearNote([] { return QString(); }),
      provider_(std::make_shared<Provider>(std::move(provider))), io_(io), options_(options),
      clock_(options.clock != nullptr ? options.clock : &system_clock()),
      ui_(std::make_shared<qt::QtUiExecutor>(this)),
      loadCanceled_(std::make_shared<std::atomic<bool>>(false)),
      sourceFiltered_(options.sourceFiltered) {
    setObjectName(QStringLiteral("pastMeetingsContent"));
    setAttribute(Qt::WA_StyledBackground, true);
    setupUi();
    ui::setLocalStyleSheet(this, kPanelStyle);
}

PastMeetingsPanel::~PastMeetingsPanel() {
    // A load still running stops before it hands anything back.
    loadCanceled_->store(true);
}

void PastMeetingsPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->setSpacing(8);

    menuButton_ = new QPushButton(QStringLiteral("☰"));
    menuButton_->setObjectName(QStringLiteral("pastMeetingsMenuBtn"));
    menuButton_->setFixedSize(28, 28);
    menuButton_->setCursor(Qt::PointingHandCursor);
    connect(menuButton_, &QPushButton::clicked, this, [this] {
        QMenu* menu = buildHeaderMenu();
        menu->exec(menuButton_->mapToGlobal(menuButton_->rect().bottomLeft()));
        menu->deleteLater();
    });
    header->addWidget(menuButton_);

    auto* title = new QLabel(QStringLiteral("Past Meetings"));
    title->setObjectName(QStringLiteral("pastMeetingsHeader"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::Bold));
    header->addWidget(title);
    header->addStretch();

    calendarButton_ = new CalendarGlyphButton(*clock_);
    calendarButton_->setToolTip(
        QStringLiteral("See meetings and history on a calendar (Ctrl+Shift+D)"));
    connect(calendarButton_, &CalendarGlyphButton::calendarRequested, this,
            &PastMeetingsPanel::calendarRequested);
    header->addWidget(calendarButton_);
    layout->addLayout(header);

    searchInput_ = new QLineEdit;
    searchInput_->setObjectName(QStringLiteral("historySearchInput"));
    searchInput_->setPlaceholderText(QStringLiteral("Search meetings..."));
    searchInput_->setClearButtonEnabled(true);
    searchInput_->setFixedHeight(32);
    connect(searchInput_, &QLineEdit::textChanged, this, [this] {
        // An in-flight result is out of date at once; the replacement query starts after the
        // user pauses typing.
        gate_.next();
        searchTimer_->start();
    });
    layout->addWidget(searchInput_);

    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(kSearchDelayMs);
    connect(searchTimer_, &QTimer::timeout, this, &PastMeetingsPanel::refresh);

    scrollArea_ = new QScrollArea;
    scrollArea_->setObjectName(QStringLiteral("pastMeetingsScrollArea"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* scrollContent = new QWidget;
    scrollContent->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* meetingsLayout = new QVBoxLayout(scrollContent);
    meetingsLayout->setContentsMargins(0, 0, 6, 0);
    meetingsLayout->setSpacing(12);
    meetingsLayout->setAlignment(Qt::AlignTop);

    // The panel title already says Past Meetings; this line only counts the meetings, and hides
    // while there are none.
    sectionHeader_ = new QLabel(QString());
    sectionHeader_->setObjectName(QStringLiteral("sectionHeader"));
    sectionHeader_->hide();
    sectionHeader_->setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold));
    meetingsLayout->addWidget(sectionHeader_);

    meetingsList_ = new QVBoxLayout;
    meetingsList_->setSpacing(12);
    meetingsLayout->addLayout(meetingsList_);

    scrollArea_->setWidget(scrollContent);
    layout->addWidget(scrollArea_, 1);
}

std::vector<Json> PastMeetingsPanel::exportableMeetings() const {
    std::vector<Json> kept;
    for (const Json& meeting : meetings_) {
        if (!ui_meeting::detail::truthy(ui_meeting::detail::get(meeting, "stored_on"))) {
            kept.push_back(meeting);
        }
    }
    return kept;
}

void PastMeetingsPanel::setSelectedMeetingId(const QString& meetingId) {
    selectedId_ = meetingId;
    applySelection();
}

void PastMeetingsPanel::applySelection() {
    for (PastMeetingItem* card : findChildren<PastMeetingItem*>()) {
        card->setSelected(card->meetingId() == selectedId_);
    }
}

PastMeetingItem* PastMeetingsPanel::cardFor(const QString& meetingId) const {
    for (PastMeetingItem* card : findChildren<PastMeetingItem*>()) {
        if (card->meetingId() == meetingId) {
            return card;
        }
    }
    return nullptr;
}

std::vector<PastMeetingItem*> PastMeetingsPanel::cards() const {
    std::vector<PastMeetingItem*> shown;
    for (int index = 0; index < meetingsList_->count(); ++index) {
        if (auto* card = qobject_cast<PastMeetingItem*>(meetingsList_->itemAt(index)->widget())) {
            shown.push_back(card);
        }
    }
    return shown;
}

const Json* PastMeetingsPanel::meetingById(const QString& meetingId) const {
    const std::string id = qt::toUtf8(meetingId);
    for (const Json& meeting : meetings_) {
        if (ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(meeting, "id")) == id) {
            return &meeting;
        }
    }
    return nullptr;
}

std::string PastMeetingsPanel::pairedHostFingerprint(const QString& meetingId) const {
    const auto* meeting = meetingById(meetingId);
    return meeting ? ui_meeting::detail::strOrEmpty(
                         ui_meeting::detail::get(*meeting, "_stored_on_fingerprint"))
                   : std::string();
}

void PastMeetingsPanel::onCardSelected(const QString& meetingId) {
    if (const Json* meeting = meetingById(meetingId)) {
        const std::string host =
            ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(*meeting, "stored_on"));
        if (!host.empty()) {
            fetchFromHost(meetingId, text(host), QStringLiteral("open"));
            return;
        }
    }
    selectedId_ = meetingId;
    applySelection();
    Q_EMIT meetingSelected(meetingId);
}

void PastMeetingsPanel::fetchFromHost(const QString& meetingId, const QString& host,
                                      const QString& then) {
    if (std::find(fetching_.begin(), fetching_.end(), meetingId) != fetching_.end()) {
        return;
    }
    fetching_.push_back(meetingId);
    onHostFetchProgress(meetingId, QStringLiteral("Getting it from %1…").arg(host));
    Q_EMIT hostFetchRequested(meetingId, host, then);
}

void PastMeetingsPanel::onHostFetchProgress(const QString& meetingId, const QString& progress) {
    if (PastMeetingItem* card = cardFor(meetingId)) {
        card->setProgress(progress);
    }
}

void PastMeetingsPanel::onHostFetchDone(const QString& meetingId, const QString& then,
                                        const QString& error) {
    fetching_.erase(std::remove(fetching_.begin(), fetching_.end(), meetingId), fetching_.end());
    if (!error.isEmpty()) {
        if (PastMeetingItem* card = cardFor(meetingId)) {
            card->setProgress(then == QStringLiteral("delete")
                                  ? QStringLiteral("Couldn't delete it: %1").arg(error)
                                  : QStringLiteral("Couldn't open it: %1").arg(error));
        }
        return;
    }
    refresh();
    if (then == QStringLiteral("open")) {
        selectedId_ = meetingId;
        Q_EMIT meetingSelected(meetingId);
    }
}

QMenu* PastMeetingsPanel::buildHeaderMenu() {
    QMenu* menu = contextMenu(this);
    connect(menu->addAction(QStringLiteral("Refresh")), &QAction::triggered, this,
            &PastMeetingsPanel::refresh);
    const bool hasMeetings = !meetings_.empty();
    QAction* exportAction = menu->addAction(QStringLiteral("Export past meetings…"));
    exportAction->setEnabled(hasMeetings);
    connect(exportAction, &QAction::triggered, this, &PastMeetingsPanel::exportRequested);
    connect(menu->addAction(QStringLiteral("Open meetings folder")), &QAction::triggered, this,
            &PastMeetingsPanel::openMeetingsFolderRequested);
    menu->addSeparator();
    QAction* clearAction = menu->addAction(QStringLiteral("Clear history"));
    clearAction->setEnabled(hasMeetings);
    connect(clearAction, &QAction::triggered, this, &PastMeetingsPanel::onClearHistory);
    QAction* clearAllAction = menu->addAction(QStringLiteral("Clear history + recordings"));
    clearAllAction->setEnabled(hasMeetings);
    connect(clearAllAction, &QAction::triggered, this,
            &PastMeetingsPanel::onClearHistoryAndRecordings);
    return menu;
}

QLabel* PastMeetingsPanel::placeholder(const QString& message) const {
    auto* label = new QLabel(message);
    label->setObjectName(QStringLiteral("pastMeetingsEmpty"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

// _clear_list: every item goes now, not at the next event-loop pass: a card left showing until
// then reads as one more meeting than the header says.
void PastMeetingsPanel::clearList() {
    cards_.clear();
    while (meetingsList_->count() > 0) {
        QLayoutItem* item = meetingsList_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void PastMeetingsPanel::refresh() {
    const std::string query = py::lower(py::strip(qt::toUtf8(searchInput_->text())));
    const std::uint64_t token = gate_.next();
    loadCanceled_->store(true);
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    loadCanceled_ = canceled;

    if (meetings_.empty()) {
        clearList();
        meetingsList_->addWidget(placeholder(QStringLiteral("Loading past meetings…")));
    }
    // One row more than is shown tells there are more.
    const int limit = kMaxMeetings + 1;
    std::shared_ptr<Provider> provider = provider_;
    std::function<Json(const std::string&)> summarize = options_.summarize;
    const bool sourceFiltered = options_.sourceFiltered;
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    // `this` is used only on the GUI thread, by a task the panel's executor drops once the panel
    // is gone.
    const auto deliver = [this, ui, token, sourceFiltered](std::vector<Json> meetings,
                                                           std::string error) {
        ui->post([this, token, sourceFiltered, error = std::move(error),
                  meetings = std::move(meetings)]() mutable {
            applyMeetingResults(token, std::move(meetings), text(error), sourceFiltered);
        });
    };
    io_.post([provider, summarize, deliver, canceled, query, limit, &io = io_,
              wanted = options_.hostWanted, merge = options_.hostMerge,
              wait = options_.remoteMergeWait] {
        // The provider blocks on the database: never on the GUI thread (fails in debug builds).
        assert_not_gui_thread();
        std::vector<Json> meetings;
        std::string error;
        try {
            meetings =
                past_meetings::historical_meetings((*provider)(limit, text(query)), summarize);
        } catch (const std::exception& failure) {
            kLog.error_with(failure,
                            std::format("Failed to load past meetings: {}", failure.what()));
            // As in Python, a failure with no message counts as no failure.
            error = failure.what();
            meetings.clear();
        }
        if (canceled->load()) {
            return;
        }
        bool askHost = false;
        if (error.empty() && wanted && merge) {
            try {
                askHost = wanted();
            } catch (const std::exception& failure) {
                kLog.debug("Record sync unavailable for Past Meetings: {}", failure.what());
            }
        }
        if (!askHost) {
            deliver(std::move(meetings), std::move(error));
            return;
        }
        // _start_remote_listing: the host is asked by a task of its own, so this computer's
        // meetings show after REMOTE_MERGE_WAIT_S even while the host is slow.
        struct Listing {
            std::mutex mutex;
            std::condition_variable arrived;
            bool done = false;
            std::vector<Json> merged;
            std::function<void()> late;
        };
        auto listing = std::make_shared<Listing>();
        io.post([listing, merge, local = meetings, query, limit]() mutable {
            std::vector<Json> merged;
            try {
                merged = merge(local, text(query), limit);
            } catch (const std::exception& failure) {
                kLog.warning("Couldn't list the host's meetings: {}", failure.what());
                merged = std::move(local);
            }
            std::function<void()> late;
            {
                const std::lock_guard lock(listing->mutex);
                listing->merged = std::move(merged);
                listing->done = true;
                late = std::move(listing->late);
            }
            listing->arrived.notify_all();
            if (late) {
                late();
            }
        });
        std::unique_lock lock(listing->mutex);
        if (!listing->arrived.wait_for(lock, wait, [&] { return listing->done; })) {
            listing->late = [listing, deliver, canceled] {
                if (!canceled->load()) {
                    deliver(std::move(listing->merged), {});
                }
            };
            lock.unlock();
            deliver(std::move(meetings), {});
            return;
        }
        auto merged = std::move(listing->merged);
        lock.unlock();
        if (!canceled->load()) {
            deliver(std::move(merged), {});
        }
    });
}

void PastMeetingsPanel::showLoadFailure() {
    meetings_.clear();
    clearList();
    sectionHeader_->setText(QString());
    sectionHeader_->hide();
    meetingsList_->addWidget(placeholder(QStringLiteral("Past meetings could not be loaded")));
}

void PastMeetingsPanel::applyMeetingResults(std::uint64_t generation, std::vector<Json> meetings,
                                            const QString& error, bool sourceFiltered) {
    if (generation != gate_.current()) {
        return;
    }
    if (!error.isEmpty()) {
        showLoadFailure();
        return;
    }
    meetings_ = std::move(meetings);
    // A meeting kept here whose copy the paired host also holds (the listing's _host_copy_name)
    // says so on its card: "Also on <host>", as OpenWhisper's copies_on_host merge marks it.
    for (Json& meeting : meetings_) {
        using ui_meeting::detail::get;
        using ui_meeting::detail::strOrEmpty;
        const std::string copy = strOrEmpty(get(meeting, "_host_copy_name"));
        if (!copy.empty() && meeting.is_object() && strOrEmpty(get(meeting, "stored_on")).empty() &&
            strOrEmpty(get(meeting, "also_on")).empty()) {
            meeting["also_on"] = copy;
        }
    }
    sourceFiltered_ = sourceFiltered;
    hasMore_ = meetings_.size() > static_cast<std::size_t>(kMaxMeetings);
    rebuildList();
}

PastMeetingItem* PastMeetingsPanel::createCard(const Json& meeting) {
    auto* card = new PastMeetingItem(meeting, *clock_, scrollArea_->widget());
    connect(card, &PastMeetingItem::meetingSelected, this, &PastMeetingsPanel::onCardSelected);
    card->setSpeakerUploadAvailable(speakerUploadAvailable_);
    connect(card, &PastMeetingItem::speakerUploadRequested, this,
            &PastMeetingsPanel::speakerUploadRequested);
    connect(card, &PastMeetingItem::copyTranscriptRequested, this,
            &PastMeetingsPanel::copyTranscriptRequested);
    connect(card, &PastMeetingItem::hostCopyDeleteRequested, this,
            [this](const QString& id, const QString& host, const QString& title,
                   const QString& fingerprint, const QString& revision) {
                if (std::ranges::find(fetching_, id) != fetching_.end()) {
                    return;
                }
                const auto name = title.isEmpty() ? id : title;
                if (!confirm(QStringLiteral("Delete meeting copy"),
                             QStringLiteral(
                                 "Delete “%1” from %2?\n\nThe copy on this computer will be kept.")
                                 .arg(name, host))) {
                    return;
                }
                fetching_.push_back(id);
                onHostFetchProgress(id, QStringLiteral("Deleting the copy on %1…").arg(host));
                Q_EMIT hostCopyDeleteRequested(id, fingerprint, revision);
            });
    connect(card, &PastMeetingItem::deleteRequested, this, &PastMeetingsPanel::confirmDelete);
    return card;
}

void PastMeetingsPanel::rebuildList() {
    const past_meetings::ListView view = past_meetings::list_view(
        meetings_, sourceFiltered_, hasMore_, qt::toUtf8(searchInput_->text()), *clock_);
    sectionHeader_->setText(text(view.count));
    sectionHeader_->setVisible(view.count_visible);

    if (!view.placeholder.empty()) {
        clearList();
        meetingsList_->addWidget(placeholder(text(view.placeholder)));
        return;
    }

    std::vector<ReconcileItem> items;
    items.reserve(view.cards.size());
    for (const Json& meeting : view.cards) {
        items.push_back(
            {ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(meeting, "id")), meeting});
    }
    std::vector<QWidget*> made;
    try {
        cards_ = reconcileCards(
            meetingsList_, items, cards_,
            [&](std::size_t index) -> QWidget* {
                QWidget* card = createCard(view.cards[index]);
                made.push_back(card);
                return card;
            },
            [&](QWidget* card, std::size_t index) {
                static_cast<PastMeetingItem*>(card)->setMeeting(view.cards[index]);
            });
    } catch (...) {
        // Nothing reached the layout: the list keeps its old cards and the new ones go.
        for (QWidget* card : made) {
            delete card;
        }
        throw;
    }

    if (!view.footer.empty()) {
        meetingsList_->addWidget(placeholder(text(view.footer)));
    }
    applySelection();
}

bool PastMeetingsPanel::deleteConfirmationWanted() const {
    if (options_.settings == nullptr) {
        return true;
    }
    try {
        return options_.settings->get(keys::confirm_meeting_delete);
    } catch (const std::exception& failure) {
        kLog.warning("Failed to load meeting deletion preference: {}", failure.what());
        return true;
    }
}

void PastMeetingsPanel::confirmDelete(const QString& meetingId) {
    const Json* meeting = meetingById(meetingId);
    if (meeting != nullptr) {
        const std::string host =
            ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(*meeting, "stored_on"));
        if (!host.empty()) {
            const QString hostText = text(host);
            if (confirm(QStringLiteral("Delete meeting"),
                        QStringLiteral("Delete this meeting from %1?\n\nIts transcript, insights "
                                       "and recordings there are deleted. This cannot be undone.")
                            .arg(hostText))) {
                fetchFromHost(meetingId, hostText, QStringLiteral("delete"));
            }
            return;
        }
    }
    if (!deleteConfirmationWanted()) {
        Q_EMIT deleteMeetingRequested(meetingId, false);
        return;
    }

    const bool hasAudio = past_meetings::has_audio(meeting != nullptr ? *meeting : Json());
    MeetingDeleteDialog dialog(this, hasAudio);
    if (runDeleteDialog(dialog) != QDialog::Accepted) {
        return;
    }
    const bool deleteRecordings = hasAudio && dialog.deleteRecordings()->isChecked();
    if (dialog.dontAskAgain()->isChecked() && options_.settings != nullptr) {
        try {
            options_.settings->save_setting(keys::confirm_meeting_delete.key(), false);
        } catch (const std::exception& failure) {
            kLog.warning("Failed to save meeting deletion preference: {}", failure.what());
        }
    }
    Q_EMIT deleteMeetingRequested(meetingId, deleteRecordings);
}

void PastMeetingsPanel::onClearHistory() {
    if (!confirm(QStringLiteral("Clear History"),
                 QStringLiteral("Delete all past meetings?\n\nSaved recordings will be kept.") +
                     hostClearNote())) {
        return;
    }
    Q_EMIT clearMeetingsRequested(false);
}

void PastMeetingsPanel::onClearHistoryAndRecordings() {
    if (!confirm(QStringLiteral("Clear History and Recordings"),
                 QStringLiteral("Delete all past meetings AND permanently delete their "
                                "recordings from disk?\n\nThis cannot be undone.") +
                     hostClearNote())) {
        return;
    }
    Q_EMIT clearMeetingsRequested(true);
}

bool openMeetingsFolder(const std::filesystem::path& folder) {
    std::filesystem::path absolute = std::filesystem::absolute(folder).lexically_normal();
    if (!absolute.has_filename()) {
        absolute = absolute.parent_path();
    }
    // _on_open_meetings_folder: os.makedirs raises, so nothing opens. The slot's exception
    // ends in OpenWhisper's log; here the error is logged and false returned.
    std::error_code error;
    std::filesystem::create_directories(absolute, error);
    if (error) {
        kLog.error("Could not create the meetings folder {}: {}", path_to_utf8(absolute),
                   error.message());
        return false;
    }
    return openFolderInFileManager(text(path_to_utf8(absolute)));
}

} // namespace fv
