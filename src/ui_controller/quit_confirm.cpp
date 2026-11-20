#include "ui_controller/quit_confirm.h"

#include <QMessageBox>

#include "appcore/activity.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"

namespace fv {

std::optional<QuitQuestion> quitQuestion(const ActivitySnapshot& activity) {
    const auto reasons = busy_reasons(activity);
    if (reasons.empty()) {
        return std::nullopt;
    }
    const QString product = qt::toQString(identity().product);
    return QuitQuestion{
        QStringLiteral("Quit %1?").arg(product),
        QStringLiteral("%1 is busy with %2. Quitting now stops it before it finishes.")
            .arg(product, qt::toQString(reasons.front()))};
}

QuitPrompt questionBoxPrompt() {
    return [](QWidget* parent, const QuitQuestion& question) {
        const auto answer =
            QMessageBox::question(parent, question.title, question.text,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return answer == QMessageBox::Yes;
    };
}

bool confirmQuitWhileBusy(QWidget* parent, const ActivitySnapshot& activity,
                          const QuitPrompt& prompt) {
    const std::optional<QuitQuestion> question = quitQuestion(activity);
    if (!question) {
        return true;
    }
    return prompt ? prompt(parent, *question) : questionBoxPrompt()(parent, *question);
}

} // namespace fv
