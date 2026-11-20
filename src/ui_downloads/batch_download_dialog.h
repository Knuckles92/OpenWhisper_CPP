#pragma once

#include <string>
#include <vector>

#include <QDialog>
#include <QString>

class QLabel;

namespace fv {

class ElidingLabel;

// The confirmation before a multi-model download: each model with its bundled size, the
// estimated total, and the folder the files land in. Accepting is the consent for every listed
// model; the controller grants each one just before its download starts.
// Affinity: GUI thread.
class BatchDownloadDialog : public QDialog {
    Q_OBJECT
public:
    // `destination` is the folder the models are stored in (the speech model cache's root).
    BatchDownloadDialog(const std::vector<std::string>& modelNames, const QString& destination,
                        QWidget* parent = nullptr);

    QLabel* titleLabel() const { return titleLabel_; }
    QLabel* modelListLabel() const { return modelListLabel_; }
    QLabel* totalLabel() const { return totalLabel_; }
    ElidingLabel* destinationPathLabel() const { return destinationPathLabel_; }

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* modelListLabel_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    ElidingLabel* destinationPathLabel_ = nullptr;
};

} // namespace fv
