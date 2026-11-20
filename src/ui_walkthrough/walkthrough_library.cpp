#include "ui_walkthrough/walkthrough_library.h"

#include <algorithm>
#include <utility>

#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/utf8.h"
#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_walkthrough/review_text.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"

namespace fv {
namespace {

QLabel* label(const QString& text, const QString& name) {
    auto* value = new QLabel(text);
    value->setObjectName(name);
    return value;
}

// Interrupted and unreadable captures still have a row, with the date from the draft or folder.
QWidget* entry(const std::filesystem::path& path, std::chrono::year_month_day today,
               QString& summary) {
    std::optional<walkthrough::Walkthrough> document;
    try {
        document = walkthrough::load_document(path);
    } catch (const std::exception&) {
        // The review supplies the error details when the row is selected.
    }
    std::error_code error;
    const bool draft = std::filesystem::exists(path / "draft.json", error);
    const auto when = walkthrough_review::started(path, document ? &*document : nullptr);
    auto* widget = new QWidget;
    widget->setObjectName(QStringLiteral("wlEntry"));
    auto* column = new QVBoxLayout(widget);
    column->setContentsMargins(12, 9, 12, 10);
    column->setSpacing(3);
    auto* heading = new QHBoxLayout;
    const QString day =
        qt::toQString(when ? walkthrough_review::day(*when, today) : path_to_utf8(path.filename()));
    heading->addWidget(label(day, QStringLiteral("wlEntryDay")));
    heading->addStretch();
    const QString time = when ? qt::toQString(walkthrough_review::time(*when)) : QString{};
    if (when) {
        heading->addWidget(label(time, QStringLiteral("wlEntryTime")));
    }
    column->addLayout(heading);
    auto* details = new QHBoxLayout;
    QString meta = draft ? QStringLiteral("Not finished") : QStringLiteral("Can't be opened");
    if (document) {
        const auto shots = std::ranges::count_if(
            document->sections, [](const auto& section) { return section.shot.has_value(); });
        meta = qt::toQString(walkthrough_review::shots(static_cast<int>(shots)) + " · " +
                             walkthrough::clock(document->duration_s));
    }
    details->addWidget(label(meta, QStringLiteral("wlEntryMeta")));
    details->addStretch();
    if (draft || walkthrough::written_up(path)) {
        auto* pill = label(draft ? QStringLiteral("Interrupted") : QStringLiteral("AI write-up"),
                           QStringLiteral("wlPill"));
        if (draft) {
            ui::setStyleProperty(pill, ui::prop::tone, QStringLiteral("warning"));
        }
        details->addWidget(pill);
    }
    column->addLayout(details);
    summary = day + " " + time + ", " + meta + (draft ? ", interrupted" : "");
    return widget;
}

} // namespace

WalkthroughLibraryDialog::WalkthroughLibraryDialog(SettingsStore& settings,
                                                   std::filesystem::path root, ReviewHost host,
                                                   QWidget* parent)
    : QDialog(parent), root_(std::move(root)),
      review_(new WalkthroughReview(settings, std::move(host), false, this)) {
    setObjectName(QStringLiteral("walkthroughLibraryDialog"));
    setWindowTitle(QStringLiteral("Walkthroughs"));
    setWindowFlag(Qt::WindowMinMaxButtonsHint);
    resize(1100, 780);
    QFile sheet(QStringLiteral(":/ui_walkthrough/walkthrough_library.qss"));
    if (sheet.open(QIODevice::ReadOnly)) {
        ui::setLocalStyleSheet(this, QString::fromUtf8(sheet.readAll()));
    }

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("wlSidebar"));
    sidebar->setFixedWidth(290);
    layout->addWidget(sidebar);
    auto* column = new QVBoxLayout(sidebar);
    column->setContentsMargins(14, 18, 14, 14);
    column->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    titles->addWidget(label(QStringLiteral("Walkthroughs"), QStringLiteral("wlSidebarTitle")));
    count_ = label({}, QStringLiteral("wlSidebarCount"));
    titles->addWidget(count_);
    heading->addLayout(titles, 1);
    refresh_ = new QToolButton;
    refresh_->setObjectName(QStringLiteral("wlRefresh"));
    refresh_->setIcon(ui::designIcon(QStringLiteral("refresh-blue.svg")));
    refresh_->setIconSize({18, 18});
    refresh_->setToolTip(QStringLiteral("Refresh"));
    refresh_->setAccessibleName(refresh_->toolTip());
    heading->addWidget(refresh_, 0, Qt::AlignTop);
    column->addLayout(heading);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("wlList"));
    list_->setAccessibleName(QStringLiteral("Saved walkthroughs and interrupted drafts"));
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    column->addWidget(list_, 1);
    auto* retention = new QHBoxLayout;
    retention->addWidget(label(QStringLiteral("Keep walkthroughs"), QStringLiteral("wlKeepLabel")));
    retention->addStretch();
    keep_ = new QComboBox;
    keep_->setObjectName(QStringLiteral("wlKeep"));
    keep_->setAccessibleName(QStringLiteral("How long to keep completed walkthroughs"));
    keep_->setToolTip(QStringLiteral("Completed walkthroughs older than this are deleted at "
                                     "startup. Interrupted drafts are never deleted."));
    for (const auto& [days, text] : {std::pair{0, "Forever"},
                                     {7, "1 week"},
                                     {30, "30 days"},
                                     {90, "90 days"},
                                     {365, "1 year"}}) {
        keep_->addItem(QString::fromLatin1(text), days);
    }
    retention->addWidget(keep_);
    column->addLayout(retention);
    showRetention();

    auto* detail = new QVBoxLayout;
    detail->setContentsMargins(28, 22, 28, 18);
    detail->setSpacing(10);
    detail->addWidget(review_, 1);
    auto* close = new QPushButton(QStringLiteral("Close"));
    close->setObjectName(QStringLiteral("wlClose"));
    close->setAutoDefault(false);
    detail->addWidget(close, 0, Qt::AlignRight);
    layout->addLayout(detail, 1);

    connect(close, &QPushButton::clicked, this, &WalkthroughLibraryDialog::reject);
    connect(refresh_, &QToolButton::clicked, this, [this] {
        if (review_->discardChanges()) {
            refresh(review_->folder());
        }
    });
    connect(list_, &QListWidget::currentItemChanged, this, &WalkthroughLibraryDialog::selectItem);
    connect(keep_, &QComboBox::currentIndexChanged, this, [this] { changeRetention(); });
    connect(review_, &WalkthroughReview::busyChanged, this, [this](bool busy) {
        list_->setEnabled(!busy);
        keep_->setEnabled(!busy);
        refresh_->setEnabled(!busy);
        if (!busy) {
            showRetention();
        }
    });
    connect(review_, &WalkthroughReview::changed, this, [this](const QString& folder) {
        refresh(folder.isEmpty() ? std::nullopt
                                 : std::optional(path_from_utf8(qt::toUtf8(folder))));
        Q_EMIT changed();
    });
    refresh({});
}

