#pragma once

#include <QDialog>
#include <QString>

#include "ui_meeting/ai_insights_copy.h"

namespace fv {

class ScrollingBody;

// The AI insights consent (ui_qt/dialogs/meeting_consent_dialog.py). OpenWhisper_CPP asks it for
// each meeting and names the destination each time; nothing is stored that would let a later
// meeting send text without it. `backup` asks the same for the backup agent, which is authorized on
// its own route.
class MeetingConsentDialog final : public QDialog {
public:
    static constexpr const char* kResultCancel = "cancel";
    static constexpr const char* kResultEnable = "enable";

    explicit MeetingConsentDialog(const MeetingConsentCopy&, QWidget* parent = nullptr,
                                  bool backup = false);

    // "enable" after the enable button, else "cancel" (result_action).
    QString resultAction() const { return resultAction_; }
    bool remote() const { return remote_; }
    // The installed agent named in the copy, "" for the built-in engine.
    QString agentId() const { return agentId_; }

    void setVisible(bool visible) override;

    // The body's text for a copy (_location_text, _backup_text and the fixed paragraphs).
    static QString bodyText(const MeetingConsentCopy&);

private:
    QString resultAction_ = QString::fromLatin1(kResultCancel);
    bool remote_ = true;
    QString agentId_;
    ScrollingBody* body_ = nullptr;
    bool fitted_ = false;
};

} // namespace fv
