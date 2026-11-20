#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"
#include "models/catalog.h"
#include "models/downloads_model.h"
#include "ui_settings/home_rows.h"
#include "ui_widgets/no_wheel.h"

class QBoxLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace fv {

class AppContext;
class ProfileHotkeyInput;
class SettingsDialog;
class SettingsSwitch;
class WhatsNewTile;
class WrappedLabel;

// What the where strip and the Storage card show (settings_home.py HomeSummary), composed by
// HomePage::refreshHome from what each destination reports.
struct HomeSummary {
    QStringList localItems;
    QStringList cloudItems;
    int storageDownloaded = 0;
    int storageTotal = 0;
    std::int64_t storageBytes = 0;
    // Bytes per backend label, in row order.
    std::vector<std::pair<QString, std::int64_t>> storageByBackend;
    bool storageChecking = false;
    // (display name, usable) of every component installable here.
    std::vector<std::pair<QString, bool>> components;
    QString footer;
};

// What Home and the search palette read that lives in libraries above this one: the Voice model
// page's model_assignments functions, the Downloads page's inventory and the component
// coordinator. Provided (empty) by registerHomePage with ctx.provide<HomeSources>(); the feature
// that owns those pages fills it in its install(). An unset member reads as nothing known: the
// voice as transcribed on this computer, the storage as still being checked, no components and
// an empty runtime summary.
// Affinity: GUI thread (each member is called there and must not block for long).
struct HomeSources {
    // voice_detail, voice_is_remote and voice_is_cloud of a settings snapshot.
    std::function<std::string(const Json& settings)> voiceDetail;
    std::function<bool(const Json& settings)> voiceIsRemote;
    std::function<bool(const Json& settings)> voiceIsCloud;
    // runtime_summary: the NeMo runtime as the footer names it.
    std::function<std::string(const Json& settings)> runtimeSummary;
    // The Downloads inventory's storage_summary; nullopt while it is still being read.
    std::function<std::optional<models::StorageSummary>()> storage;
    // ComponentCoordinator::list_components: every component installable here.
    std::function<std::vector<models::ComponentInfo>()> components;
    // focus_context.text_reading_supported(): whether this computer can read text near the
    // cursor at all. Empty: focus::text_reading_supported().
    std::function<bool()> textReadingSupported;
    // models.on_backend_changed: the controller's hook for an engine chosen on Home while the
    // Voice model page is not built, given the engine's display name ("Parakeet", "API",
    // "Remote computer"). It saves the choice; what it throws is shown. Unset: Home saves
    // selected_model itself.
    std::function<void(const QString& displayName)> backendChanged;
};

// One rounded bar split by how much each backend's models use (settings_home.py StorageBar).
// Affinity: GUI thread.
class StorageBar : public QWidget {
    Q_OBJECT
public:
    // The segments' colour tokens, in legend order; a sixth backend takes the first again.
    static constexpr std::string_view kBackendTokens[] = {"accent", "success", "purple",
                                                          "warning-text-soft", "accent-cyan"};

    explicit StorageBar(QWidget* parent = nullptr);

    // One segment per non-zero size, as a share of their sum, coloured by its position.
    void setSegments(const std::vector<std::int64_t>& sizes);
    // (fraction, token) of each segment.
    const std::vector<std::pair<double, std::string_view>>& segments() const { return segments_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<std::pair<double, std::string_view>> segments_;
};

// The microphone combo of Home (_MicrophoneCombo): shows the saved microphone and runs
// `beforePopup` (which reads the device list) each time its menu opens.
// Affinity: GUI thread.
class HomeMicrophoneCombo : public ElidingComboBox {
    Q_OBJECT
public:
    explicit HomeMicrophoneCombo(std::function<void()> beforePopup, QWidget* parent = nullptr);
    void showPopup() override;

private:
    std::function<void()> beforePopup_;
};

// One group of Home rows under an icon and a title (object name "homeCard"). A row keeps its
// control beside its text down to kSideBySideWidth at 100 % text, and stacks it below the text
// in a narrower card.
// Affinity: GUI thread.
class HomeCard : public QFrame {
    Q_OBJECT
public:
    // Narrowest card, at 100 % text, that keeps a control beside its row's text.
    static constexpr int kSideBySideWidth = 400;

    HomeCard(const QString& title, const QString& icon, QWidget* parent = nullptr);

    QVBoxLayout* rows() const { return rows_; }
    QLabel* titleLabel() const { return title_; }
    // Adds a row (with a divider above every row but the first) and lays it out for the width.
    void addRow(QBoxLayout* row, QWidget* control);
    int rowCount() const { return static_cast<int>(rowList_.size()); }
    bool isStacked() const { return stacked_.value_or(false); }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void reflow();