void WalkthroughLibraryDialog::refresh(const std::optional<std::filesystem::path>& folder) {
    const auto folders = walkthrough::entries(root_);
    const auto today = walkthrough_review::today();
    const QSignalBlocker block(list_);
    list_->clear();
    for (const auto& path : folders) {
        QString accessible;
        auto* row = entry(path, today, accessible);
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, qt::toQString(path_to_utf8(path)));
        item->setData(Qt::AccessibleTextRole, accessible);
        item->setSizeHint(row->sizeHint());
        list_->setItemWidget(item, row);
        if (folder && path == *folder) {
            list_->setCurrentItem(item);
        }
    }
    if (list_->currentRow() < 0 && list_->count()) {
        list_->setCurrentRow(0);
    }
    count_->setText(folders.empty() ? QStringLiteral("None yet")
                                    : QStringLiteral("%1 saved").arg(list_->count()));
    const auto* item = list_->currentItem();
    review_->showFolder(
        item ? std::optional(path_from_utf8(qt::toUtf8(item->data(Qt::UserRole).toString())))
             : std::nullopt);
}

void WalkthroughLibraryDialog::selectItem(QListWidgetItem* current, QListWidgetItem* previous) {
    if (!review_->discardChanges()) {
        const QSignalBlocker block(list_);
        list_->setCurrentItem(previous);
        return;
    }
    review_->showFolder(
        current ? std::optional(path_from_utf8(qt::toUtf8(current->data(Qt::UserRole).toString())))
                : std::nullopt);
}

void WalkthroughLibraryDialog::showRetention() {
    const QSignalBlocker block(keep_);
    const int days = walkthrough::retention_days(root_);
    if (keep_->findData(days) < 0) {
        keep_->addItem(QStringLiteral("%1 days").arg(days), days);
    }
    keep_->setCurrentIndex(keep_->findData(days));
}

void WalkthroughLibraryDialog::changeRetention() {
    const int days = keep_->currentData().toInt();
    if (days == walkthrough::retention_days(root_)) {
        return;
    }
    if (!review_->discardChanges() ||
        (days &&
         !review_->ask(
             QStringLiteral("Keep walkthroughs"),
             QStringLiteral("Delete completed walkthroughs older than %1 now and at each startup? "
                            "Interrupted drafts and standalone screenshots are never deleted.")
                 .arg(keep_->currentText().toLower())))) {
        showRetention();
        return;
    }
    review_->run(
        [root = root_, days](const CancelToken& cancel) {
            if (!cancel.cancelled()) {
                walkthrough::set_retention(root, days);
                walkthrough::prune(root);
            }
            return ReviewOutcome{};
        },
        std::nullopt, QStringLiteral("Retention updated."));
    if (!review_->isBusy()) {
        showRetention();
    }
}

bool WalkthroughLibraryDialog::openFolder(std::optional<std::filesystem::path> folder) {
    if (review_->isBusy() || !review_->discardChanges()) {
        return false;
    }
    refresh(folder);
    show();
    raise();
    activateWindow();
    return true;
}

bool WalkthroughLibraryDialog::canClose() {
    if (review_->isBusy()) {
        review_->cancel();
        return false;
    }
    if (!review_->discardChanges()) {
        return false;
    }
    if (review_->isDirty()) {
        review_->discard();
    }
    return true;
}

void WalkthroughLibraryDialog::reject() {
    if (canClose()) {
        QDialog::reject();
    }
}

void WalkthroughLibraryDialog::closeEvent(QCloseEvent* event) {
    if (canClose()) {
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace fv
