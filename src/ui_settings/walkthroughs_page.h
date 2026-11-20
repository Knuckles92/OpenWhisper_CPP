#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <QComboBox>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"

class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;

namespace fv {

class AppContext;
class Executor;
class FieldTile;
class HotkeysPage;
class SettingTile;
class SettingsDialog;
class SettingsPage;

namespace agents {
class InstalledAgents;
}
namespace detail {
template <class Widget> class Mailbox;
}

// Settings > Dictation > Walkthroughs (settings_dialog.py _build_walkthroughs_page): the three
// walkthrough shortcuts, then the Copy, Document and AI write-up groups. The shortcut rows are the
// Hotkeys page's own (HotkeysPage::shortcutRow on the page "walkthroughs"), so they share its
// capture, conflict check and Reset to defaults and always agree with the Hotkeys page.
//
// The Agent combo marks "(not installed)" on the agents the last scan did not find; with no scan
// yet it starts one on the worker, once, and marks them when it ends. Follow notes is disabled
// until the write-up is on.
// Affinity: GUI thread.
class WalkthroughsPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // The services the page reaches, each a seam a test replaces.
    struct Ports {
        // The Hotkeys page whose rows the shortcuts are. Null (or a function returning null)
        // leaves the page without its shortcuts group. The Settings window's own page unless set.
        std::function<HotkeysPage*()> hotkeysPage;
        // The agents the scan asks: installed_agents_if_set() unless set; none leaves every name
        // plain.
        std::shared_ptr<agents::InstalledAgents> agents;
        // Where the scan runs: it blocks.
        Executor* worker = nullptr;
    };

    // What the rows' own page is called: HotkeysPage's page argument.
    static constexpr std::string_view kShortcutPage = "walkthroughs";
    // "<name> (not installed)" is the combo's text for an agent the scan did not find.
    static constexpr std::string_view kNotInstalled = " (not installed)";

    WalkthroughsPage(SettingsPage& page, SettingsDialog& dialog, Ports ports);
    ~WalkthroughsPage() override;

    // The rail's one-line value: "AI write-up on" or "As spoken".
    static QString railValue(const Json& settings);

    FieldTile* copyFormatTile() const { return copyFormatTile_; }
    QComboBox* copyFormatCombo() const { return copyFormatCombo_; }
    FieldTile* titleTile() const { return titleTile_; }
    QLineEdit* titleEdit() const { return titleEdit_; }
    FieldTile* pageSizeTile() const { return pageSizeTile_; }
    QComboBox* pageSizeCombo() const { return pageSizeCombo_; }
    SettingTile* detailsTile() const { return detailsTile_; }
    SettingTile* aboutTile() const { return aboutTile_; }
    SettingTile* captionsTile() const { return captionsTile_; }
    SettingTile* writeUpTile() const { return writeUpTile_; }
    FieldTile* agentTile() const { return agentTile_; }
    QComboBox* agentCombo() const { return agentCombo_; }
    FieldTile* instructionsTile() const { return instructionsTile_; }
    QPlainTextEdit* instructionsEdit() const { return instructionsEdit_; }
    SettingTile* notesTile() const { return notesTile_; }
    // The shortcut field of one of the three rows; null when the Hotkeys page was not reachable
    // or the action has no row here.
    QWidget* shortcutInput(std::string_view action) const;

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void cancelCapture() override;
    QWidget* namedControl(std::string_view name) override;

private:
    void buildShortcuts(QVBoxLayout* layout);
    void buildCopyGroup(SettingsPage& page, QVBoxLayout* layout);
    void buildDocumentGroup(SettingsPage& page, QVBoxLayout* layout);
    void buildWriteUpGroup(SettingsPage& page, QVBoxLayout* layout);
    void bindSettings();
    void labelAgents();
    void startScan();

    SettingsDialog& dialog_;
    Ports ports_;
    std::shared_ptr<detail::Mailbox<WalkthroughsPage>> mailbox_;
    QPointer<HotkeysPage> hotkeys_;
    bool scanStarted_ = false;

    FieldTile* copyFormatTile_ = nullptr;
    QComboBox* copyFormatCombo_ = nullptr;
    FieldTile* titleTile_ = nullptr;
    QLineEdit* titleEdit_ = nullptr;
    FieldTile* pageSizeTile_ = nullptr;
    QComboBox* pageSizeCombo_ = nullptr;
    SettingTile* detailsTile_ = nullptr;
    SettingTile* aboutTile_ = nullptr;
    SettingTile* captionsTile_ = nullptr;
    SettingTile* writeUpTile_ = nullptr;
    FieldTile* agentTile_ = nullptr;
    QComboBox* agentCombo_ = nullptr;
    FieldTile* instructionsTile_ = nullptr;
    QPlainTextEdit* instructionsEdit_ = nullptr;
    SettingTile* notesTile_ = nullptr;
};

// What registerWalkthroughsPage reads when the page is built; a member left empty is taken from the
// context.
struct WalkthroughsOptions {
    // The Hotkeys page the shortcut rows come from: the Settings window's own, built when the
    // Walkthroughs page is.
    std::function<HotkeysPage*(SettingsDialog&)> hotkeysPage;
    std::shared_ptr<agents::InstalledAgents> agents;
    Executor* worker = nullptr;
};

// Adds the page for destination `walkthroughs` to the context's SettingsPageRegistry, with its
// rail value and the names of its controls. Throws std::invalid_argument when it is registered
// already.
void registerWalkthroughsPage(AppContext& context, WalkthroughsOptions options = {});

} // namespace fv
