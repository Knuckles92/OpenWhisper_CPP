#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "base/json.h"

#include "ui_overlay/moments_painter.h"
#include "ui_overlay/walkthrough_controls.h"
#include "uimath/overlay_layout.h"
#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

class QFont;
class QPainter;
class QRectF;
class QTimer;

namespace fv {

class Clock;
class DragToMove;
class HotkeyHoverHint;
class SettingsStore;
class Visual;
struct Moment;

// The recording overlay (ui_qt/overlays/waveform_overlay.py): a small floating pill that opens
// by the pointer when dictation starts and shows what the app is doing: the recording look and
// its live label, the look's own Processing and Transcribing, Cleaning up, the one-shot moments,
// notices and captions. With live preview words it grows a header (or a footer) for its look and
// shows the words under it; a language chip shows the dictation language when there are two or
// more, and a walkthrough adds Snap and Done along the badge row. Everything is painted. It is a
// top-level window that never takes focus, because any activation would take the paste target away;
// in preview mode it is an ordinary child widget that a Settings page lays out, never moved and
// never clicked. Times come from the injected clock's steady time, as Python reads
// time.monotonic(). Affinity: GUI thread.
class WaveformOverlay : public QWidget {
    Q_OBJECT

public:
    static inline const QString kStateIdle = QStringLiteral("idle");
    static inline const QString kStateRecording = QStringLiteral("recording");
    static inline const QString kStateStreaming = QStringLiteral("streaming");
    static inline const QString kStateCommandListening = QStringLiteral("command_listening");
    static inline const QString kStateProcessing = QStringLiteral("processing");
    static inline const QString kStateTranscribing = QStringLiteral("transcribing");
    static inline const QString kStateCleaning = QStringLiteral("cleaning");
    static inline const QString kStateRewriting = QStringLiteral("rewriting");
    static inline const QString kStateCanceling = QStringLiteral("canceling");
    static inline const QString kStateSttEnable = QStringLiteral("stt_enable");
    static inline const QString kStateSttDisable = QStringLiteral("stt_disable");
    static inline const QString kStateCopied = QStringLiteral("copied");
    static inline const QString kStateLanguage = QStringLiteral("language");
    static inline const QString kStateNotice = QStringLiteral("notice");
    static inline const QString kStateLargeFileSplitting = QStringLiteral("large_file_splitting");

    // How long a caption stays, and the base of a notice's time on screen (ms).
    static constexpr int kCaptionMs = 2500;
    // Extra reading time per character of a notice, on top of kCaptionMs.
    static constexpr int kNoticeMsPerChar = 25;
    // The fade out when the text lands, and the stage label's crossfade (s).
    static constexpr double kFadeOutS = 0.25;
    static constexpr double kLabelCrossfadeS = 0.28;
    // A walkthrough's or a snap's distance from the screen's corner (px).
    static constexpr int kParkMargin = 16;

    // How long the language chip flashes after a change while it is on screen (s).
    static constexpr double kLanguageFlashS = 0.6;

    // The standalone floating overlay, or the preview a Settings page shows (a plain child
    // widget the parent's layout places, transparent for the mouse).
    enum class Mode { Standalone, Preview };

    // Whether a live look shows its dot, its "Recording" words and its clock.
    struct LabelParts {
        bool dot = true;
        bool text = true;
        bool clock = true;
        friend bool operator==(const LabelParts&, const LabelParts&) = default;
    };

    // settings is read by refreshRecordingLook (null reads as an empty settings file); clock
    // defaults to the system clock. Both must outlive the overlay.
    explicit WaveformOverlay(const SettingsStore* settings = nullptr, const Clock* clock = nullptr);
    // Mode::Preview draws as a child of parent (Python's preview=True); Mode::Standalone is the
    // floating window of the other constructor, and parent is then ignored.
    WaveformOverlay(QWidget* parent, Mode mode, const SettingsStore* settings = nullptr,
                    const Clock* clock = nullptr);
    ~WaveformOverlay() override;

    bool isPreview() const { return preview_; }

    // Shows the overlay by the pointer, kept on its screen, in state; without one, the current
    // state, or recording when idle. A dragged or parked overlay stays where it was put.
    void showAtCursor(std::optional<QString> state = std::nullopt);
    void setState(const QString& state);
    const QString& currentState() const { return currentState_; }

    // The microphone's RMS levels (0-1); the first 20 count.
    void updateAudioLevels(const QList<float>& levels);
    // A custom Processing label, such as "Retrying connection (2/3)...".
    void setProcessingLabel(const QString& text);
    // An action's outcome near the pointer, even while hidden; a recording or job already on
    // screen keeps its overlay and gets the text as a caption. done plays the finished check.
    void showNotice(const QString& text, bool done = false);
    // A short line, such as a microphone switch, in the label's place; ignored while hidden.
    void showCaption(const QString& text);

    // A push-and-hold recording latched on. Kept while hidden; cleared when the recording ends,
    // the next one starts, or the overlay hides.
    void setHandsFree(bool on);
    bool handsFree() const { return handsFree_; }
    // The capture's destination, kept visible through its transcription and cleanup.
    void setRecordingDestination(const QString& destination);
    const QString& recordingDestination() const { return recordingDestination_; }

