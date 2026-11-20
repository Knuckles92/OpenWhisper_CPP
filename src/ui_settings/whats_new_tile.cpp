#include "ui_settings/whats_new_tile.h"

#include "appcore/whats_new.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"

namespace fv {

WhatsNewTile::WhatsNewTile(QWidget* parent)
    : InfoTile(QStringLiteral("New in %1").arg(qt::toQString(identity().product)),
               qt::toQString(appcore::whats_new().text),
               ui::designIcon(QStringLiteral("wand-purple.svg")), parent),
      dismiss_(neutralButton(new Button(QStringLiteral("Got it")))) {
    ui::setStyleProperty(this, ui::prop::kind, QStringLiteral("intro"));
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("whatsNew"));
    dismiss_->setToolTip(QStringLiteral("Hide this note"));
    fitCompactButton(dismiss_, kDismissWidth);
    connect(dismiss_, &QPushButton::clicked, this, [this] { Q_EMIT dismissRequested(); });
    addTrailing(dismiss_);
}

} // namespace fv
