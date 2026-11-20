#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QFrame>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QWidget>

#include "base/json.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/no_wheel.h"

class QBoxLayout;
class QLabel;
class QLineEdit;
class QVariantAnimation;

// Fields the Actions page shares between the Agent hotkey card and the action editor
// (ui_qt/widgets/action_fields.py). The pieces are what the page's theme block selects on: the
// object names and tileId values are OpenWhisper's.
namespace fv {

class AgentMark;
class Button;
class ElidingComboBox;
class ElidingLabel;
class ProfileHotkeyInput;
class Reflow;
class SegmentedBar;
class SettingsSwitch;
class StatusDot;
class TileBase;
class WrappedLabel;

namespace action_fields {

// One agent's arrival once a scan finds it: the mark warms to its colour (OutCubic).
inline constexpr int kArrivalMs = 380;
// The widest the choosers get at 100 % font scale: equal cells stretched across a wide card read
// as one heavy bar.
inline constexpr int kChooserMaxWidth = 660;
inline constexpr int kBarMaxWidth = 460;
inline constexpr int kComboMaxWidth = 340;
inline constexpr int kShortcutMaxWidth = 380;
// A tile narrower than this (at 100 %) gives its body the full width.
inline constexpr int kNarrowTileWidth = 360;

// Agents with no desktop app to open: a session always opens in a terminal (AGENTS_WITHOUT_APP).
bool agentHasNoApp(std::string_view agentId);

// (widget, width at 100 %) pairs.
using WidthCaps = std::vector<std::pair<QWidget*, int>>;
// Holds each widget to its width at the current font scale (round half to even). Widgets that
// carry caps re-apply them on font and style changes.
void capWidths(const WidthCaps& caps);

// A field's caption (object name "textModelFieldLabel"), the buddy of `buddy` when given.
QLabel* fieldLabel(const QString& text, QWidget* buddy = nullptr);
// A one-line hint under a field (object name "actionHint", plain text); the `tone` property
// ("ok", "warning", "error") colours it.
WrappedLabel* hintLabel(const QString& text = {});
// Sits a tile's icon in a soft rounded square of its own colour, like the MCP cards: `side` px
// square, an 18 px pixmap, and the iconTone the file name's suffix gives ("-purple", "-green",
// "-gray", "-blue"; blue otherwise). Once per tile, the body is lined up with the title (side +
// 12 px, or 0 without `indentBody`) and gets the full width while the tile is narrower than
// kNarrowTileWidth at the font scale.
void tintIcon(TileBase* tile, const QString& icon, int side = 32, bool indentBody = true);
// Gives a text box the Settings field look (tileId "actionInput").
template <class W> W* inputField(W* widget);
void markInputField(QWidget* widget);
// A flat accent link (object name "actionLink"), like the other Settings pages' "Open …" links.
QPushButton* pageLink(const QString& text, const QString& tooltip = {});
// A transparent row (tileId "actionRow") of widgets, so it never paints the page colour inside a
// card; a null entry is a stretch.
QWidget* plainRow(const std::vector<QWidget*>& widgets, int spacing = 8, bool stretchLast = false);
// A hairline (object name "actionDivider") and a small heading ("actionSectionLabel") that open
// a part of a long card.
QWidget* sectionHeading(const QString& text);
// A transparent column of widgets: a label, its field and its hint.
QWidget* stack(const std::vector<QWidget*>& widgets, int spacing = 6);

// The pickers PathField opens (choose_folder, choose_program): a full path, or "" when the
// person cancelled. Tests replace them; the defaults are Qt's file dialogs.
using PathChooser =
    std::function<QString(QWidget* parent, const QString& title, const QString& start)>;
PathChooser& chooseFolder();
PathChooser& chooseProgram();

} // namespace action_fields

// Buttons at their own width in one row, wrapping into more rows when narrow. Its size hint is
// always the one-row width, so whoever places the row (a Reflow, a stretch) decides the same way
// however it is wrapped right now. Affinity: GUI thread.
class ActionButtons : public ButtonRow {
    Q_OBJECT
public:
    explicit ActionButtons(std::vector<QPushButton*> buttons, int spacing = 8,
                           QWidget* parent = nullptr);
    QSize sizeHint() const override;

private:
    int spacing_;
};

namespace action_fields {

enum class RowAlign { Left, Right, None };
struct ButtonRowBox {
    QWidget* box;
    ActionButtons* buttons;
};
// `buttons` side by side at their own (compact) width, wrapping when the column is narrow, in a
// transparent box that puts the slack on the right (Left) or the left (Right).
ButtonRowBox buttonRow(const std::vector<Button*>& buttons, RowAlign align = RowAlign::Left);

} // namespace action_fields

// Two columns side by side while each gets `minColumn` px (at 100 %), else stacked.
// Affinity: GUI thread.
class Columns : public QWidget {
    Q_OBJECT
public:
    Columns(QWidget* left, QWidget* right, int minColumn = 260, int spacing = 18,
            QWidget* parent = nullptr);
    bool isStacked() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int minColumn_;
    QBoxLayout* layout_;
};

// A dot, one honest sentence about the state, and an optional link to fix it (object name
// "actionStatusLine", property `state`). Affinity: GUI thread.
class StatusLine : public QFrame {
    Q_OBJECT
public:
    static constexpr const char* kReady = "ready";
    static constexpr const char* kAttention = "attention";
    static constexpr const char* kOff = "off";