    // Draws recordings with look (see uimath/overlay_look.h); Classic is "legacy".
    void setRecordingLook(const QString& look);
    const QString& recordingLook() const { return recordingLook_; }
    void setLabelParts(bool dot, bool text, bool clock);
    LabelParts labelParts() const { return labelParts_; }
    // Re-reads the look and its label from settings after Settings or an agent changed them.
    void refreshRecordingLook();

    // The live preview's words so far: stripped (final is ignored, as in Python); the overlay
    // grows to fit them, as far as the room by its anchor allows (400 px at most).
    void updateStreamingText(const QString& text, bool final = true);
    void clearStreamingText();
    const QString& streamingText() const { return streamingText_; }
    // Re-read streaming_overlay_font_size and streaming_overlay_layout, reflowing any words.
    void refreshStreamingFontSize();
    void refreshStreamingLayout();
    PreviewLayout streamingLayout() const { return streamingLayout_; }
    int streamingFontSize() const { return streamingFontSize_; }

    // The active dictation language and the choices; the chip shows with two or more choices
    // while listening. A change while shown flashes the chip; a hidden overlay takes it quietly.
    void setLanguage(const QString& code, const QStringList& choices);
    const QString& language() const { return language_; }
    const QStringList& languageChoices() const { return languageChoices_; }
    // Briefly shows the active language near the pointer; a repeat replays it.
    void showLanguageNotice();

    // The size of the file being split, for the splitting state's line (MB).
    void setLargeFileInfo(double mb);

    // Sits in area's top-left corner until unpark, as a snap covering the screen needs; a
    // hidden overlay is not parked.
    void park(const QRect& area);
    // Goes back to where the overlay was before park.
    void unpark();

    // Offers Snap and Done along the badge row while a walkthrough records (where the platform
    // takes overlay clicks), and sends the overlay to its screen's top-left corner, out of what
    // is being explained, until it is dragged elsewhere or hides.
    void setWalkthroughControls(bool on);
    bool walkthroughControls() const { return walkthroughControls_; }
    // The Screenshot and Walkthrough shortcuts that Snap and Done press, shown in a pill above
    // the hovered control; empty shows none.
    void setWalkthroughShortcuts(const QString& snap, const QString& finish);
    // Lets another window (the walkthrough's column) show the recording and its processing:
    // while showing() is true the overlay keeps to notices and moments, and a recording on
    // screen hides at once. An empty function ends that.
    void setStandIn(std::function<bool()> showing);

    // The frame timer: 33 ms ticks while the overlay is not idle.
    QTimer& timer() { return *timer_; }

public Q_SLOTS:
    // Hides the overlay; a result being worked on (Processing to Rewriting) fades out first.
    // A new state or notice cancels the fade.
    void hide();

Q_SIGNALS:
    void stateChanged(const QString& state);
    // The language chip, Snap or Done was clicked (where the platform takes overlay clicks).
    void languageCycleRequested();
    void walkthroughSnapRequested();
    void walkthroughFinishRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    friend struct WaveformOverlayTestAccess;

    void init();
    double now() const;
    // The saved settings, or an empty mapping without a store.
    std::shared_ptr<const Json> savedSettings() const;
    void updateAnimation();
    void beginWorkStage();
    void restartAnimation(const QString& state);
    void followListening(const QString& state, bool wasListening);
    void finishHide();
    void endListeningExtras();
    void clearCaption();
    int noticeMs() const;
    QString fitNotice(const QFont& font, const QRect& rect) const;

    // Placement.
    std::optional<QRect> availableGeometryForAnchor() const;
    void repositionNearAnchor();
    int effectiveStreamingMaxHeight() const;
    void applyHeight();
    bool dragged() const;
    bool placed() const;

    // The language chip and the pointer.
    void refreshLanguage();
    QString languageText() const;
    QString languageName() const;
    double languageFlash() const;
    double chipWidth() const;
    bool chipVisible() const;
    bool platformTakesOverlayClicks() const;
    bool chipClickable() const;
    bool chipHit(const QPointF& position) const;
    // What a click at position presses: "language", "snap", "finish" or "".
    QString hit(const QPointF& position) const;
    void setHover(const QString& target);

    // Snap and Done.
    bool controlsClickable() const;
    std::vector<PlacedControl> layoutControls() const;
    void drawWalkthroughControls(QPainter& painter);
    // The state is one a stand-in shows in the overlay's place, and it is showing now.
    bool stoodIn(const QString& state) const;