    QVBoxLayout* rows_;
    QLabel* title_;
    std::vector<std::pair<QBoxLayout*, QWidget*>> rowList_;
    std::optional<bool> stacked_;
};

// Settings > Home (settings_home.py HomePage and settings_dialog.py _refresh_home): the "New in
// FioriVoce" note, the where strip, the Dictation, Personalize, Meetings, App and Storage cards
// and the footer.
//
// Its rows are projections of the pages' own controls (bind): an edit builds the owning page,
// sets that page's control so its handler, callbacks and confirmations run, re-reads Home, and
// puts the source back silently when the save failed or was declined. Pages are built only when
// an edit needs their handler, and the microphone list is read only once its menu opens. Home is
// re-read (refreshHome) only while it is the page shown: on a rail redraw and when selected.
// Cards sit in two columns (Dictation, App | Personalize, Meetings, Storage) from
// kTwoColumnWidth at 100 % text, else in one; the where strip stacks below the same width.
// Affinity: GUI thread.
class HomePage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // What a projection shows for a settings snapshot: a bool for a switch, the item data for a
    // combo.
    using Resolver = std::function<QVariant(const Json& settings)>;

    // Narrowest card, at 100 % text, the page sets in two columns.
    static constexpr int kColumnWidth = 430;
    static constexpr int kColumnSpacing = 12;
    // The page width from which cards sit in two columns, at 100 % text.
    static constexpr int kTwoColumnWidth = 2 * kColumnWidth + kColumnSpacing;
    // The width of Command Mode's shortcut field, which Home's matches.
    static constexpr int kShortcutMinWidth = 160;
    static constexpr int kShortcutMaxWidth = 210;

    struct Options {
        // Whether this desktop session has a system tray.
        bool trayAvailable = true;
        // A native Wayland session pastes by hand.
        bool nativeWayland = false;
    };

    HomePage(SettingsDialog& dialog, Options options, QWidget* parent = nullptr);
    ~HomePage() override;

    SettingsDialog& dialog() const { return dialog_; }

    // --- Rows, for this page and the HomeRows contributors ---
    // A titled row with its control in `card`; returns the description label. The control's
    // accessible name becomes the title.
    WrappedLabel* addRow(HomeCardId card, const QString& title, const QString& description,
                         QWidget* control);
    // The same row, showing `resolver`'s value of the settings and saving edits through the
    // control `attribute` of the page `destination` (bind).
    WrappedLabel* addRow(HomeCardId card, const QString& title, const QString& description,
                         QWidget* control, std::string destination, std::string attribute,
                         Resolver resolver, std::string key = {});
    // A widget under the card's rows, outside any row (a status line).
    void addWidget(HomeCardId card, QWidget* widget);
    // A switch row bound as addRow does.
    SettingsSwitch* toggle(HomeCardId card, const QString& title, const QString& description,
                           std::string destination, std::string attribute, Resolver resolver,
                           std::string key = {});
    // A combo styled as Home's ("homeCombo", 36 px tall, at least 150 px wide; at most 240 px
    // unless expanding).
    ElidingComboBox* combo(bool expanding = false);
    // Shows a setting in `control` (a SettingsSwitch or a combo) and saves edits through the
    // page control `attribute` of `destination`. `key` names the control for control().
    void bind(QWidget* control, std::string destination, std::string attribute, Resolver resolver,
              std::string key = {});
    // Names a row's control for control() without binding it (Python's controls[key] = ...),
    // for a control whose row saves through a handler of its own.
    void registerControl(std::string key, QWidget* control);
    // Calls hook(settings) whenever Home re-reads its rows.
    void addRefreshHook(std::function<void(const Json& settings)> hook);

    // --- State ---
    // refresh: every projection, the voice line, the shortcut and the refresh hooks, under a
    // QSignalBlocker.
    void refresh();
    // _refresh_home: refresh(), then the where strip, the Storage card and the footer.
    void refreshHome();
    // refresh_shortcut: the record shortcut and what it does.
    void refreshShortcut();
    // update_summary.
    void updateSummary(const HomeSummary& summary);
    // The summary refreshHome shows, from a settings snapshot.
    HomeSummary composeSummary(const Json& settings) const;
    // _load_microphones: builds Recording (which lists the devices off this thread) and copies
    // its microphones, once.
    void loadMicrophones();

