#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QFrame>
#include <QString>

#include "automation/hooks_model.h"
#include "ui_automation/hooks_panel.h"

class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;

namespace fv {

class NoWheelSpinBox;
class SegmentedBar;

// The fields for one item, with Save, Cancel and Delete (ui_qt/widgets/hooks_panel.py _Editor,
// object name "hooksEditor"). `source` is the item being edited (saved, or the blank a new one
// starts from); it supplies the id and the on/off state, which the editor does not show.
// `baseline` is the source as the fields read it back, so an untouched editor never reads as
// unsaved. One editor serves every item, so a test or a secret still on its way when it moves to
// another item (or saves, or closes) is dropped rather than shown here.
// Affinity: GUI thread.
class HookEditor : public QFrame {
    Q_OBJECT
public:
    // Set by the section: saves the draft first and returns whether it did.
    std::function<bool()> saveFirst;

    const QString& noun() const { return noun_; }
    QLineEdit* nameEdit() const { return nameEdit_; }
    WrappedLabel* message() const { return message_; }
    Button* deleteButton() const { return deleteButton_; }
    Button* cancelButton() const { return cancelButton_; }
    Button* saveButton() const { return saveButton_; }
    AdaptiveButtons* footer() const { return footer_; }
    // The widgets Tab stops at, in the editor's own order, read once it is built.
    const std::vector<QWidget*>& tabStops() const { return tabStops_; }
    // Whether the shown item is saved (a new one is not).
    bool isSaved() const { return saved_; }

    // The shown item's id; "" with none.
    virtual QString itemId() const = 0;
    virtual bool hasUnsavedChanges() const = 0;
    // The row's switch changed the saved item; keep the draft in step.
    virtual void setSourceEnabled(bool enabled) = 0;
    // Re-reads what the engine remembers about this item (memory only).
    virtual void refreshLive() {}
    // Shows nothing: no item, no pending answers.
    virtual void clear() = 0;

    // The line above the footer: "Saved.", or why the item cannot be saved ("error").
    void setMessage(const QString& text, bool error = false);

Q_SIGNALS:
    void saveRequested();
    void cancelRequested();
    void deleteRequested();

protected:
    HookEditor(const QString& noun, const QString& namePlaceholder, const HookServices& services,
               QWidget* parent = nullptr);

    // The editor's column: the name is in it; a kind adds its fields, then calls finish().
    QVBoxLayout* column() const { return column_; }
    // Adds the message and the footer, reads the tab stops and watches the name.
    void finish();
    // "Saved." stays true only until the fields differ from what was saved.
    void draftEdited();
    // The start of showing an item: nothing on its way, buttons and results idle.
    void begin(bool saved);
    // The end of showing an item: Delete for a saved one, no message, the engine's records.
    void end();
    // Buttons and results as they are with nothing on its way.
    virtual void showIdle() {}
    void startOver();
    // Forgets the answers on their way and shows nothing.
    void forgetShown();

    QLabel* sectionLabel(const QString& text) const;
    QFrame* divider() const;
    hooks_panel::EngineRequests& requests() { return requests_; }
    const HookServices& services() const { return services_; }

private:
    HookServices services_;
    QString noun_;
    hooks_panel::EngineRequests requests_;
    QVBoxLayout* column_;
    QLineEdit* nameEdit_;
    WrappedLabel* message_;
    Button* deleteButton_;
    Button* cancelButton_;
    Button* saveButton_;
    AdaptiveButtons* footer_;
    std::vector<QWidget*> tabStops_;
    bool saved_ = false;
};

// Program path, a Choose button and arguments, shared by programs and filters.
// Affinity: GUI thread.
class ProgramFields {
public:
    // The program line, whose label/row/arguments hide for a web address filter.
    QLineEdit* programEdit() const { return programEdit_; }
    Button* chooseButton() const { return chooseButton_; }
    QWidget* programLabel() const { return programLabel_; }
    AdaptiveButtons* programRow() const { return programRow_; }
    QPlainTextEdit* argsEdit() const { return argsEdit_; }
    QWidget* argsLabel() const { return argsLabel_; }

protected:
    ProgramFields() = default;
    ~ProgramFields() = default;
    ProgramFields(const ProgramFields&) = delete;
    ProgramFields& operator=(const ProgramFields&) = delete;

    // `noun` is "Hook" or "Filter": the field's accessible name. `host` owns the Choose button's
    // connection.
    void buildProgramFields(QWidget* host, QVBoxLayout* layout, const QString& noun);
    // The arguments one per line, blank lines left out.
    std::vector<std::string> args() const;
    void chooseProgram(QWidget* parent);

