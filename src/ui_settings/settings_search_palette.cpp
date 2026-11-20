#include "ui_settings/settings_search_palette.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/log.h"
#include "models/model_details.h"
#include "models/speech_catalog.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_search_source.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_search"};

using appcore::SearchEntry;
using appcore::SearchKind;

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

QString highlighted(const QString& text, const std::vector<std::string>& tokens,
                    const std::string& color) {
    return q(appcore::highlight(u(text), tokens, color));
}

// The rail icon of a page (settings_dialog.py _rail_icons), "box-blue.svg" for one the rail
// layout does not list.
std::string railIcon(std::string_view key) {
    if (key == destinations::home.key) {
        return std::string(destinations::home.icon);
    }
    for (const auto& chapter : destinations::rail) {
        for (const auto& page : chapter.pages) {
            if (page.key == key) {
                return std::string(page.icon);
            }
        }
    }
    return "box-blue.svg";
}

// _crumb: where a destination sits, as results name it.
std::string crumb(const SettingsDialog& dialog, const QString& key) {
    const QString chapter = dialog.rail()->chapter(key);
    const QString name = dialog.rail()->name(key);
    return u(chapter.isEmpty() ? name : QStringLiteral("%1 › %2").arg(chapter, name));
}

// _among_choices: whether `tile` sits in a part of `page` made of choices rather than settings
// (a part marked with settingsSearch=false, say a gallery of starting points built from tiles).
// Tiles that are only folded away or on another tab are still settings.
bool amongChoices(const QWidget* tile, const QWidget* page) {
    for (const QWidget* widget = tile; widget != nullptr && widget != page;
         widget = widget->parentWidget()) {
        const QVariant mark = widget->property("settingsSearch");
        if (mark.isValid() && mark.typeId() == QMetaType::Bool && !mark.toBool()) {
            return true;
        }
    }
    return false;
}

// _control_value: the current text of the group's combo.
QString controlValue(const QWidget* group) {
    if (const auto* combo = group->findChild<QComboBox*>()) {
        return combo->currentText();
    }
    return {};
}

// _catalog_entries for a Downloads page that is not built yet, read from the catalogs: every
// speech model and every component installable here, without sizes or badges.
std::vector<SearchEntry> catalogEntries(AppContext& context) {
    std::vector<SearchEntry> entries;
    for (const models::SpeechModel& model : models::models()) {
        const models::ModelDetails* details = models::find_model_details(model.key);
        const std::string repo = details != nullptr && !details->repository_id.empty()
                                     ? details->repository_id
                                     : std::string(model.key);
        SearchEntry entry;
        entry.kind = SearchKind::Model;
        entry.title = std::string(model.label);
        entry.detail = "Downloads · " + repo;
        entry.destination = std::string(destinations::downloads);
        entry.model_name = std::string(model.key);
        entry.keywords = std::string(model.key) + " model";
        entry.icon = "download-blue.svg";
        entries.push_back(std::move(entry));
    }
    const HomeSources* sources = context.find<HomeSources>();
    if (sources == nullptr || !sources->components) {
        return entries;
    }
    try {
        for (const models::ComponentInfo& info : sources->components()) {
            SearchEntry entry;
            entry.kind = SearchKind::Model;
            entry.title = info.display_name;
            entry.detail = "Downloads › Components · " + info.summary;
            entry.destination = std::string(destinations::downloads);
            entry.component_id = info.component_id;
            entry.keywords = "component runtime add-on";
            entry.icon = "box-blue.svg";
            entries.push_back(std::move(entry));
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't list components for search; showing none: {}", error.what());
    }
    return entries;
}

// Whether `widget` still belongs to the window (a result's target may have been rebuilt since
// the index was read). Compares pointers only.
bool isLiveChild(const QWidget& root, const void* widget) {
    if (widget == nullptr) {
        return false;
    }
    const QList<QWidget*> children = root.findChildren<QWidget*>();
    return std::ranges::any_of(children, [widget](const QWidget* child) {
        return static_cast<const void*>(child) == widget;
    });
}

} // namespace

