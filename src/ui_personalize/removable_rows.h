#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace fv {

class Button;
class WrappedLabel;

// App names, each with a Remove button, or an empty-state line (settings_styles.py _Rows, object
// name "appStylesRows"): the "Never read from" apps and the apps moved to another style.
//
// `picker` is the control under the list. Tab goes from the last Remove to it, and the rows sit
// in the Tab order between whatever precedes the list and the picker, however often they are
// rebuilt. A rebuild keeps keyboard focus: on the same app's Remove when it is still listed, else
// on the Remove that took its place (the one above when it was the last), else on the picker.
// Focus moves before the old rows go, and new rows show at once, because focus given to a hidden
// button waits for it and is lost to the next focus change.
// Affinity: GUI thread.
class RemovableRows : public QWidget {
    Q_OBJECT
public:
    // (value, label): Remove reports the value.
    using Entries = std::vector<std::pair<QString, QString>>;

    RemovableRows(const QString& emptyText, std::function<void(const QString&)> onRemove,
                  QWidget* picker, QWidget* parent = nullptr);

    // Shows `entries`; the same entries again change nothing.
    void setRows(const Entries& entries);

    // The current rows (object name "appStylesRow"), in order.
    const std::vector<QWidget*>& rows() const { return rows_; }
    // Their Remove buttons, in order.
    std::vector<Button*> removeButtons() const;
    WrappedLabel* emptyLabel() const { return empty_; }

private:
    void chainTabOrder(const std::vector<Button*>& removes);

    std::function<void(const QString&)> onRemove_;
    QPointer<QWidget> picker_;
    QVBoxLayout* column_;
    WrappedLabel* empty_;
    std::vector<QWidget*> rows_;
    std::vector<Button*> removes_;
    Entries shown_;
};

} // namespace fv