    QLineEdit* programEdit_ = nullptr;
    Button* chooseButton_ = nullptr;
    QLabel* programLabel_ = nullptr;
    AdaptiveButtons* programRow_ = nullptr;
    QPlainTextEdit* argsEdit_ = nullptr;
    QLabel* argsLabel_ = nullptr;
};

// A webhook: its address, events, signing secret, a test and recent deliveries.
// Affinity: GUI thread.
class WebhookEditor : public HookEditor {
    Q_OBJECT
public:
    explicit WebhookEditor(const HookServices& services, QWidget* parent = nullptr);

    QLineEdit* urlEdit() const { return urlEdit_; }
    WrappedLabel* urlNote() const { return urlNote_; }
    EventChooser* events() const { return events_; }
    QWidget* extras() const { return extras_; }
    Button* copySecretButton() const { return copySecret_; }
    Button* rotateSecretButton() const { return rotateSecret_; }
    StatusChip* secretChip() const { return secretChip_; }
    Button* testButton() const { return testButton_; }
    StatusChip* testChip() const { return testChip_; }
    RecentList* recent() const { return recent_; }

    // show_item.
    void showItem(const automation::hooks::Webhook& hook, bool saved);
    // The fields as a webhook; requires an item shown.
    automation::hooks::Webhook draft() const;
    const std::optional<automation::hooks::Webhook>& source() const { return source_; }

    QString itemId() const override;
    bool hasUnsavedChanges() const override;
    void setSourceEnabled(bool enabled) override;
    void refreshLive() override;
    void clear() override;

    // Sends a test event, after saving a changed draft.
    void sendTest();

protected:
    void showIdle() override;

private:
    void updateNote();
    void copySecret();
    void rotateSecret();

    QLineEdit* urlEdit_;
    WrappedLabel* urlNote_;
    EventChooser* events_;
    QWidget* extras_;
    Button* copySecret_;
    Button* rotateSecret_;
    StatusChip* secretChip_;
    Button* testButton_;
    StatusChip* testChip_;
    RecentList* recent_;
    std::optional<automation::hooks::Webhook> source_;
    std::optional<automation::hooks::Webhook> baseline_;
};

// A program: its path, arguments, events and time limit, a test and recent runs.
// Affinity: GUI thread.
class ProgramEditor : public HookEditor, public ProgramFields {
    Q_OBJECT
public:
    explicit ProgramEditor(const HookServices& services, QWidget* parent = nullptr);

    EventChooser* events() const { return events_; }
    NoWheelSpinBox* timeout() const { return timeout_; }
    QWidget* extras() const { return extras_; }
    Button* testButton() const { return testButton_; }
    StatusChip* testChip() const { return testChip_; }
    RecentList* recent() const { return recent_; }

    void showItem(const automation::hooks::Program& program, bool saved);
    automation::hooks::Program draft() const;
    const std::optional<automation::hooks::Program>& source() const { return source_; }

    QString itemId() const override;
    bool hasUnsavedChanges() const override;
    void setSourceEnabled(bool enabled) override;
    void refreshLive() override;
    void clear() override;

    // Runs the program with a test event, after saving a changed draft.
    void runTest();

protected:
    void showIdle() override;

private:
    EventChooser* events_;
    NoWheelSpinBox* timeout_;
    QWidget* extras_;
    Button* testButton_;
    StatusChip* testChip_;
    RecentList* recent_;
    std::optional<automation::hooks::Program> source_;
    std::optional<automation::hooks::Program> baseline_;
};

// A text filter: a web address or a program, when it runs and its time limit.
// Affinity: GUI thread.
class FilterEditor : public HookEditor, public ProgramFields {
    Q_OBJECT
public:
    explicit FilterEditor(const HookServices& services, QWidget* parent = nullptr);

    SegmentedBar* kindBar() const { return kindBar_; }
    QLineEdit* urlEdit() const { return urlEdit_; }
    QWidget* urlLabel() const { return urlLabel_; }
    WrappedLabel* urlNote() const { return urlNote_; }
    SegmentedBar* stageBar() const { return stageBar_; }
    NoWheelSpinBox* timeout() const { return timeout_; }
    WrappedLabel* replyHint() const { return replyHint_; }

    void showItem(const automation::hooks::TextFilter& filter, bool saved);
    automation::hooks::TextFilter draft() const;
    const std::optional<automation::hooks::TextFilter>& source() const { return source_; }

    QString itemId() const override;
    bool hasUnsavedChanges() const override;
    void setSourceEnabled(bool enabled) override;
    void clear() override;

private:
    // The chosen kind: "http" or "program".
    std::string kind() const;
    void kindChanged();
    void updateNote();

    SegmentedBar* kindBar_;
    QLineEdit* urlEdit_;
    QLabel* urlLabel_;
    WrappedLabel* urlNote_;
    SegmentedBar* stageBar_;
    NoWheelSpinBox* timeout_;
    WrappedLabel* replyHint_;
    std::optional<automation::hooks::TextFilter> source_;
    std::optional<automation::hooks::TextFilter> baseline_;
};

} // namespace fv
