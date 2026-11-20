#include "ui_widgets/thinking_combo.h"

#include <QComboBox>
#include <QString>
#include <QVariant>

#include "appkit/qt_bridge.h"
#include "llm/thinking_levels.h"

namespace fv {

std::optional<std::string> fillThinkingCombo(QComboBox* combo,
                                             const std::vector<std::string>& offered,
                                             const std::string& saved) {
    const std::optional<std::string> shown = llm::nearest(saved, offered);
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    if (!offered.empty()) {
        for (const std::string& level : offered) {
            // An offered level always has a label; the id stands in for one that does not.
            const std::optional<std::string_view> label = llm::thinking_label(level);
            combo->addItem(qt::toQString(label ? *label : std::string_view(level)),
                           qt::toQString(level));
        }
        combo->setCurrentIndex(combo->findData(qt::toQString(*shown)));
    } else {
        // Item data None: an invalid variant, which findData and currentData report as null.
        combo->addItem(QStringLiteral("Provider default"), QVariant());
    }
    combo->setEnabled(!offered.empty());
    combo->blockSignals(blocked);
    return shown;
}

} // namespace fv
