#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractAnimation>
#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/manual_executor.h"
#include "review_support.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_shell/main_window.h"
#include "ui_shell/page_fit.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/side_panel_host.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_geometry.h"
#include "walkthrough/finish.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_shot_image.h"

// The main window with the Walkthrough tab registered through ScreenRegistry, as OpenWhisper's
// MainWindow builds it, for the window and stage width tests (tests/test_walkthrough_storyboard.py
// and tests/test_window_fits_tab.py): a data folder of its own, in-memory settings, the shell
// feature, a stand-in Quick Record page with Quick Record's scroll-area structure, and a stand-in
// side panel with the History sidebar's width machinery.
namespace fvtest {

namespace wt = fv::walkthrough;

// _large_capture: two screenshots as large as a real screen's, so previews can take any size.
struct LargeCapture {
    fv::testkit::TempDataRoot tmp;
    std::shared_ptr<wt::FakeRenderer> renderer = std::make_shared<wt::FakeRenderer>();
    ScopedRenderer installed{renderer};
    std::filesystem::path folder;
    std::filesystem::path root;

    LargeCapture() {
        using namespace std::chrono_literals;
        folder = tmp.path() / "captures" / "2026-10-30_14-15-02";
        root = folder.parent_path();
        std::filesystem::create_directories(folder);
        wt::WalkthroughSession::Options options;
        options.started_at =
            std::chrono::local_days{std::chrono::year{2026} / 10 / 30} + 14h + 15min + 2s;
        options.folder = folder;
        wt::WalkthroughSession session(std::move(options));
        session.add_shot(std::make_shared<wt::FakeShotImage>(1600, 1000, 0xe8845c), 14.0);
        session.add_shot(std::make_shared<wt::FakeShotImage>(1600, 1000, 0xe8845c), 36.0);
        const std::vector<std::string> words{"On billing, Change plan stalls for three seconds.",
                                             "Then the invoices table hangs on its last rows.",
                                             "That's it."};
        wt::build_documents(session, session.saved_shots(), words, words, 50.0, {},
                            fv::Json::object());
        session.end();
    }
};

// Quick Record's structure: a QVBoxLayout holding a resizable QScrollArea of fixed rows.
class QuickStandIn : public QWidget {
public:
    explicit QuickStandIn(QWidget* parent = nullptr) : QWidget(parent) {
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        scroll_ = new QScrollArea();
        scroll_->setWidgetResizable(true);
        scroll_->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget();
        auto* rows = new QVBoxLayout(content);
        rows->setContentsMargins(24, 6, 24, 16);
        rows->setSpacing(12);
        for (int height : {200, 160}) {
            auto* row = new QFrame();
            row->setFixedHeight(height);
            rows->addWidget(row);
        }
        rows->addStretch(1);
        scroll_->setWidget(content);
        outer->addWidget(scroll_);
    }
    QScrollArea* scrollArea() const { return scroll_; }

private:
    QScrollArea* scroll_;
};

// HistorySidebar's width machinery: 0 px collapsed, 380 px expanded, animated, every frame
// announced through widthAnimated.
class StandInSidePanel : public QWidget, public fv::SidePanel {
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    static constexpr int kExpandedWidth = fv::window_size::kHistorySidebarWidth;

    explicit StandInSidePanel(QWidget* parent = nullptr)
        : QWidget(parent), animation_(new QPropertyAnimation(this, "sidebarWidth", this)) {
        setFixedWidth(0);
        animation_->setDuration(fv::kCollapseDurationMs);
        animation_->setEasingCurve(fv::kCollapseEasing);
        connect(animation_, &QPropertyAnimation::finished, this,
                [this] { setFixedWidth(expanded_ ? kExpandedWidth : 0); });
    }

    bool isExpanded() const override { return expanded_; }
    void toggle() override {
        expanded_ = !expanded_;
        animation_->stop();
        animation_->setStartValue(width_);
        animation_->setEndValue(expanded_ ? kExpandedWidth : 0);
        animation_->start();
    }
    QAbstractAnimation* widthAnimation() const override { return animation_; }

