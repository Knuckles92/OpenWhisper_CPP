#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <QBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "automation/filters.h"
#include "automation/hooks_model.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/wrapped_label.h"

class QGridLayout;
class QTimer;
class QVBoxLayout;

// The lists of Settings > Automate > Hooks (ui_qt/widgets/hooks_panel.py, from _EngineRelay to
// FilterSection): each kind of hook shows its saved items as rows with one editor that opens
// under the row being edited. Saving goes through SettingsStore::mutate with the models of
// automation/hooks_model.h. The engines answer for secrets, tests, recent deliveries and leases;
// their slow calls run on the io executor or on the engine's own thread, and come back to the
// widgets through EngineRequests, so no other thread ever holds a widget.
namespace fv {

class Button;
class ElidingLabel;
class EmptyState;
class FilterEditor;
class FlowLayout;
class HookEditor;
class ProgramEditor;
class SettingsStore;
class SettingsSwitch;
class WebhookEditor;

// What the sections and editors reach beyond their own widgets. An engine that is absent
// (outside the running app) leaves its Test buttons disabled and its recent lists empty.
struct HookServices {
    SettingsStore* store = nullptr;
    // Blocking work (credential store) runs here, never on the GUI thread.
    Executor* io = nullptr;
    automation::WebhookControl* webhooks = nullptr;
    automation::ProgramControl* programs = nullptr;
    automation::FilterLeases* leases = nullptr;
    // The zone and "now" of record times and lease countdowns; null is the system clock.
    const Clock* clock = nullptr;
};

namespace hooks_panel {

// str(exc).strip(), or the exception's type when it has no words.
QString errorText(const std::exception& failure);

// The next answers still on their way are dropped by forget(); a late answer to a request made
// before it arrives to nothing. Python's _Requests over the session relay.
// Affinity: GUI thread (answers may be handed over from any thread).
class EngineRequests {
public:
    template <class T> using Done = std::function<void(std::optional<T> result, QString error)>;

    // `owner` must be destroyed after this object (a member of it, say): the answers are posted
    // to it only while this object lives, under a lock the destructor takes, so a worker never
    // posts to an owner that is going away.
    explicit EngineRequests(QObject* owner) : lifeline_(std::make_shared<Lifeline>()) {
        lifeline_->owner = owner;
    }
    ~EngineRequests() {
        const std::lock_guard lock(lifeline_->mutex);
        lifeline_->owner = nullptr;
    }
    EngineRequests(const EngineRequests&) = delete;
    EngineRequests& operator=(const EngineRequests&) = delete;

    // Drops the answers still on their way.
    void forget() { gate_.next(); }

    // Runs call() on `io`; done(result, error) runs on the owner's thread unless forget() came
    // first or the owner is gone.
    template <class T> void inBackground(Executor& io, std::function<T()> call, Done<T> done) {
        const std::uint64_t generation = gate_.current();
        io.post([call = std::move(call), done = std::move(done), gate = gate_, generation,
                 lifeline = lifeline_]() mutable {
            std::optional<T> result;
            QString error;
            try {
                result = call();
            } catch (const std::exception& failure) {
                error = errorText(failure);
            }
            const std::lock_guard lock(lifeline->mutex);
            if (lifeline->owner != nullptr) {
                gate.deliver(
                    lifeline->owner, generation,
                    [done = std::move(done), result = std::move(result),
                     error = std::move(error)]() mutable { done(std::move(result), error); });
            }
        });
    }

