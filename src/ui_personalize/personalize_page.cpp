#include "ui_personalize/personalize_page.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/show_watcher.h"
#include "ui_settings/settings_dialog.h"

namespace fv {

PersonalizePage::PersonalizePage(PersonalizeHost& host, SettingsPage& container)
    : QWidget(&container), host_(host), container_(container) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
    // Other pages change what this one shows (the AI cleanup provider, shortcuts) while it is
    // hidden.
    new ShowWatcher(this, [this] { resync(); });
}

PersonalizePage::~PersonalizePage() = default;

SettingsPage::TileGroup PersonalizePage::addTileGroup(const QString& title,
                                                      std::vector<QWidget*> tiles, int columns,
                                                      const QString& intro) {
    return container_.addTileGroup(layout_, title, std::move(tiles), columns, intro);
}

bool PersonalizePage::loading() const {
    return loadingDepth_ > 0 || host_.isLoading();
}

bool PersonalizePage::save(std::string_view key, const Json& value) {
    return !loading() && host_.persist(key, value);
}

bool PersonalizePage::saveMany(const Json& updates, std::vector<std::string> drops) {
    return !loading() && host_.persistMany(updates, std::move(drops));
}

void PersonalizePage::exposeControl(std::string name, QWidget* control) {
    controls_[std::move(name)] = control;
}

QWidget* PersonalizePage::namedControl(std::string_view name) {
    const auto found = controls_.find(name);
    return found == controls_.end() ? nullptr : found->second.data();
}

void PersonalizePage::loadSettings(const Json& settings) {
    const LoadingScope scope(*this);
    load(settings);
}

void PersonalizePage::resync() {
    const LoadingScope scope(*this);
    sync(host_.settings());
}

void registerPersonalizePage(AppContext& context, PageDescriptor descriptor) {
    SettingsPageSpec spec;
    spec.destination = descriptor.destination;
    spec.title = std::move(descriptor.title);
    spec.subtitle = std::move(descriptor.subtitle);
    spec.scroll = descriptor.scroll;
    spec.railValue = std::move(descriptor.railValue);
    spec.searchFields = std::move(descriptor.searchFields);
    spec.helpTexts = std::move(descriptor.helpTexts);
    spec.controls = std::move(descriptor.controls);
    spec.create = [create = std::move(descriptor.create)](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        if (dialog == nullptr) {
            throw std::logic_error("A Personalize page is built inside the Settings window.");
        }
        auto host = std::make_unique<DialogPersonalizeHost>(*dialog);
        PersonalizePage* page = create ? create(*host, *container) : nullptr;
        if (page == nullptr) {
            throw std::logic_error("A Personalize page's builder made no page.");
        }
        // Owned by the window, which deletes its pages first (~SettingsDialog, while it is still
        // whole and refuses saves): the host outlives every control torn down with the page (a
        // capture field resuming hotkeys, say). As a child of the container it would go before
        // the page, since children are deleted in the order they were added.
        host.release()->setParent(dialog);
        return page;
    };
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