    int sidebarWidth() const { return width_; }
    void setSidebarWidth(int width) {
        width_ = width;
        setFixedWidth(width);
        Q_EMIT widthAnimated(width);
    }

Q_SIGNALS:
    void widthAnimated(int width);

private:
    QPropertyAnimation* animation_;
    int width_ = 0;
    bool expanded_ = false;
};

class StandInSideTab : public QPushButton, public fv::SidePanelEdgeTab {
    Q_OBJECT
public:
    StandInSideTab() { setFixedWidth(fv::window_size::kHistoryEdgeTabWidth); }
    void setExpanded(bool) override {}
    void setPanelName(const QString&) override {}
    void setShortcutHint(const QString&) override {}
};

inline bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

class WindowRig {
public:
    struct Options {
        QRect screen{0, 0, 1600, 1000};
        std::optional<int> maxUsableHeight;
        std::optional<QRect> geometry = QRect(40, 40, fv::window_size::kDefaultWidth, 700);
        bool sidePanel = true;
    };

    // Two constructors, not a default argument: GCC cannot value-initialise Options, whose
    // members have initialisers, before the enclosing class is complete.
    explicit WindowRig(std::filesystem::path root) : WindowRig(std::move(root), Options()) {}
    WindowRig(std::filesystem::path root, Options options)
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          settings_(fv::Json::object()), ui_(qApp), context_(services()) {
        shell_.install(context_);
        context_.screens().addScreen(fv::ScreenSpec{fv::order::tab::quick_record,
                                                    QStringLiteral("Quick Record"),
                                                    [](QWidget* parent) -> QWidget* {
                                                        return new QuickStandIn(parent);
                                                    }});
        context_.screens().addScreen(fv::ScreenSpec{
            fv::order::tab::walkthrough, QStringLiteral("Walkthrough"),
            [this, root = std::move(root)](QWidget* parent) -> QWidget* {
                fv::WalkthroughTabOptions made;
                made.capturesRoot = root;
                made.io = &io;
                made.today = [] {
                    return QDate(2026, 10, 30);
                };
                made.platform = "win32";
                tab = new fv::WalkthroughTab(settings_, host.callbacks(), made, parent);
                tab->review()->setQuestion(
                    [](QWidget*, const QString&, const QString&, QMessageBox::StandardButtons,
                       QMessageBox::StandardButton) { return QMessageBox::Yes; });
                // What the walkthrough feature's registration does with fills_height.
                if (window) {
                    window->setPageElastic(tab, tab->fillsHeight());
                }
                return tab;
            },
            "walkthrough"});
        window = std::make_unique<fv::MainWindow>(context_);
        const QRect screen = options.screen;
        window->availableScreenRect = [this, screen] {
            return screenOverride.value_or(screen);
        };
        if (options.maxUsableHeight) {
            window->maxUsableHeight = [height = *options.maxUsableHeight] {
                return height;
            };
        }
        if (options.sidePanel) {
            panel = new StandInSidePanel;
            window->setSidePanel(panel, new StandInSideTab);
            QObject::connect(panel, &StandInSidePanel::widthAnimated, window.get(),
                             &fv::MainWindow::sidePanelWidthAnimated);
        }
        if (options.geometry) {
            window->setGeometry(*options.geometry);
        }
        window->show();
        settle();
    }
    ~WindowRig() {
        if (tab) {
            tab->review()->setDirty(false);
        }
        window->close();
        window.reset();
        QApplication::processEvents();
    }
    WindowRig(const WindowRig&) = delete;
    WindowRig& operator=(const WindowRig&) = delete;

    // _settle: the window has finished fitting itself, the side panel is still, and what the tab
    // handed its reader has come back.
    void settle() {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 800) {
            io.run_all();
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        waitFor([this] {
            io.run_all();
            const QAbstractAnimation* moving = panel != nullptr ? panel->widthAnimation() : nullptr;
            return !window->pageFit().scheduled() && !window->pageFit().animationRunning() &&
                   (moving == nullptr || moving->state() != QAbstractAnimation::Running);
        });
        QApplication::processEvents();
    }

    void openTab(int index) {
        window->tabbedContent()->setCurrentIndex(index);
        settle();
    }

    // _toggle_panel.
    void togglePanel() {
        window->toggleSidePanel();
        settle();
    }

    fv::MemorySettingsStore& settings() { return settings_; }

    fv::ManualExecutor io;
    Host host;
    QPointer<fv::WalkthroughTab> tab;
    StandInSidePanel* panel = nullptr;
    std::optional<QRect> screenOverride;
    std::unique_ptr<fv::MainWindow> window;

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::testkit::TempDataRoot dataRoot_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
};

} // namespace fvtest