// --- SearchResultRow ---

SearchResultRow::SearchResultRow(const SearchEntry& entry, std::vector<std::string> tokens,
                                 QWidget* parent)
    : QWidget(parent), tokens_(std::move(tokens)), color_(u(ui::tokenColor("accent-soft").name())) {
    setObjectName(QStringLiteral("settingsSearchRow"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 7, 12, 7);
    row->setSpacing(12);
    auto* glyph = new QLabel;
    glyph->setObjectName(QStringLiteral("settingsSearchIcon"));
    glyph->setFixedSize(28, 28);
    glyph->setAlignment(Qt::AlignCenter);
    glyph->setPixmap(ui::tablerIcon(q(entry.icon)).pixmap(15, 15));
    row->addWidget(glyph);
    auto* copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(1);
    titleText_ = q(entry.title);
    title_ = new QLabel(highlighted(titleText_, tokens_, color_));
    title_->setObjectName(QStringLiteral("settingsSearchTitle"));
    title_->setTextFormat(Qt::RichText);
    copy->addWidget(title_);
    detailText_ = entry.kind == SearchKind::Help ? q(appcore::snippet(entry.detail, tokens_))
                                                 : q(entry.detail);
    detail_ = new QLabel(highlighted(detailText_, tokens_, color_));
    detail_->setObjectName(QStringLiteral("settingsSearchDetail"));
    detail_->setTextFormat(Qt::RichText);
    for (const auto& [label, text] :
         {std::pair{title_, titleText_}, std::pair{detail_, detailText_}}) {
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setToolTip(text);
    }
    copy->addWidget(detail_);
    row->addLayout(copy, 1);
    for (const std::string& badge : entry.badges) {
        auto* pill = new QLabel(q(badge));
        pill->setObjectName(QStringLiteral("settingsSearchBadge"));
        ui::setStyleProperty(pill, ui::prop::tone,
                             badge == "Downloaded" || badge == "Installed" ? QStringLiteral("ok")
                                                                           : QStringLiteral("use"));
        row->addWidget(pill);
    }
}

void SearchResultRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout()->activate();
    for (const auto& [label, text] :
         {std::pair{title_, titleText_}, std::pair{detail_, detailText_}}) {
        const QString visible =
            label->fontMetrics().elidedText(text, Qt::ElideRight, label->width());
        label->setText(highlighted(visible, tokens_, color_));
    }
}

// --- SearchPalette ---