    explicit StatusLine(const QString& linkText = {}, const QString& linkTip = {},
                        QWidget* parent = nullptr);

    StatusDot* dot() const { return dot_; }
    WrappedLabel* message() const { return message_; }
    QPushButton* link() const { return link_; }
    const QString& state() const { return state_; }

    // show_state: the state's dot and property, the sentence, and whether the link shows.
    void showState(const QString& state, const QString& message, bool link);
    // ready, attention or off (any other value shows the off dot).
    void setState(const QString& state);
    void setMessage(const QString& text);
    // The link's text and tooltip; an empty text hides it.
    void setLink(const QString& text, const QString& tooltip = {});
    void setLinkVisible(bool visible);

Q_SIGNALS:
    void linkActivated();

private:
    StatusDot* dot_;
    WrappedLabel* message_;
    QPushButton* link_;
    QString state_ = QString::fromLatin1(kOff);
};

// A full path, chosen with a picker: a folder or a program (tileId "actionPathField"). The path
// line is read-only; Choose… opens action_fields::chooseFolder or chooseProgram from the
// current path, and the choice is normalized (native separators, "." and ".." resolved); Clear
// shows only with a path. The buttons move under the path when the column is narrow.
// Affinity: GUI thread.
class PathField : public QWidget {
    Q_OBJECT
public:
    PathField(const QString& title, bool folder, const QString& placeholder,
              QWidget* parent = nullptr);

    QLineEdit* line() const { return line_; }
    Button* chooseButton() const { return choose_; }
    Button* clearButton() const { return clear_; }
    QWidget* buttons() const { return buttons_; }

    QString path() const;
    // Shows `path` from its start, as its tooltip too; emits nothing.
    void setPath(const QString& path);

Q_SIGNALS:
    // The person chose or cleared a different path.
    void changed(const QString& path);

private:
    void set(const QString& path);
    void choose();

    QString title_;
    bool folder_;
    QLineEdit* line_;
    Button* choose_;
    Button* clear_;
    QWidget* buttons_;
};

// A shortcut box with Clear; the owner checks conflicts and saves (tileId
// "actionShortcutField"). Clear emits captured("") (as a capture of nothing does) and then
// cleared(). Affinity: GUI thread.
class ShortcutField : public QWidget {
    Q_OBJECT
public:
    explicit ShortcutField(const QString& name, QWidget* parent = nullptr);

    ProfileHotkeyInput* input() const { return input_; }
    Button* clearButton() const { return clear_; }
    Reflow* row() const { return row_; }

