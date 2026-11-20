#include "ui_downloads/component_details_dialog.h"

#include <utility>

#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "history/format_utils.h"
#include "ui_theme/app_icon.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

ComponentDetailsDialog::ComponentDetailsDialog(const std::string& componentId,
                                               const models::ComponentInfo& info, QWidget* parent)
    : QDialog(parent), componentId_(qt::toQString(componentId)),
      details_(models::get_component_details(componentId)) {
    setObjectName(QStringLiteral("componentDetailsDialog"));
    setWindowTitle(qt::toQString(details_.display_name));
    setWindowIcon(ui::appIcon());
    setModal(true);
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(460);
    setMaximumWidth(520);
    setMinimumHeight(420);

    setupUi();
    render(info);
}

void ComponentDetailsDialog::setUrlOpener(std::function<void(const QUrl&)> opener) {
    urlOpener_ = std::move(opener);
}

WrappedLabel* ComponentDetailsDialog::factLabel(const QString& caption) const {
    const auto found = factLabels_.find(caption);
    return found != factLabels_.end() ? found->second : nullptr;
}

void ComponentDetailsDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 18, 22, 16);
    layout->setSpacing(10);

    auto* eyebrow = new QLabel(QStringLiteral("Component"));
    eyebrow->setObjectName(QStringLiteral("downloadsEyebrow"));
    layout->addWidget(eyebrow);

    nameLabel_ = new QLabel(qt::toQString(details_.display_name));
    nameLabel_->setObjectName(QStringLiteral("downloadsInspectorName"));
    nameLabel_->setWordWrap(true);
    layout->addWidget(nameLabel_);

    tagsLabel_ = new QLabel(QString());
    tagsLabel_->setObjectName(QStringLiteral("downloadsInspectorTags"));
    tagsLabel_->setVisible(false);
    layout->addWidget(tagsLabel_);

    auto* detailScroll = new QScrollArea;
    detailScroll->setObjectName(QStringLiteral("downloadsInspectorScroll"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setMinimumHeight(180);

    auto* content = new QWidget;
    auto* detail = new QVBoxLayout(content);
    detail->setContentsMargins(0, 0, 6, 0);
    detail->setSpacing(12);

    descriptionLabel_ = new WrappedLabel(QString());
    descriptionLabel_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(descriptionLabel_);

    facts_ = new QGridLayout;
    facts_->setContentsMargins(0, 0, 0, 0);
    facts_->setHorizontalSpacing(12);
    facts_->setVerticalSpacing(5);
    facts_->setColumnStretch(1, 1);
    detail->addLayout(facts_);

    auto* bestHeading = new QLabel(QStringLiteral("Best for"));
    bestHeading->setObjectName(QStringLiteral("downloadsSectionTitle"));
    bestForLabel_ = new WrappedLabel(QString());
    bestForLabel_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(bestHeading);
    detail->addWidget(bestForLabel_);

    auto* tradeoffsHeading = new QLabel(QStringLiteral("Tradeoffs"));
    tradeoffsHeading->setObjectName(QStringLiteral("downloadsSectionTitle"));
    tradeoffsLabel_ = new WrappedLabel(QString());
    tradeoffsLabel_->setObjectName(QStringLiteral("downloadsInspectorBody"));
    detail->addWidget(tradeoffsHeading);
    detail->addWidget(tradeoffsLabel_);

    sourceNote_ = new WrappedLabel(QString());
    sourceNote_->setObjectName(QStringLiteral("downloadsSourceNote"));
    detail->addWidget(sourceNote_);
    detail->addStretch();

    detailScroll->setWidget(content);
    layout->addWidget(detailScroll, 1);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    sourceButton_ = new Button(qt::toQString(details_.source_label));
    sourceButton_->setObjectName(QStringLiteral("downloadsRepoButton"));
    fitCompactButton(sourceButton_);
    connect(sourceButton_, &QPushButton::clicked, this,
            [this] { openUrl(QUrl(qt::toQString(details_.source_url))); });
    actions->addWidget(sourceButton_, 1);

    originButton_ = new Button(qt::toQString(details_.origin_label));
    originButton_->setObjectName(QStringLiteral("downloadsOriginButton"));
    fitCompactButton(originButton_);
    connect(originButton_, &QPushButton::clicked, this,
            [this] { openUrl(QUrl(qt::toQString(details_.origin_url))); });
    actions->addWidget(originButton_, 1);

    auto* closeButton = new Button(QStringLiteral("Close"));
    closeButton->setObjectName(QStringLiteral("downloadsCloseButton"));
    fitCompactButton(closeButton);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    actions->addWidget(closeButton);
    layout->addLayout(actions);
}

void ComponentDetailsDialog::render(const models::ComponentInfo& info) {
    const QString tags = qt::toQString(details_.compact_tags);
    tagsLabel_->setText(tags);
    tagsLabel_->setVisible(!tags.isEmpty());
    descriptionLabel_->setText(qt::toQString(details_.description));
    bestForLabel_->setText(qt::toQString(details_.best_for));
    QStringList limitations;
    for (const std::string& item : details_.limitations) {
        limitations.append(QStringLiteral("• ") + qt::toQString(item));
    }
    tradeoffsLabel_->setText(limitations.join(QLatin1Char('\n')));
    sourceNote_->setText(qt::toQString(details_.source_note));
    sourceButton_->setToolTip(qt::toQString(details_.source_url));
    originButton_->setToolTip(qt::toQString(details_.origin_url));
    // Speech runtimes publish one upstream project for both origin and source; a second button
    // to the same page is a duplicate.
    originButton_->setHidden(details_.origin_url == details_.source_url);

    const QString dash = QStringLiteral("—");
    const QString download =
        info.download_bytes != 0 ? qt::toQString(format_size_bytes(info.download_bytes)) : dash;
    const QString install =
        info.install_bytes != 0 ? qt::toQString(format_size_bytes(info.install_bytes)) : dash;
    const std::pair<QString, QString> rows[] = {
        {QStringLiteral("Origin"), qt::toQString(details_.origin_name)},
        {QStringLiteral("Source"), qt::toQString(details_.source_name)},
        {QStringLiteral("Maintainer"), qt::toQString(details_.maintainer)},
        {QStringLiteral("Requires"), qt::toQString(details_.requires_text)},
        {QStringLiteral("Payload"), qt::toQString(details_.payload)},
        {QStringLiteral("Download size"), download},
        {QStringLiteral("Install size"), install},
        {QStringLiteral("Local format"), qt::toQString(details_.local_format)},
        {QStringLiteral("License"), qt::toQString(details_.license)},
    };
    int index = 0;
    for (const auto& [caption, value] : rows) {
        auto* captionLabel = new QLabel(caption);
        captionLabel->setObjectName(QStringLiteral("downloadsFactLabel"));
        auto* valueLabel = new WrappedLabel(value);
        valueLabel->setObjectName(QStringLiteral("downloadsFactValue"));
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        facts_->addWidget(captionLabel, index, 0);
        facts_->addWidget(valueLabel, index, 1);
        factLabels_[caption] = valueLabel;
        ++index;
    }
}

void ComponentDetailsDialog::openUrl(const QUrl& url) const {
    if (urlOpener_) {
        urlOpener_(url);
    } else {
        QDesktopServices::openUrl(url);
    }
}

} // namespace fv