SearchPalette::SearchPalette(QWidget* host, IndexProvider index)
    : QWidget(host), host_(host), index_(std::move(index)) {
    setObjectName(QStringLiteral("settingsSearchOverlay"));
    hide();

    panel_ = new QFrame(this);
    panel_->setObjectName(QStringLiteral("settingsSearchPanel"));
    auto* column = new QVBoxLayout(panel_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    input_ = new QLineEdit;
    input_->setObjectName(QStringLiteral("settingsSearchInput"));
    input_->setPlaceholderText(QStringLiteral("Search settings, models, and help"));
    input_->addAction(ui::tablerIcon(QStringLiteral("search-slate.svg")),
                      QLineEdit::LeadingPosition);
    connect(input_, &QLineEdit::textChanged, this, [this](const QString& text) { runQuery(text); });
    input_->installEventFilter(this);
    column->addWidget(input_);

    results_ = new QListWidget;
    results_->setObjectName(QStringLiteral("settingsSearchResults"));
    results_->setFrameShape(QFrame::NoFrame);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setFocusPolicy(Qt::NoFocus);
    connect(results_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { activateItem(item); });
    column->addWidget(results_);

    empty_ = new QLabel(QString());
    empty_->setObjectName(QStringLiteral("settingsSearchEmpty"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    column->addWidget(empty_);

    footer_ = new QWidget;
    footer_->setObjectName(QStringLiteral("settingsSearchFooter"));
    auto* hints = new QHBoxLayout(footer_);
    hints->setContentsMargins(16, 8, 16, 9);
    hints->setSpacing(16);
    for (const QString& text : {QStringLiteral("↑ ↓  move"), QStringLiteral("Enter  open"),
                                QStringLiteral("Esc  close")}) {
        auto* label = new QLabel(text);
        label->setObjectName(QStringLiteral("settingsSearchHint"));
        hints->addWidget(label);
    }
    hints->addStretch();
    count_ = new QLabel(QString());
    count_->setObjectName(QStringLiteral("settingsSearchHint"));
    hints->addWidget(count_);
    column->addWidget(footer_);

    host_->installEventFilter(this);
}

void SearchPalette::open(const QString& text) {
    entries_ = index_ ? index_() : std::vector<SearchEntry>{};
    place();
    show();
    raise();
    input_->setText(text);
    input_->selectAll();
    runQuery(input_->text());
    input_->setFocus(Qt::ShortcutFocusReason);
}

void SearchPalette::closePalette() {
    hide();
    host_->setFocus(Qt::OtherFocusReason);
}

void SearchPalette::place() {
    setGeometry(host_->rect());
    const int width = std::min(kPanelWidth, std::max(1, this->width() - 32));
    panel_->setFixedWidth(width);
    footer_->setVisible(width >= kFooterMinWidth && height() >= kFooterMinHeight);
    int chrome = input_->sizeHint().height();
    if (footer_->isVisibleTo(this)) {
        chrome += footer_->sizeHint().height();
    }
    results_->setFixedHeight(std::min(resultHeight_, std::max(48, height() - chrome - 32)));
    panel_->adjustSize();
    const int top = std::max(8, std::min(58, height() - panel_->height() - 8));
    panel_->move((this->width() - width) / 2, top);
}

bool SearchPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == host_ && event->type() == QEvent::Resize && isVisible()) {
        place();
    } else if (watched == input_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) {
            closePalette();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (QListWidgetItem* item = results_->currentItem()) {
                activateItem(item);
            }
            return true;
        }
        if (key == Qt::Key_Down || key == Qt::Key_Up) {
            move(key == Qt::Key_Down ? 1 : -1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SearchPalette::mousePressEvent(QMouseEvent* event) {
    if (!panel_->geometry().contains(event->position().toPoint())) {
        closePalette();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SearchPalette::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), ui::tokenColor("slate-rail", kScrimAlpha));
}

const SearchEntry* SearchPalette::entryOf(const QListWidgetItem* item) const {
    if (item == nullptr) {
        return nullptr;
    }
    const QVariant index = item->data(Qt::UserRole);
    if (!index.isValid()) {
        return nullptr;
    }
    const int at = index.toInt();
    return at >= 0 && at < static_cast<int>(listed_.size()) ? &listed_[at] : nullptr;
}

void SearchPalette::runQuery(const QString& text) {
    const std::string query = u(text);
    const std::vector<std::string> tokens = appcore::search_tokens(query);
    listed_ = appcore::match_entries(entries_, query);
    results_->clear();
    std::optional<SearchKind> currentKind;
    QListWidgetItem* first = nullptr;
    for (std::size_t index = 0; index < listed_.size(); ++index) {
        const SearchEntry& entry = listed_[index];
        if (entry.kind != currentKind) {
            currentKind = entry.kind;
            auto* header = new QListWidgetItem(results_);
            header->setFlags(Qt::NoItemFlags);
            auto* label = new QLabel(q(appcore::section_title(entry.kind)).toUpper());
            label->setObjectName(QStringLiteral("settingsSearchSection"));
            header->setSizeHint(QSize(0, label->sizeHint().height() + 12));
            results_->setItemWidget(header, label);
        }
        auto* item = new QListWidgetItem(results_);
        item->setData(Qt::UserRole, static_cast<int>(index));
        auto* row = new SearchResultRow(entry, tokens);
        item->setSizeHint(QSize(0, row->sizeHint().height()));
        results_->setItemWidget(item, row);
        if (first == nullptr) {
            first = item;
        }
    }
    if (first != nullptr) {
        results_->setCurrentItem(first);
    }
    const bool hasQuery = !tokens.empty();
    results_->setVisible(!listed_.empty());
    empty_->setVisible(listed_.empty());
    empty_->setText(hasQuery
                        ? QStringLiteral("Nothing matches \"%1\".").arg(text.trimmed())
                        : QStringLiteral("Type to search every setting, model, and help note."));
    const QString noun = listed_.size() == 1 ? QStringLiteral("result") : QStringLiteral("results");
    count_->setText(hasQuery ? QStringLiteral("%1 %2").arg(listed_.size()).arg(noun) : QString());
    int height = 0;
    for (int index = 0; index < results_->count(); ++index) {
        height += results_->sizeHintForRow(index) + 2 * results_->spacing();
    }
    resultHeight_ = std::min(kResultsMaxHeight, height + 12);
    place();
}

void SearchPalette::move(int step) {
    const int count = results_->count();
    if (count == 0) {
        return;
    }
    int index = results_->currentRow();
    for (int tries = 0; tries < count; ++tries) {
        index = ((index + step) % count + count) % count;
        QListWidgetItem* item = results_->item(index);
        if (entryOf(item) != nullptr) {
            results_->setCurrentItem(item);
            results_->scrollToItem(item);
            return;
        }
    }
}

void SearchPalette::activateItem(QListWidgetItem* item) {
    const SearchEntry* found = entryOf(item);
    if (found == nullptr) {
        return;
    }
    const SearchEntry entry = *found;
    closePalette();
    Q_EMIT activated(entry);
}

// --- Index ---

const std::vector<SearchAlias>& searchAliases() {
    // _SEARCH_ALIASES. The Voice model alias no longer names Runtime, a page FioriVoce does not
    // have.
    static const std::vector<SearchAlias> aliases{
        {destinations::walkthroughs, "Walkthrough and screenshot settings",
         "Dictation › Walkthroughs (moved from General)",
         "walkthrough screenshot snap capture pdf document title captions paper letter a4 ai "
         "write-up claude code agent instructions prompt"},
        {destinations::mcp, "Connect an agent with MCP", "Automate › API & MCP",
         "model context protocol agent server connection claude cursor history api live api "
         "events stream webhook subscribe mcp events claude code channel permissions run actions "
         "start recordings change dictated text curl"},
        {destinations::voice_model, "Model assignments",
         "Model Manager's choices now sit on Voice model, AI cleanup, Voice & speakers, and "
         "Intelligence",
         "model manager models assign languages i dictate in multilingual language switcher"},
        {destinations::cleanup, "Spoken corrections and lists",
         "Dictation › AI cleanup › Cleanup level",
         "self-correction scratch that actually bullet numbered list light medium high rewrite "
         "polish formatting"},
        {destinations::hotkeys,
         "Every shortcut: Command Mode, Scratchpad, walkthroughs, profiles, actions",
         "App › Hotkeys",
         "command mode scratchpad switch language paste original hands-free double tap latch "
         "mouse side button back forward mouse 4 mouse 5 walkthrough screenshot profile shortcut "
         "transform shortcut action shortcut agent hotkey keyboard shortcuts all shortcuts"},
        {destinations::downloads, "Download models and components", "Models & storage › Downloads",
         "download manager library catalog hugging face"},
        {destinations::meeting_intelligence, "Cloud intelligence",
         "Now AI insights; choose its model on Meeting Mode › Intelligence",
         "cloud insights ai insights meeting model llm"},
    };
    return aliases;
}

std::vector<SearchEntry> keywordEntries(const std::vector<SearchAlias>& aliases) {
    std::vector<SearchEntry> entries;
    for (const SearchAlias& alias : aliases) {
        SearchEntry entry;
        entry.kind = SearchKind::Setting;
        entry.title = alias.title;
        entry.detail = alias.detail;
        entry.destination = std::string(alias.destination);
        entry.keywords = alias.keywords;
        entries.push_back(std::move(entry));
    }
    return entries;
}

QString shortcutText(const QString& sequence) {
    return QKeySequence(sequence).toString(QKeySequence::NativeText);
}

std::vector<SearchEntry> buildSearchIndex(SettingsDialog& dialog) {
    std::vector<SearchEntry> entries;
    const QStringList keys = dialog.rail()->keys();
    // build_index: every page the rail lists.
    for (const QString& name : keys) {
        const std::string key = u(name);
        SettingsPage* page = dialog.page(key);
        if (page == nullptr) {
            continue;
        }
        const std::string where = crumb(dialog, name);
        const std::string icon = railIcon(key);
        const std::string title = u(dialog.pageTitle(key));
        SearchEntry heading;
        heading.title = title;
        heading.detail = where + " · " + u(dialog.pageSubtitle(key));
        heading.destination = key;
        heading.icon = icon;
        entries.push_back(std::move(heading));
        std::set<std::string> seen;
        for (const TileBase* tile : page->findChildren<TileBase*>()) {
            if (tile->isHidden() || amongChoices(tile, page)) {
                continue;
            }
            const std::string tileTitle = u(tile->titleLabel()->text().trimmed());
            if (tileTitle.empty() || !seen.insert(tileTitle).second) {
                continue;
            }
            SearchEntry entry;
            entry.title = tileTitle;
            entry.detail = where;
            entry.destination = key;
            entry.target = static_cast<const void*>(static_cast<const QWidget*>(tile));
            entry.keywords = u(tile->descriptionLabel()->text());
            entry.icon = icon;
            entries.push_back(std::move(entry));
        }
        for (const QWidget* group :
             page->findChildren<QWidget*>(QStringLiteral("modelManagerFieldGroup"))) {
            if (group->isHidden()) {
                continue;
            }
            const auto* caption = group->findChild<QLabel*>(QStringLiteral("textModelFieldLabel"));
            if (caption == nullptr || caption->text().trimmed().isEmpty()) {
                continue;
            }
            const QString value = controlValue(group);
            const QString text = caption->text().trimmed();
            SearchEntry entry;
            entry.title = u(value.isEmpty() ? text : QStringLiteral("%1: %2").arg(text, value));
            entry.detail = where;
            entry.destination = key;
            entry.target = static_cast<const void*>(group);
            entry.icon = icon;
            entries.push_back(std::move(entry));
        }
        for (const QLabel* note :
             page->findChildren<QLabel*>(QStringLiteral("textModelFootnote"))) {
            SearchEntry entry;
            entry.kind = SearchKind::Help;
            entry.title = title;
            entry.detail = u(note->text());
            entry.destination = key;
            entry.target = static_cast<const void*>(note->parentWidget());
            entry.icon = "info-blue.svg";
            entries.push_back(std::move(entry));
        }
    }
    AppContext& context = dialog.context();
    if (dialog.isRegistered(destinations::downloads)) {
        // A built Downloads page lists its own rows, with sizes and badges; until then the
        // catalogs stand in for it.
        std::optional<std::vector<SearchEntry>> rows;
        if (auto* source =
                dynamic_cast<SettingsSearchSource*>(dialog.pageContent(destinations::downloads))) {
            rows = source->searchEntries();
        }
        std::vector<SearchEntry> catalog = rows ? std::move(*rows) : catalogEntries(context);
        entries.insert(entries.end(), catalog.begin(), catalog.end());
    }
    std::vector<SearchAlias> aliases;
    for (const SearchAlias& alias : searchAliases()) {
        if (dialog.isRegistered(alias.destination)) {
            aliases.push_back(alias);
        }
    }
    std::vector<SearchEntry> keywords = keywordEntries(aliases);
    entries.insert(entries.end(), keywords.begin(), keywords.end());
    if (dialog.isRegistered(destinations::meeting_intelligence)) {
        // _agent_search_entries: the meeting agent picker, which is tiles rather than a setting
        // tile.
        SearchEntry agent;
        agent.title = "Who runs AI insights";
        agent.detail = crumb(dialog, q(destinations::meeting_intelligence));
        agent.destination = std::string(destinations::meeting_intelligence);
        agent.target_name = "meeting_agent_picker";
        if (dialog.isBuilt(destinations::meeting_intelligence)) {
            // The built picker itself, as Python's target; until then the name finds it.
            agent.target = static_cast<const void*>(dialog.control(agent.target_name));
        }
        agent.keywords = "meeting agent installed coding agent claude code codex opencode sign-in "
                         "pi sdk built-in";
        agent.icon = railIcon(destinations::meeting_intelligence);
        entries.push_back(std::move(agent));
    }
    // Pages never built are searched by their registered copy.
    for (const QString& name : keys) {
        const std::string key = u(name);
        if (dialog.isBuilt(key)) {
            continue;
        }
        const SettingsPageSpec* spec = context.settingsPages().page(key);
        if (spec == nullptr) {
            continue;
        }
        const std::string where = crumb(dialog, name);
        const std::string icon = railIcon(key);
        for (const SettingsSearchField& field : spec->searchFields) {
            SearchEntry entry;
            entry.title = u(field.label);
            entry.detail = where;
            entry.destination = key;
            entry.target_name = field.target;
            entry.keywords = u(field.description);
            entry.icon = icon;
            entries.push_back(std::move(entry));
        }
        for (const QString& text : spec->helpTexts) {
            SearchEntry entry;
            entry.kind = SearchKind::Help;
            entry.title = u(spec->title);
            entry.detail = u(text);
            entry.destination = key;
            entry.icon = "info-blue.svg";
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

void activateSearchEntry(SettingsDialog& dialog, const SearchEntry& entry) {
    if (!entry.model_name.empty()) {
        dialog.selectDestination(q(destinations::downloads));
        if (auto* links =
                dynamic_cast<SettingsDeepLinks*>(dialog.pageContent(destinations::downloads))) {
            links->revealModel(q(entry.model_name));
        }
        return;
    }
    if (!entry.component_id.empty()) {
        dialog.showDownloads(QString(), q(entry.component_id));
        return;
    }
    dialog.selectDestination(q(entry.destination));
    QWidget* target = nullptr;
    if (isLiveChild(dialog, entry.target)) {
        target = static_cast<QWidget*>(const_cast<void*>(entry.target));
    } else if (!entry.target_name.empty()) {
        target = dialog.control(entry.target_name);
    }
    if (target != nullptr) {
        dialog.reveal(target);
    }
}

SearchPalette* installSearchPalette(SettingsDialog& dialog) {
    QPointer<SettingsDialog> window = &dialog;
    auto* palette = new SearchPalette(&dialog, [window] {
        return window != nullptr ? buildSearchIndex(*window) : std::vector<SearchEntry>{};
    });
    QPointer<SearchPalette> guarded = palette;
    dialog.setSearchOpener([guarded](const QString& text) {
        if (guarded != nullptr) {
            guarded->open(text);
        }
    });
    dialog.setSearchCloser([guarded] {
        if (guarded != nullptr) {
            guarded->closePalette();
        }
    });
    QObject::connect(palette, &SearchPalette::activated, &dialog,
                     [window](const SearchEntry& entry) {
                         if (window == nullptr) {
                             return;
                         }
                         try {
                             activateSearchEntry(*window, entry);
                         } catch (const std::exception& error) {
                             kLog.error_with(error, "Couldn't open a search result");
                         } catch (...) {
                             kLog.error("Couldn't open a search result");
                         }
                     });
    return palette;
}

} // namespace fv