    QString hotkey() const;
    void setHotkey(const QString& hotkey);
    // Puts back the shortcut from before the last capture (it was refused).
    void restore();
    void beginCapture();
    void cancelCapture();

Q_SIGNALS:
    void captured(const QString& hotkey);
    void captureChanged(bool capturing);
    void cleared();

protected:
    void changeEvent(QEvent* event) override;

private:
    ProfileHotkeyInput* input_;
    Button* clear_;
    Reflow* row_;
    action_fields::WidthCaps caps_;
    QString before_;
};

// Where a standard shortcut is saved: the Settings window's hotkey path (the dialog contract
// CommandShortcutField uses).
class StandardShortcutHost {
public:
    virtual ~StandardShortcutHost() = default;
    // The settings the page shows (_settings_snapshot).
    virtual Json settings() = 0;
    // The standard shortcuts the Hotkeys page holds (current_hotkeys); empty when it holds none.
    virtual Json currentHotkeys() = 0;
    // Saves one standard shortcut through the Hotkeys page's path: "" once saved, else why not.
    virtual QString setStandardHotkey(const QString& action, const QString& hotkey) = 0;
    // Pauses global hotkeys while a field captures a shortcut, and resumes them.
    virtual void setHotkeyCaptureSuspended(bool suspended) = 0;

protected:
    StandardShortcutHost() = default;
    StandardShortcutHost(const StandardShortcutHost&) = default;
    StandardShortcutHost& operator=(const StandardShortcutHost&) = default;
};

// The Route by voice shortcut (tileId "actionRouteShortcut"), saved like any standard one
// through host.setStandardHotkey("route_record", ...), which owns conflicts; a refusal shows
// under the box. The parts of command_settings.py's CommandShortcutField it uses: the capture
// box (160 to 240 px), Clear (at least 64 x 34), the error line, and the buttons going under the
// box when the field is narrower than the box's floor (at the font scale) beside them, with size
// hints that never depend on that arrangement. Capturing pauses the app's hotkeys.
// Affinity: GUI thread.
class RouteShortcutField : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kAction = "route_record";
    static constexpr int kInputMinWidth = 160;
    static constexpr int kInputMaxWidth = 240;

    // `host` outlives the field.
    explicit RouteShortcutField(StandardShortcutHost& host, QWidget* parent = nullptr);

    ProfileHotkeyInput* input() const { return input_; }
    Button* clearButton() const { return clear_; }
    WrappedLabel* message() const { return message_; }
    QBoxLayout* row() const { return row_; }

    // The saved shortcut: the host's current standard shortcuts, else the saved ones.
    QString hotkey() const;
    // Shows the saved shortcut (unless a capture is under way) and enables Clear only with one.
    void sync();
    void beginCapture();
    void cancelCapture();
    void showError(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // A shortcut was captured, before it is saved.
    void captured(const QString& hotkey);
    void captureChanged(bool capturing);
    // Clear was pressed, before the empty shortcut is saved.
    void cleared();
    // A new shortcut was saved, or the old one cleared.
    void changed();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void save(const QString& hotkey);
    int sideBySideWidth() const;

    StandardShortcutHost& host_;
    QBoxLayout* row_;
    ProfileHotkeyInput* input_;
    Button* clear_;
    WrappedLabel* message_;
};

// A title, one line of detail and a switch; the whole row toggles it (object name
// "actionSwitchRow"). Affinity: GUI thread.
class SwitchRow : public QFrame {
    Q_OBJECT
public:
    SwitchRow(const QString& title, const QString& detail, QWidget* parent = nullptr);

    WrappedLabel* titleLabel() const { return title_; }
    WrappedLabel* detailLabel() const { return detail_; }
    SettingsSwitch* switchButton() const { return switch_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    WrappedLabel* title_;
    WrappedLabel* detail_;
    SettingsSwitch* switch_;
};

// Where an answer, reply or output goes (object name "actionOutputCombo"): the Output values in
// order with their labels, "Nowhere" only with `allowNone`. Its width floor is 110 px; its width
// is capped at kComboMaxWidth at the font scale. Affinity: GUI thread.
class OutputCombo : public ElidingComboBox {
    Q_OBJECT
public:
    static constexpr int kMinimumWidth = 110;

