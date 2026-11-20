#pragma once

#include <vector>

#include <QFrame>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "ui_widgets/buttons.h"

class QBoxLayout;
class QHBoxLayout;
class QToolButton;
class QVBoxLayout;

namespace fv {

class SettingsSwitch;
class WrappedLabel;

// The building blocks of Settings > Automate > API & MCP (ui_qt/dialogs/settings_mcp.py: the
// private classes at its top and the page's _card, _label and _detail helpers). OpenWhisper's
// private names (_ActionButton, ...) carry an Mcp prefix here, so that no later class of a
// generic name in namespace fv clashes with them.

// A compact action button that keeps the height of its styled text, even inside a compressed
// settings card (_ActionButton). Call setText and setBaseMinimumSize through McpActionButton.
// Affinity: GUI thread.
class McpActionButton : public Button {
    Q_OBJECT
public:
    explicit McpActionButton(const QString& text = {}, QWidget* parent = nullptr);

    void setText(const QString& text);
    void setBaseMinimumSize(int width, int height);

protected:
    void changeEvent(QEvent* event) override;

private:
    // Button's own fit to the label, then a height that is never below what the styled text asks
    // for. A change of font or style fits again, width included: the label is measured in the
    // style it ends up in.
    void refit();
    void fitHeight();

    int baseMinWidth_ = 140;
    int baseMinHeight_ = 44;
};

// Side by side while the widgets fit, stacked when they do not (object name "mcpAdaptiveRow").
// A null widget is a flexible gap that only exists side by side.
// Affinity: GUI thread.
class McpAdaptiveRow : public QWidget {
    Q_OBJECT
public:
    struct Item {
        QWidget* widget = nullptr; // null: a gap
        int stretch = 0;
    };

    explicit McpAdaptiveRow(const std::vector<Item>& items, int spacing = 8,
                            QWidget* parent = nullptr);

    QBoxLayout* boxLayout() const { return layout_; }
    // Arranges the row for its current width and the widgets' size hints. The page calls it when
    // a button's text changed, which no event of this row says.
    void reflow();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    std::vector<QWidget*> widgets_;
    std::vector<int> stretches_;
    QBoxLayout* layout_ = nullptr;
    QTimer* timer_ = nullptr;
};

// A collapsible section (_Disclosure, object names "mcpDisclosure", "mcpDisclosureToggle" and
// "mcpDisclosureBody"). `inline` leaves the toggle for the caller to place beside other actions;
// the section then occupies no space until it is expanded. `trailing` puts a short status label
// beside the toggle. Affinity: GUI thread.
class McpDisclosure : public QWidget {
    Q_OBJECT
public:
    explicit McpDisclosure(const QString& title, bool inlineToggle = false,
                           QWidget* trailing = nullptr, QWidget* parent = nullptr);

    QToolButton* toggle() const { return toggle_; }
    QWidget* body() const { return body_; }
    QVBoxLayout* bodyLayout() const { return bodyLayout_; }

private:
    void expand(bool expanded);

    bool inline_ = false;
    QToolButton* toggle_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
};

// A read-only field and its copy button; the button stacks under the field when the field would
// get under 180 px (at the UI font scale) beside it (_CopyField, object name "mcpCopyField").
// Affinity: GUI thread.
class McpCopyField : public QWidget {
    Q_OBJECT
public:
    McpCopyField(QWidget* field, QWidget* button, QWidget* parent = nullptr);

    QWidget* field() const { return field_; }
    QWidget* button() const { return button_; }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void reflow();

    QWidget* field_ = nullptr;
    QWidget* button_ = nullptr;
    QBoxLayout* layout_ = nullptr;
    QTimer* timer_ = nullptr;
};

// An icon, a title and a description with an on/off switch, in the permissions lists
// (_PermissionRow, object name "mcpPermissionRow"). A click anywhere on the row toggles an
// enabled switch. Affinity: GUI thread.
class McpPermissionRow : public QFrame {
    Q_OBJECT
public:
    McpPermissionRow(const QString& title, const QString& description, const QString& icon,
                     QWidget* parent = nullptr);

    SettingsSwitch* checkbox() const { return checkbox_; }
    QHBoxLayout* rowLayout() const { return layout_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    QHBoxLayout* layout_ = nullptr;
    SettingsSwitch* checkbox_ = nullptr;
};

// A wrapped label with an object name (the page's _label); mcpDetail is the settings tile's
// description style (_detail).
WrappedLabel* mcpLabel(const QString& text, const char* objectName);
WrappedLabel* mcpDetail(const QString& text);

// A card (object name "mcpCard") headed by a tile icon, a bold title and a description, and its
// layout for the body (the page's _card).
struct McpCard {
    QFrame* frame = nullptr;
    QVBoxLayout* layout = nullptr;
    // The heading row: icon, then the title and description; a chip goes at its end.
    QHBoxLayout* header = nullptr;
};
McpCard makeMcpCard(const QString& title, const QString& description, const QString& icon);

} // namespace fv
