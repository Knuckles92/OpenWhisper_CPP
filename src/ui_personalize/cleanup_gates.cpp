#include "ui_personalize/cleanup_gates.h"

#include <algorithm>
#include <exception>
#include <typeinfo>
#include <utility>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_prompts.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

// The Settings window `widget` sits in, or null.
SettingsDialog* dialogOf(QWidget* widget) {
    for (QWidget* at = widget; at != nullptr; at = at->parentWidget()) {
        if (auto* dialog = qobject_cast<SettingsDialog*>(at)) {
            return dialog;
        }
    }
    return nullptr;
}

} // namespace

CleanupGates::CleanupGates(SettingsStore& settings, QObject* parent)
    : QObject(parent), settings_(settings) {}

CleanupGates::~CleanupGates() = default;

InfoTile* CleanupGates::gateTile(const QString& description, std::function<bool()> needed,
                                 QWidget* parent) {
    auto* tile = new InfoTile(QStringLiteral("AI cleanup is off"), description,
                              ui::designIcon(QStringLiteral("info-warning.svg")), parent);
    ui::setStyleProperty(tile, ui::prop::kind, QStringLiteral("notice"));
    auto* button = compactPrimaryButton(new Button(QStringLiteral("Turn on")));
    connect(button, &QPushButton::clicked, this, [this, button] { turnOnCleanup(button); });
    tile->addActions({button});
    gates_.push_back({tile, button, std::move(needed)});
    refresh(*settings_.snapshot());
    return tile;
}

Button* CleanupGates::turnOnButton(const InfoTile* tile) const {
    const auto found = std::ranges::find_if(
        gates_, [tile](const Gate& gate) { return tile != nullptr && gate.tile == tile; });
    return found == gates_.end() ? nullptr : found->button.data();
}

void CleanupGates::refresh(const Json& settings) {
    std::erase_if(gates_, [](const Gate& gate) { return gate.tile.isNull(); });
    const bool enabled = text::resolve_level(settings) != text::CleanupLevel::None;
    const QString label = turnOnLabel(settings);
    // A needed() may build or tear down another gate, so walk a copy.
    const std::vector<Gate> gates = gates_;
    for (const Gate& gate : gates) {
        if (gate.tile.isNull()) {
            continue;
        }
        if (gate.button != nullptr) {
            gate.button->setText(label);
        }
        if (enabled || (gate.needed && !gate.needed())) {
            gate.tile->hide();
        } else if (gate.tile->parentWidget() != nullptr) {
            gate.tile->show();
        }
    }
}

void CleanupGates::turnOnCleanup(QWidget* origin) {
    if (handler_) {
        handler_(origin);
        return;
    }
    const std::string_view key = keys::transcript_cleanup_enabled.key();
    SettingsDialog* dialog = dialogOf(origin);
    bool saved = false;
    if (dialog != nullptr) {
        saved = dialog->persist(key, true);
    } else {
        try {
            settings_.save_setting(key, true);
            saved = true;
        } catch (const std::exception& error) {
            kLog.warning("Couldn't turn AI cleanup on ({})", typeid(error).name());
        }
    }
    if (saved && dialog != nullptr) {
        dialog->notifyChanged(QStringLiteral("cleanup"));
    }
    refresh(*settings_.snapshot());
}

void CleanupGates::setTurnOnHandler(std::function<void()> handler) {
    if (!handler) {
        handler_ = {};
        return;
    }
    handler_ = [handler = std::move(handler)](QWidget*) {
        handler();
    };
}

void CleanupGates::setTurnOnHandlerWithOrigin(std::function<void(QWidget* origin)> handler) {
    handler_ = std::move(handler);
}

QString CleanupGates::turnOnLabel(const Json& settings) {
    QString name;
    if (!text::custom_prompt(settings).empty()) {
        name = qt::toQString(text::CUSTOM_LABEL);
    } else {
        const std::string level =
            keys::transcript_cleanup_level.read(settings.is_object() ? settings : Json::object());
        const auto parsed = text::parse_cleanup_level(level);
        name = qt::toQString(text::level_name(parsed.value_or(text::CleanupLevel::Medium)));
    }
    return QStringLiteral("Turn on (%1)").arg(name);
}

} // namespace fv
