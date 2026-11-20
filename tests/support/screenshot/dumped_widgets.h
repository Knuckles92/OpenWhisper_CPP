#pragma once

#include <QDialog>
#include <QFrame>
#include <QString>
#include <QWidget>

#include <utility>

// Stand-ins for dumped widgets whose Python class answered text() although their Qt class has no
// text property (EmptyState, a QFrame, says "title hint"). The property lets dump_tree read the
// same text back from the rebuilt tree.
namespace fv::screenshot::detail {

class DumpedWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
public:
    DumpedWidget(QString text, QWidget* parent) : QWidget(parent), text_(std::move(text)) {}
    QString text() const { return text_; }

private:
    QString text_;
};

class DumpedFrame : public QFrame {
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
public:
    DumpedFrame(QString text, QWidget* parent) : QFrame(parent), text_(std::move(text)) {}
    QString text() const { return text_; }

private:
    QString text_;
};

class DumpedDialog : public QDialog {
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
public:
    DumpedDialog(QString text, QWidget* parent) : QDialog(parent), text_(std::move(text)) {}
    QString text() const { return text_; }

private:
    QString text_;
};

} // namespace fv::screenshot::detail