    // Painting.
    void drawState(QPainter& painter);
    void drawBackground(QPainter& painter);
    void drawListeningBand(QPainter& painter, const QString& status, std::string_view statusToken,
                           double top);
    void drawParticles(QPainter& painter, double top);
    std::string_view liveTone() const;
    void drawLiveListening(QPainter& painter, const QString& label, std::string_view textTone,
                           double top);
    void drawWork(QPainter& painter);
    void drawWorkLabel(QPainter& painter);
    QString workLabel() const;
    void drawLabelAt(QPainter& painter, const QString& text, double alpha, double dy);
    QRect statusRect() const;
    void drawStatus(QPainter& painter, const QString& text, const QColor& color);
    bool badgeShown() const;
    TopRowInput topRowInput() const;
    bool topRowInUse() const;
    int bandTop() const;
    // The live label's width without words, for the header's fit; nullopt without one.
    std::optional<double> liveLabelWidth() const;
    bool liveSharesTopRow() const;
    int headerHeight() const;
    Margins previewTextMargins() const;
    QFont streamingPreviewFont() const;
    void drawStreamingState(QPainter& painter, const QString& status, std::string_view statusToken);
    void drawPreviewHeader(QPainter& painter, std::string_view textTone);
    void drawSmallLook(QPainter& painter, const QRectF& area);
    void drawDivider(QPainter& painter, double y);
    void drawStreamingPreviewText(QPainter& painter, const QRect& textRect);
    QRectF drawLanguageChip(QPainter& painter);
    void drawLanguageState(QPainter& painter);
    void drawLargeFileSplittingState(QPainter& painter);
    void drawListeningExtras(QPainter& painter);
    QString badgeText() const;
    double badgeWidth() const;
    void drawModeBadge(QPainter& painter);
    void drawCaption(QPainter& painter);
    void drawNoticeState(QPainter& painter);

    // The moments (moments_painter.cpp).
    void drawCleaningState(QPainter& painter, const QString& text);
    const Moment* moment() const;
    QPointF momentCenter() const;
    int transientMs() const;
    static double cancelSeconds();
    void drawCanceled(QPainter& painter);
    void drawMoment(QPainter& painter, const QString& text);

    const SettingsStore* settings_;
    const Clock* clock_;
    std::mt19937 rng_;
    bool preview_ = false;
    // Tests stand in for the platform's answer (Windows, X11 and macOS take overlay clicks).
    std::optional<bool> takesClicksForTesting_;

    int overlayWidth_;
    int overlayHeight_;
    int baseHeight_;

    QString currentState_ = kStateIdle;
    QString processingLabel_;
    std::vector<double> audioLevels_ = std::vector<double>(20, 0.0);
    double animationTime_ = 0.0;
    double cancelProgress_ = 0.0;
    // The confetti or bubbles of the moment on screen.
    std::vector<Particle> sttParticles_;
    // The pointer the overlay opened by, which keeps it on screen as it grows.
    std::optional<QPoint> anchorPos_;
    // Where it was before park.
    std::optional<QPoint> unparkedAt_;

    std::unique_ptr<ParticleStyle> style_;
    double legacyAudioResponse_ = 0.0;
    VoiceRibbon ribbon_;
    double listenStarted_ = 0.0;
    double defaultEmissionRate_ = 0.0;
    QString recordingLook_;
    std::unique_ptr<Visual> visual_;
    LabelParts labelParts_;
    double workStarted_ = 0.0;
    double workRate_ = 1.0;
    double workClock_ = 0.0;
    QString labelFrom_;
    double labelChanged_ = 0.0;
    std::optional<double> fadingSince_;

    QTimer* timer_ = nullptr;
    int frameRate_ = 0;
    double lastFrameTime_ = 0.0;
    QTimer* hiddenTimer_ = nullptr;

    bool handsFree_ = false;
    QString recordingDestination_;
    QRectF badgeRect_;
    QString streamingText_;
    int streamingFontSize_ = 16;
    PreviewLayout streamingLayout_ = PreviewLayout::Header;
    // The header's room between the live label and the chips, where its small look goes and a
    // caption takes over; set as the header paints.
    QRectF headerRoom_;
    double largeFileSizeMb_ = 0.0;
    QString language_;
    QStringList languageChoices_;
    std::optional<double> languageChangedAt_;
    QRectF chipRect_;
    // What the pointer is over: "language", "snap", "finish" or "".
    QString hover_;
    // Snap and Done while a walkthrough records, and where they were last painted, keyed
    // "snap" and "finish" (empty while they are not on screen).
    bool walkthroughControls_ = false;
    std::vector<std::pair<QString, QRectF>> controlRects_;
    // Their shortcuts, keyed as the rectangles, and the pill that shows one.
    QString snapShortcut_;
    QString finishShortcut_;
    QPointer<HotkeyHoverHint> hotkeyHint_;
    // A walkthrough keeps the overlay in its screen's top-left corner until dragged or hidden.
    bool cornered_ = false;
    std::function<bool()> standIn_;
    // What the preview header decided as it last painted.
    HeaderRow headerRow_;
    QString caption_;
    double captionStarted_ = 0.0;
    QString notice_;
    bool noticeDone_ = false;
    bool burstFired_ = false;
    std::vector<CollapsingDot> collapseFrom_;
    QTimer* captionTimer_ = nullptr;
    std::unique_ptr<DragToMove> drag_;
    bool paintErrorLogged_ = false;
};

} // namespace fv
