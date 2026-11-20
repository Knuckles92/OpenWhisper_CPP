#pragma once

// The Remote engine widgets the reference builders take out of OpenWhisper's trees
// (tools/parity/builders/a08.py _prune, _prune_upload and transcription_stats_filled detach
// remote_model_field, remote_engine, remote_runtime_label, remote_manage_button, link_glyph,
// serving_row and the stats strip's detail label). Each is hidden in every state those builders
// render, so nothing visible differs; the port builds them too, and the tests take them out of
// its tree the same way before comparing.

#include <initializer_list>

#include <QObject>
#include <QWidget>

#include "ui_record/stats_display.h"
#include "ui_record/transcription_tab_base.h"
#include "ui_widgets/remote_engine_controls.h"
#include "ui_widgets/remote_link_glyph.h"
#include "ui_widgets/remote_model_notice.h"
#include "ui_widgets/wrapped_label.h"

namespace fvtest {

// Moves `widgets` under a hidden holder that is deleted with `owner`, so they leave the owner's
// tree (and its layouts) while every pointer the owner keeps to them stays good.
inline void detachFromTree(QObject& owner, std::initializer_list<QWidget*> widgets) {
    auto* holder = new QWidget;
    for (QWidget* widget : widgets) {
        if (widget != nullptr) {
            widget->setParent(holder);
        }
    }
    QObject::connect(&owner, &QObject::destroyed, [holder] { delete holder; });
}

inline void detachRemoteDetail(fv::TranscriptionStatsWidget& stats) {
    detachFromTree(stats, {stats.remoteDetailLabel()});
}

inline void detachRemoteEngine(fv::TranscriptionTabBase& tab) {
    detachFromTree(tab, {tab.remoteModelField(), tab.remoteEngine(), tab.remoteRuntimeLabel(),
                         tab.remoteManageButton(), tab.linkGlyph(), tab.servingRow()});
    if (tab.statsWidget() != nullptr) {
        detachRemoteDetail(*tab.statsWidget());
    }
}

} // namespace fvtest
