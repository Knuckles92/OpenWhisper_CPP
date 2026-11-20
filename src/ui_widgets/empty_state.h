#pragma once

#include <QFrame>
#include <QString>

class QVBoxLayout;
class QWidget;

namespace fv {

class WrappedLabel;

// The one empty-list look Settings uses: a dashed box (object name "emptyState") with a
// centred title and a one-line hint. addAction puts a button centred under the hint, for lists
// whose first item is best added from right there. Affinity: GUI thread.
class EmptyState : public QFrame {
    Q_OBJECT
    // The title and hint as one string, for tests and accessibility.
    Q_PROPERTY(QString text READ text)
public:
    explicit EmptyState(const QString& title = {}, const QString& hint = {},
                        QWidget* parent = nullptr);

    // Shows title and, when given, hint under it.
    void setText(const QString& title, const QString& hint = {});
    QString text() const;

    // Puts button centred under the hint, spacing pixels below it (none when 0).
    void addAction(QWidget* button, int spacing = 8);

    WrappedLabel* titleLabel() const { return titleLabel_; }
    WrappedLabel* hintLabel() const { return hintLabel_; }

private:
    QVBoxLayout* layout_;
    WrappedLabel* titleLabel_;
    WrappedLabel* hintLabel_;
};

} // namespace fv
