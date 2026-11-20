#pragma once

#include <functional>
#include <map>
#include <string>

#include <QDialog>
#include <QString>
#include <QUrl>

#include "models/catalog.h"
#include "models/component_details.h"

class QGridLayout;
class QLabel;

namespace fv {

class Button;
class WrappedLabel;

// The bundled profile of one downloadable component: description, facts, what it is best for,
// its tradeoffs and two outbound links (source and original site). Installing and removing stay
// on the component row. Affinity: GUI thread.
class ComponentDetailsDialog : public QDialog {
    Q_OBJECT
public:
    // `info` is the component's current description (models::ComponentCoordinator::describe),
    // which carries the download and install sizes. Throws std::out_of_range for a component id
    // without a bundled profile.
    ComponentDetailsDialog(const std::string& componentId, const models::ComponentInfo& info,
                           QWidget* parent = nullptr);

    // What the two link buttons open; QDesktopServices::openUrl when not set.
    void setUrlOpener(std::function<void(const QUrl&)> opener);

    const QString& componentId() const { return componentId_; }
    QLabel* nameLabel() const { return nameLabel_; }
    QLabel* tagsLabel() const { return tagsLabel_; }
    WrappedLabel* descriptionLabel() const { return descriptionLabel_; }
    WrappedLabel* bestForLabel() const { return bestForLabel_; }
    WrappedLabel* tradeoffsLabel() const { return tradeoffsLabel_; }
    WrappedLabel* sourceNote() const { return sourceNote_; }
    Button* sourceButton() const { return sourceButton_; }
    Button* originButton() const { return originButton_; }
    // The value label of a facts row by its caption ("Origin", "Download size", ...).
    WrappedLabel* factLabel(const QString& caption) const;

private:
    void setupUi();
    void render(const models::ComponentInfo& info);
    void openUrl(const QUrl& url) const;

    QString componentId_;
    models::ComponentDetails details_;
    std::function<void(const QUrl&)> urlOpener_;

    QLabel* nameLabel_ = nullptr;
    QLabel* tagsLabel_ = nullptr;
    WrappedLabel* descriptionLabel_ = nullptr;
    QGridLayout* facts_ = nullptr;
    WrappedLabel* bestForLabel_ = nullptr;
    WrappedLabel* tradeoffsLabel_ = nullptr;
    WrappedLabel* sourceNote_ = nullptr;
    Button* sourceButton_ = nullptr;
    Button* originButton_ = nullptr;
    std::map<QString, WrappedLabel*> factLabels_;
};

} // namespace fv
