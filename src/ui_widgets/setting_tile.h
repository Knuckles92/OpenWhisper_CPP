#pragma once

#include <vector>

#include <QFrame>
#include <QIcon>
#include <QString>

class QHBoxLayout;
class QLabel;
class QLayout;
class QVBoxLayout;

namespace fv {

class SettingsSwitch;
class WrappedLabel;

// Rounded tiles that pair one setting with its explanation.
//
// TileBase (object name "settingsTile") is an icon, title and description row with a trailing
// slot, and an indented body under it for controls the setting owns, so disabling the tile
// disables everything the setting depends on. The theme selects on its "kind" property:
// "toggle" (SettingTile), "field" (FieldTile, InfoTile), or "notice" and "intro", which callers
// set through ui::setStyleProperty. Affinity: GUI thread.
class TileBase : public QFrame {
    Q_OBJECT
public:
    static constexpr int kIconSize = 20;
    // The body's indent under the title: the icon plus the row spacing.
    static constexpr int kBodyIndent = kIconSize + 12;

    TileBase(const QString& title, const QString& description, const QIcon& icon = {},
             QWidget* parent = nullptr);

    QLabel* iconLabel() const { return iconLabel_; }
    WrappedLabel* titleLabel() const { return titleLabel_; }
    WrappedLabel* descriptionLabel() const { return descriptionLabel_; }
    // Hidden until something is added to it.
    QWidget* body() const { return body_; }
    QVBoxLayout* bodyLayout() const { return bodyLayout_; }

    void setDescription(const QString& text);
    // Puts a small pill, such as "Experimental", right after the title.
    QLabel* addTitleBadge(const QString& text,
                          const QString& objectName = QStringLiteral("meetingBetaBadge"));
    // Beside the text, vertically centred.
    void addTrailing(QWidget* widget);
    void addBody(QWidget* widget);
    void addBodyLayout(QLayout* layout);
    // Indents the body under the title (the default), or gives it the full width: a tile whose
    // body is a whole panel needs the room more than the alignment, most of all in a narrow
    // tiled window.
    void setBodyIndented(bool indented);
    // Puts buttons in a left-aligned row under the text. Notice banners all place their action
    // here, so it fits a narrow window and every banner reads the same way.
    void addActions(const std::vector<QWidget*>& buttons);

protected:
    QHBoxLayout* row() const { return row_; }

private:
    QHBoxLayout* row_;
    QLabel* iconLabel_;
    WrappedLabel* titleLabel_;
    WrappedLabel* descriptionLabel_;
    QWidget* body_;
    QVBoxLayout* bodyLayout_;
};

// Tile for an on/off setting; a left press anywhere on the card toggles its switch. The
// "checked" property mirrors the switch for callers and tests; the theme leaves the card's
// colour alone, so only the switch shows on and off. Affinity: GUI thread.
class SettingTile : public TileBase {
    Q_OBJECT
public:
    SettingTile(const QString& title, const QString& description, const QIcon& icon = {},
                QWidget* parent = nullptr);

    // Named for the callers that bind it; it is a switch (object name "settingsTileCheck"),
    // with the QAbstractButton API they use.
    SettingsSwitch* checkbox() const { return checkbox_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    SettingsSwitch* checkbox_;
};

// Tile for a setting with a value control instead of a switch. compact places the control
// beside the text (spin boxes); otherwise it fills the body under the description (combos with
// long item text). Affinity: GUI thread.
class FieldTile : public TileBase {
    Q_OBJECT
public:
    FieldTile(const QString& title, const QString& description, QWidget* control,
              const QIcon& icon = {}, bool compact = false, QWidget* parent = nullptr);

    QWidget* control() const { return control_; }

private:
    QWidget* control_;
};

// Tile for a read-only value or a group of related controls. It is not a click target, so it
// keeps the resting colours like FieldTile; callers fill the trailing slot and the body.
// Affinity: GUI thread.
class InfoTile : public TileBase {
    Q_OBJECT
public:
    InfoTile(const QString& title, const QString& description, const QIcon& icon = {},
             QWidget* parent = nullptr);
};

} // namespace fv
