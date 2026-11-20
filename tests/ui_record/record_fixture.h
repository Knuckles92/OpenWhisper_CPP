#pragma once

#include <memory>

#include <QApplication>
#include <QWidget>

#include "settings/testing/memory_store.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/quick_record_tab.h"

namespace fvtest {

// A Quick Record tab over settings in memory, shown at the size the Python tests give it.
class ShownTab {
public:
    explicit ShownTab(const fv::Json& saved = fv::Json::object()) : settings(saved) {
        tab = std::make_unique<fv::QuickRecordTab>(settings, fv::speechEngineCatalog());
        tab->resize(680, 500);
        tab->show();
        QApplication::processEvents();
    }
    ~ShownTab() {
        tab->close();
        tab.reset();
        QApplication::processEvents();
    }
    ShownTab(const ShownTab&) = delete;
    ShownTab& operator=(const ShownTab&) = delete;

    fv::QuickRecordTab* operator->() const { return tab.get(); }
    fv::QuickRecordTab& operator*() const { return *tab; }

    fv::MemorySettingsStore settings;
    std::unique_ptr<fv::QuickRecordTab> tab;
};

} // namespace fvtest