    explicit OutputCombo(const QString& name, bool allowNone = true, QWidget* parent = nullptr);

    // The chosen Output value ("none" when nothing is chosen).
    QString output() const;
    // Chooses `value`, or the first item when it is not listed.
    void setOutput(const QString& value);

    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;
};

// One installed agent: its mark, its name, and whether the scan found it (object name
// "actionAgentChoice", checkable). Affinity: GUI thread.
class AgentChoice : public QPushButton {
    Q_OBJECT
public:
    explicit AgentChoice(const QString& agentId, QWidget* parent = nullptr);

    const QString& agentId() const { return agentId_; }
    AgentMark* mark() const { return mark_; }
    ElidingLabel* nameLabel() const { return name_; }
    ElidingLabel* detailLabel() const { return detail_; }
    // nullopt while checking.
    std::optional<bool> found() const { return found_; }
    // "Checking…", "Found" or "Not found"; once found, the mark warms to the agent's colour,
    // over kArrivalMs with `animate`.
    void setFound(std::optional<bool> found, bool animate = false);
    QVariantAnimation* arrival() const { return arrival_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString agentId_;
    AgentMark* mark_;
    ElidingLabel* name_;
    ElidingLabel* detail_;
    std::optional<bool> found_;
    QVariantAnimation* arrival_;
};

// Claude Code, Codex and OpenCode; only the ones found can be chosen (tileId
// "actionAgentChooser"). Side by side while their names fit, else one above the other. The
// chosen agent stays shown, checked, even when it is missing, so a card never claims a different
// agent than the one saved. Affinity: GUI thread.
class AgentChooser : public QWidget {
    Q_OBJECT
public:
    explicit AgentChooser(QWidget* parent = nullptr);

    // In AGENTS order.
    const std::vector<AgentChoice*>& choices() const { return choices_; }
    AgentChoice* choice(const QString& agentId) const;
    QString current() const { return current_; }
    bool isStacked() const;
    // An id that is not an agent chooses the first.
    void setCurrent(const QString& agentId);
    // nullopt while looking; otherwise {agent id: found} for every agent.
    std::optional<std::map<std::string, bool>> available() const { return available_; }
    // `scanned` false: still looking (every choice reads "Checking…"); otherwise `available`
    // says which were found (a missing id is not found).
    void setAvailable(const std::map<std::string, bool>& available, bool scanned,
                      bool animate = false);
    // The found agents, in AGENTS order.
    std::vector<QString> found() const;

Q_SIGNALS:
    // The person chose a different agent.
    void chosen(const QString& agentId);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void sync();
    void onClicked(const QString& agentId);

    std::vector<AgentChoice*> choices_;
    QBoxLayout* layout_;
    QString current_;
    std::optional<std::map<std::string, bool>> available_;
};

// What an agent action does: which agent, a session or an answer, where, and with what (tileId
// "actionAgentFields"). params() keeps any parameter the form does not show. A quick answer's
// output "none" shows as the Scratchpad (the answer always goes somewhere). Choosing OpenCode
// while "Opens in" is App switches it to Auto. Programmatic fills never emit changed().
// Affinity: GUI thread.
class AgentFields : public QWidget {
    Q_OBJECT
public:
    explicit AgentFields(QWidget* parent = nullptr);