    // --- Parts, for the dialog, search and tests ---
    // "dictation", "personalize", "meetings", "app" or "storage"; null for anything else.
    HomeCard* card(std::string_view name) const;
    // The card an old Basic tab's key (basic_dictation, basic_meetings, basic_app) opens.
    HomeCard* section(std::string_view key) const;
    // The projection showing a setting (Python's controls[key]): "audio_input_priority",
    // "auto_paste", "streaming_enabled", "ui_theme", "ui_font_scale", "minimize_tray",
    // "update_check_enabled", "update_notify_enabled", "update_restart_prompt_enabled",
    // "selected_model", and what contributors bound with a key or registered.
    QWidget* control(std::string_view key) const;
    WhatsNewTile* intro() const { return intro_; }
    ProfileHotkeyInput* shortcut() const { return shortcut_; }
    QPushButton* shortcutChangeButton() const { return shortcutChange_; }
    WrappedLabel* shortcutDescription() const { return shortcutDescription_; }
    ElidingComboBox* voiceCombo() const { return voiceCombo_; }
    WrappedLabel* voiceDetail() const { return voiceDetail_; }
    WrappedLabel* pasteDescription() const { return pasteDescription_; }
    SettingsSwitch* updateNotify() const { return updateNotify_; }
    QFrame* whereStrip() const { return where_; }
    WrappedLabel* localTitle() const { return localTitle_; }
    WrappedLabel* localList() const { return localList_; }
    WrappedLabel* cloudTitle() const { return cloudTitle_; }
    WrappedLabel* cloudList() const { return cloudList_; }
    QLabel* storageValue() const { return storageValue_; }
    WrappedLabel* storageDetail() const { return storageDetail_; }
    StorageBar* storageBar() const { return storageBar_; }
    WrappedLabel* storageLegend() const { return storageLegend_; }
    WrappedLabel* componentsLabel() const { return components_; }
    WrappedLabel* footerLabel() const { return footer_; }
    bool isTwoColumns() const { return twoColumns_.value_or(false); }

    // SettingsPageModule.
    void refreshPage() override { refreshHome(); }
    void onShown() override { refreshHome(); }
    void cancelCapture() override;
    QWidget* namedControl(std::string_view name) override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Binding {
        QPointer<QWidget> control;
        std::string destination;
        std::string attribute;
        Resolver resolver;
        bool refreshed = true;
    };

    HomeCard* cardOf(HomeCardId card) const;
    WrappedLabel* addRowTo(HomeCard* card, const QString& title, const QString& description,
                           QWidget* control);
    QPushButton* addLink(HomeCard* card, const QString& text, std::string_view destination);
    void buildIntro(QVBoxLayout* layout);
    QFrame* buildWhereStrip();
    void buildDictation();
    void buildContributed(HomeCardId card);
    void buildApp();
    void buildStorage();
    void reflowColumns();
    void apply(const Binding& binding);
    void dismissIntro();
    void refreshMicrophone(const Json& settings);
    void beginShortcutCapture();
    void changeVoice();
    Json currentHotkeys(const Json& settings) const;

    SettingsDialog& dialog_;
    Options options_;
    std::map<std::string, HomeCard*, std::less<>> cards_;
    std::map<std::string, QPointer<QWidget>, std::less<>> controls_;
    std::vector<Binding> bindings_;
    std::vector<std::function<void(const Json&)>> refreshHooks_;
    std::optional<bool> twoColumns_;

    WhatsNewTile* intro_ = nullptr;
    QFrame* where_ = nullptr;
    QBoxLayout* whereRow_ = nullptr;
    QFrame* whereDivider_ = nullptr;
    WrappedLabel* localTitle_ = nullptr;
    WrappedLabel* localList_ = nullptr;
    WrappedLabel* cloudTitle_ = nullptr;
    WrappedLabel* cloudList_ = nullptr;
    QHBoxLayout* columnRow_ = nullptr;
    QVBoxLayout* columns_[2] = {nullptr, nullptr};
    HomeMicrophoneCombo* microphone_ = nullptr;
    ProfileHotkeyInput* shortcut_ = nullptr;
    QPushButton* shortcutChange_ = nullptr;
    WrappedLabel* shortcutDescription_ = nullptr;
    ElidingComboBox* voiceCombo_ = nullptr;
    WrappedLabel* voiceDetail_ = nullptr;
    WrappedLabel* pasteDescription_ = nullptr;
    SettingsSwitch* updateNotify_ = nullptr;
    QLabel* storageValue_ = nullptr;
    WrappedLabel* storageDetail_ = nullptr;
    StorageBar* storageBar_ = nullptr;
    WrappedLabel* storageLegend_ = nullptr;
    WrappedLabel* components_ = nullptr;
    WrappedLabel* footer_ = nullptr;
};

// Registers Settings > Home (destination overview) in the context's Settings pages and provides
// HomeRows and HomeSources (unless provided already). Building Home also puts the search palette
// on its Settings window (installSearchPalette). A page registered under overview already (a
// test's stand-in) is kept.
// Affinity: GUI thread.
void registerHomePage(AppContext& context);

} // namespace fv