    // start(report) begins work that calls report(result) once, on any thread; a start that
    // throws answers at once. Python's when_done over a Future.
    template <class T>
    void whenDone(const std::function<void(std::function<void(T)>)>& start, Done<T> done) {
        const std::uint64_t generation = gate_.current();
        auto shared = std::make_shared<Done<T>>(std::move(done));
        auto report = [gate = gate_, generation, lifeline = lifeline_, shared](T result) mutable {
            {
                const std::lock_guard lock(lifeline->mutex);
                if (lifeline->owner == nullptr) {
                    return;
                }
                if (QThread::currentThread() != lifeline->owner->thread()) {
                    gate.deliver(lifeline->owner, generation,
                                 [shared, result = std::move(result)]() mutable {
                                     (*shared)(std::move(result), QString());
                                 });
                    return;
                }
            }
            // The engine answered at once, on the owner's own thread: so does the widget.
            if (gate.isCurrent(generation)) {
                (*shared)(std::move(result), QString());
            }
        };
        try {
            start(std::move(report));
        } catch (const std::exception& failure) {
            if (gate_.isCurrent(generation)) {
                (*shared)(std::nullopt, errorText(failure));
            }
        }
    }

private:
    // Shared with the workers: the owner, null once this object is gone.
    struct Lifeline {
        std::mutex mutex;
        QObject* owner = nullptr;
    };

    std::shared_ptr<Lifeline> lifeline_;
    qt::TokenGate gate_;
};

// style property `name`, set only when it changes (live refreshes are frequent).
void restyle(QWidget* widget, const char* name, const QVariant& value);
// A caption for `buddy`: object name "textModelFieldLabel".
QLabel* fieldLabel(const QString& text, QWidget* buddy);
// A wrapped, plain-text line under a field: object name "hooksHint"; `tone` colours it.
WrappedLabel* hint(const QString& text = {});
// A compact button, at least 34 px tall: primary or outlined.
Button* smallButton(const QString& text, bool primary = false);
// The widgets in `container` that Tab stops at, in its focus chain's order. A compound widget (a
// spin box, a text box) counts once.
std::vector<QWidget*> tabStops(QWidget* container);

} // namespace hooks_panel

// The events an item sends, as toggle chips that wrap like tags (object name
// "hooksEventChooser").
// Affinity: GUI thread.
class EventChooser : public QWidget {
    Q_OBJECT
public:
    explicit EventChooser(QWidget* parent = nullptr);

    // The checked event types, in the chooser's order.
    std::vector<std::string> events() const;
    // Checks exactly `events`, without announcing a change.
    void setEvents(const std::vector<std::string>& events);
    // One chip per public event type ("hooksEventChip").
    const std::vector<std::pair<std::string, QPushButton*>>& chips() const { return chips_; }
    QPushButton* chip(const std::string& eventType) const;

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void changed();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    FlowLayout* flow_;
    std::vector<std::pair<std::string, QPushButton*>> chips_;
};

// A short, honest result beside its button: "Sending…", "200 · 120 ms" (object name
// "hooksChip"). An error is a sentence, too long for a chip, so it goes in errorLine(), which
// wraps; the editor places it under the buttons.
// Affinity: GUI thread.
class StatusChip : public QLabel {
    Q_OBJECT
public:
    explicit StatusChip(QWidget* parent = nullptr);
    ~StatusChip() override;

    WrappedLabel* errorLine() const { return errorLine_; }

    // The chip shows `text` in `tone` ("ok", "error", "busy", "muted"); "" hides it. Clears the
    // error line.
    void showResult(const QString& text, const QString& tone);
    // The error line shows `text`; the chip hides.
    void showError(const QString& text);

private:
    QPointer<WrappedLabel> errorLine_;
};

// One line of RecentList.
struct RecentRow {
    QString at;
    QString what;
    QString result;
    bool ok = false;
    friend bool operator==(const RecentRow&, const RecentRow&) = default;
};

// The newest deliveries or runs: time, what, result (object name "hooksRecent").
// Affinity: GUI thread.
class RecentList : public QWidget {
    Q_OBJECT
public:
    explicit RecentList(const QString& emptyText, QWidget* parent = nullptr);