    AgentChooser* chooser() const { return chooser_; }
    WrappedLabel* agentNote() const { return agentNote_; }
    QWidget* details() const { return details_; }
    SegmentedBar* modeBar() const { return modeBar_; }
    WrappedLabel* modeHint() const { return modeHint_; }
    QWidget* sessionBox() const { return sessionBox_; }
    SegmentedBar* surfaceBar() const { return surfaceBar_; }
    WrappedLabel* surfaceHint() const { return surfaceHint_; }
    SwitchRow* newFolderRow() const { return newFolderRow_; }
    QWidget* folderBox() const { return folderBox_; }
    PathField* folder() const { return folder_; }
    QWidget* answerBox() const { return answerBox_; }
    OutputCombo* outputCombo() const { return outputCombo_; }
    QLineEdit* modelEdit() const { return modelEdit_; }
    SwitchRow* selectionRow() const { return selectionRow_; }
    QWidget* selectionNote() const { return selectionNote_; }
    WrappedLabel* selectionHint() const { return selectionHint_; }
    QPushButton* stylesLink() const { return stylesLink_; }

    // The agent parameters: the unknown ones it was given first, then agent, mode, surface,
    // folder, new_folder, include_selection, model (stripped) and output.
    Json params() const;
    // Fills the form from `params` over the agent kind's defaults, without emitting changed().
    void setParams(const Json& params);
    void setAvailable(const std::map<std::string, bool>& available, bool scanned,
                      bool animate = false);
    // Whether "Know which app I'm dictating into" is on; when off, an included selection warns.
    void setAppAwareness(bool on);
    // Only the agents, for a card that invites setup and says itself what was found.
    void setCompact(bool compact);

Q_SIGNALS:
    void changed();
    void stylesRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void sync();
    void emitChanged();
    void onAgentChosen(const QString& agentId);
    void onModelEdited();

    bool loading_ = false;
    Json extra_ = Json::object();
    bool appAwareness_ = true;
    bool compact_ = false;
    AgentChooser* chooser_;
    WrappedLabel* agentNote_;
    QWidget* details_;
    SegmentedBar* modeBar_;
    WrappedLabel* modeHint_;
    QWidget* sessionBox_;
    SegmentedBar* surfaceBar_;
    WrappedLabel* surfaceHint_;
    SwitchRow* newFolderRow_;
    QWidget* folderBox_;
    PathField* folder_;
    QWidget* answerBox_;
    OutputCombo* outputCombo_;
    QLineEdit* modelEdit_;
    SwitchRow* selectionRow_;
    QWidget* selectionNote_;
    WrappedLabel* selectionHint_;
    QPushButton* stylesLink_;
    action_fields::WidthCaps caps_;
};

// How the words are prepared: cleaned up, raw, a cleanup profile, or a transform (tileId
// "actionShapingField"). Profile and Transform are disabled, with a tooltip, while there are
// none; a chosen profile or transform that no longer exists shows as "… no longer exists"
// instead of quietly becoming another. Affinity: GUI thread.
class ShapingField : public QWidget {
    Q_OBJECT
public:
    // (id, name) pairs, in order.
    using Items = std::vector<std::pair<QString, QString>>;

    explicit ShapingField(QWidget* parent = nullptr);

    SegmentedBar* bar() const { return bar_; }
    ElidingComboBox* combo() const { return combo_; }
    WrappedLabel* hint() const { return hint_; }

    // The saved cleanup profiles and transforms of `settings`.
    void setChoices(const Json& settings);
    // Keeps the current value.
    void setChoices(Items profiles, Items transforms);
    // "cleanup", "raw", "profile" or "transform".
    QString kind() const;
    // "cleanup", "raw", "profile:<id>" or "transform:<id>"; a profile or transform with nothing
    // chosen reads as "cleanup".
    QString value() const;
    void setValue(const QString& shaping);

Q_SIGNALS:
    void changed();

protected:
    void changeEvent(QEvent* event) override;

private:
    void fillCombo(const QString& kind, const QString& selected = {});
    void onKindChanged();
    void sync();

    SegmentedBar* bar_;
    ElidingComboBox* combo_;
    WrappedLabel* hint_;
    Items profiles_;
    Items transforms_;
    bool loading_ = false;
    action_fields::WidthCaps caps_;
};

template <class W> W* action_fields::inputField(W* widget) {
    markInputField(widget);
    return widget;
}

} // namespace fv
