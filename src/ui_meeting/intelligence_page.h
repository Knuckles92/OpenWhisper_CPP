#pragma once

#include <functional>
#include <string>

#include <QWidget>

#include "appkit/settings_page.h"
#include "base/cancel.h"

class QComboBox;
class QLineEdit;

namespace fv {

class AgentPicker;
class AppContext;
class SettingsPage;
class SettingsSwitch;
class TextModelPicker;

class IntelligencePage final : public QWidget, public SettingsPageModule {
public:
    using FolderChooser = std::function<QString(QWidget*, const QString&, const QString&)>;
    IntelligencePage(SettingsPage&, AppContext&, FolderChooser chooseFolder = {});
    // The chooser a page built without one uses (tests); empty: QFileDialog.
    static FolderChooser& folderChooserSeam();
    ~IntelligencePage() override;
    void loadSettings(const Json&) override;
    QWidget* namedControl(std::string_view) override;

private:
    void save(std::string_view key, Json value);
    void saveFolder(const QString& path);
    // The chosen folder as os.path.normpath writes it.
    static QString normalizedFolder(const QString& path);
    void assignModel(const QString&);
    void refreshModels();
    // The tile's description, the thinking levels and their note for `core`.
    void refreshRunner(const Json& settings, const std::string& core);
    void editEndpoint(bool editing);
    void deleteEndpoint(const QString& id);

    SettingsPage& page_;
    AppContext& context_;
    AgentPicker* agents_ = nullptr;
    TextModelPicker* models_ = nullptr;
    QComboBox* backup_ = nullptr;
    QComboBox* live_ = nullptr;
    QComboBox* final_ = nullptr;
    SettingsSwitch* recall_ = nullptr;
    SettingsSwitch* folderEnabled_ = nullptr;
    QLineEdit* folder_ = nullptr;
    std::string lastBuiltin_ = "pi";
    QString builtinDescription_;
    CancelSource loading_;
};

void registerIntelligencePage(AppContext&);

} // namespace fv