    // The newest RECENT_ROWS of `rows`, newest first; the same rows again change nothing.
    void setRows(const std::vector<RecentRow>& rows);
    // What is shown; nullopt before the first rows.
    const std::optional<std::vector<RecentRow>>& shown() const { return shown_; }

private:
    QString emptyText_;
    std::optional<std::vector<RecentRow>> shown_;
    QVBoxLayout* layout_;
};

// Buttons in a row while they fit, stacked when the column is narrow (object name
// "hooksButtonRow"). A null item is a flexible gap that only exists side by side.
// Affinity: GUI thread.
class AdaptiveButtons : public QWidget {
    Q_OBJECT
public:
    explicit AdaptiveButtons(const std::vector<QWidget*>& items, int spacing = 8,
                             QWidget* parent = nullptr);

    QBoxLayout* boxLayout() const { return layout_; }
    void reflow();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    std::vector<QWidget*> widgets_;
    QBoxLayout* layout_;
    std::vector<int> gaps_;
    QTimer* timer_ = nullptr;
};

// One saved item: last result, name, what it does, Edit, and an on/off switch (object name
// "hooksRow"; Python's _HookRow). The Edit button and the switch sit beside the text, or under
// it in a narrow column.
// Affinity: GUI thread.
class HookRow : public QFrame {
    Q_OBJECT
public:
    HookRow(const QString& itemId, bool showsResults = true, QWidget* parent = nullptr);

    const QString& itemId() const { return itemId_; }
    StatusDot* dot() const { return dot_; }
    ElidingLabel* name() const { return name_; }
    ElidingLabel* detail() const { return detail_; }
    Button* editButton() const { return editButton_; }
    SettingsSwitch* switchButton() const { return switch_; }
    // Whether Edit and the switch are under the text.
    bool isNarrow() const { return narrow_.value_or(false); }

    void showItem(const QString& name, const QString& detail, bool enabled, EngineStatus status);
    void setSelected(bool selected);

Q_SIGNALS:
    void editRequested(const QString& itemId);
    void enabledChanged(const QString& itemId, bool enabled);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void arrange(bool narrow);

    QString itemId_;
    QGridLayout* grid_;
    StatusDot* dot_;
    QWidget* text_;
    ElidingLabel* name_;
    ElidingLabel* detail_;
    Button* editButton_;
    SettingsSwitch* switch_;
    QWidget* controls_;
    std::optional<bool> narrow_;
};

// A saved item as the list shows it.
struct HookEntry {
    std::string id;
    std::string name;
    bool enabled = true;
};

// What a kind of hook says about itself.
struct HookKind {
    // "webhook", "program" or "filter".
    QString noun;
    std::size_t maximum = 0;
    QString emptyTitle;
    QString emptyHint;
    // Rows show a dot for the last delivery or run.
    bool showsResults = true;
};

// A list of one kind of hook with an inline editor (object name "hooksSection"). An unsaved edit
// is never dropped silently: opening another item asks to save it first, refresh() leaves it in
// place, and Settings calls saveDraft() when the page is left.
// Affinity: GUI thread.
class HookSection : public QWidget {
    Q_OBJECT
public:
    ~HookSection() override;

    const HookServices& services() const { return services_; }
    HookEditor* editor() const { return editor_; }
    // "1 / 20", beside the tile's title; hidden with no items. The page puts it in the tile.
    QLabel* countLabel() const { return countLabel_; }
    EmptyState* emptyState() const { return emptyState_; }
    QWidget* listArea() const { return listArea_; }
    QVBoxLayout* listLayout() const { return list_; }
    Button* addButton() const { return addButton_; }
    WrappedLabel* message() const { return message_; }
    AdaptiveButtons* footer() const { return footer_; }
    HookRow* row(const std::string& itemId) const;
    // The rows, by item id.
    const std::map<std::string, HookRow*>& rows() const { return rows_; }
    // The saved items as of the last read, in order.
    const std::vector<HookEntry>& entries() const { return entries_; }

    // The id of the item the open editor edits, "" with none open (or a new one's own id).
    std::string editingId() const;
    bool hasUnsavedChanges() const;

    // Reads the saved items again; an open editor with unsaved changes stays as it is. With
    // `selectId` the editor opens on that item (or closes when it is gone).
    void refresh(const std::optional<std::string>& selectId = std::nullopt);
    // The rows' last results, and the editor's recent list (memory only).
    virtual void refreshLive();
    // Whether the section is polling the engines now (it is while shown).
    bool isPolling() const;

    // Edit a saved item, or start a new one with "". False when the user chose to keep editing
    // another item.
    bool openEditor(const std::string& itemId = {});
    void cancel();
    // Saves the editor's draft; false (with the reason in the editor) when it cannot be saved.
    bool save();
    // Saves an unsaved edit when Settings leaves the page. With `ask`, a draft that cannot be
    // saved is shown to the user; false when they chose to keep editing it.
    bool saveDraft(bool ask = false);
    void deleteItem();
    // The row's switch.
    void setItemEnabled(const std::string& itemId, bool enabled);

Q_SIGNALS:
    // A save, delete or switch changed the saved items.
    void changed();

protected:
    // `editor` is added under the rows; the section takes it over.
    HookSection(const HookServices& services, HookKind kind, HookEditor* editor,
                QWidget* parent = nullptr);

    // What a kind provides.
    virtual std::vector<HookEntry> loadItems(const Json& settings) = 0;
    // Shows the saved item `itemId` in the editor, or a new blank one when there is none.
    virtual void showInEditor(const std::string& itemId, bool saved) = 0;
    // Whether the saved item is no longer what the open editor started from.
    virtual bool changedElsewhere(const std::string& itemId) const = 0;
    // The editor's draft, with the saved item's on/off state: its id and what saves it.
    struct Draft {
        std::string id;
        std::function<void(Json&)> apply;
    };
    virtual Draft prepareDraft() = 0;
    // What saves the item with its on/off state changed; empty for an item not in the list.
    virtual std::function<void(Json&)> enabledApplier(const std::string& itemId, bool enabled) = 0;
    virtual void removeFromSettings(Json& settings, const std::string& itemId) = 0;
    virtual QString detail(const std::string& itemId) = 0;
    virtual EngineStatus status(const std::string&) { return EngineStatus::Unknown; }
    virtual QString deleteQuestion(const HookEntry& item) = 0;
    // Forget anything kept outside settings for a deleted item.
    virtual void afterDelete(const HookEntry&) {}

    // The visible section polls every LIVE_INTERVAL_MS and stops when hidden.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    QVBoxLayout* rootLayout() const { return root_; }
    hooks_panel::EngineRequests& requests() { return requests_; }
    void setMessage(const QString& text, bool error = false);
    Json settingsNow() const;

private:
    void showEditor(const std::string& itemId, bool saved);
    void closeEditor();
    void placeEditor();
    void chainTabOrder();
    bool saveBeforeSwitch();

    HookServices services_;
    HookKind kind_;
    HookEditor* editor_;
    std::vector<HookEntry> entries_;
    std::map<std::string, HookRow*> rows_;
    hooks_panel::EngineRequests requests_;
    QPointer<QLabel> countLabel_;
    QVBoxLayout* root_;
    QWidget* listArea_;
    QVBoxLayout* list_;
    EmptyState* emptyState_;
    Button* addButton_;
    WrappedLabel* message_;
    AdaptiveButtons* footer_;
    QTimer* live_;
};

// Webhooks: signed deliveries to a web address.
// Affinity: GUI thread.
class WebhookSection : public HookSection {
    Q_OBJECT
public:
    explicit WebhookSection(const HookServices& services, QWidget* parent = nullptr);
    WebhookEditor* webhookEditor() const;

protected:
    std::vector<HookEntry> loadItems(const Json& settings) override;
    void showInEditor(const std::string& itemId, bool saved) override;
    bool changedElsewhere(const std::string& itemId) const override;
    Draft prepareDraft() override;
    std::function<void(Json&)> enabledApplier(const std::string& itemId, bool enabled) override;
    void removeFromSettings(Json& settings, const std::string& itemId) override;
    QString detail(const std::string& itemId) override;
    EngineStatus status(const std::string& itemId) override;
    QString deleteQuestion(const HookEntry& item) override;
    void afterDelete(const HookEntry& item) override;

private:
    std::optional<automation::DeliveryRecord> last(const std::string& itemId) const;
    const automation::hooks::Webhook* find(const std::string& itemId) const;

    std::vector<automation::hooks::Webhook> items_;
};

// Programs: run on this computer when something happens.
// Affinity: GUI thread.
class ProgramSection : public HookSection {
    Q_OBJECT
public:
    explicit ProgramSection(const HookServices& services, QWidget* parent = nullptr);
    ProgramEditor* programEditor() const;

protected:
    std::vector<HookEntry> loadItems(const Json& settings) override;
    void showInEditor(const std::string& itemId, bool saved) override;
    bool changedElsewhere(const std::string& itemId) const override;
    Draft prepareDraft() override;
    std::function<void(Json&)> enabledApplier(const std::string& itemId, bool enabled) override;
    void removeFromSettings(Json& settings, const std::string& itemId) override;
    QString detail(const std::string& itemId) override;
    EngineStatus status(const std::string& itemId) override;
    QString deleteQuestion(const HookEntry& item) override;

private:
    std::optional<automation::RunRecord> last(const std::string& itemId) const;
    const automation::hooks::Program* find(const std::string& itemId) const;

    std::vector<automation::hooks::Program> items_;
};

// Filters other programs registered through the live API, with when each ends (object name
// "hooksLeases"; Python's _LeaseList). The countdown updates in place; rows are rebuilt only
// when the leases change.
// Affinity: GUI thread.
class LeaseList : public QWidget {
    Q_OBJECT
public:
    // (id, address, stage, seconds left).
    using Shown = std::tuple<std::string, std::string, std::string, long long>;

    explicit LeaseList(const HookServices& services, QWidget* parent = nullptr);

    // Reads the live leases: only those that have not lapsed, with the seconds left.
    void refresh();
    const std::optional<std::vector<Shown>>& shown() const { return shown_; }
    // The rows now on show, one per live lease.
    const std::vector<QFrame*>& lines() const { return lines_; }
    // The countdown line of a row.
    QLabel* lineDetail(QFrame* line) const;
    QWidget* rows() const { return rows_; }
    WrappedLabel* note() const { return note_; }
    QPushButton* openApi() const { return openApi_; }

Q_SIGNALS:
    void openApiRequested();

private:
    HookServices services_;
    std::optional<std::vector<Shown>> shown_;
    std::vector<QFrame*> lines_;
    std::vector<std::tuple<std::string, std::string, std::string>> lineLeases_;
    std::map<QFrame*, QLabel*> details_;
    QWidget* rows_;
    QVBoxLayout* rowsLayout_;
    WrappedLabel* note_;
    QPushButton* openApi_;
};

// Text filters: a web address or a program changes the words before or after AI cleanup.
// Affinity: GUI thread.
class FilterSection : public HookSection {
    Q_OBJECT
public:
    explicit FilterSection(const HookServices& services, QWidget* parent = nullptr);
    FilterEditor* filterEditor() const;
    LeaseList* leases() const { return leases_; }

    void refreshLive() override;

protected:
    std::vector<HookEntry> loadItems(const Json& settings) override;
    void showInEditor(const std::string& itemId, bool saved) override;
    bool changedElsewhere(const std::string& itemId) const override;
    Draft prepareDraft() override;
    std::function<void(Json&)> enabledApplier(const std::string& itemId, bool enabled) override;
    void removeFromSettings(Json& settings, const std::string& itemId) override;
    QString detail(const std::string& itemId) override;
    QString deleteQuestion(const HookEntry& item) override;

private:
    const automation::hooks::TextFilter* find(const std::string& itemId) const;

    std::vector<automation::hooks::TextFilter> items_;
    LeaseList* leases_ = nullptr;
};

} // namespace fv
