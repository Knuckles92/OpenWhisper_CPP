#include "ui_record/scratchpad_transforms.h"

#include <exception>
#include <typeinfo>

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "settings/store.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.scratchpad"};

// The status line for `ms`, then the word count again (ScratchpadWindow::notice, which is
// private).
void notice(ScratchpadWindow& window, const QString& text) {
    window.statusLabel()->setText(text);
    window.noticeTimer()->start(ScratchpadWindow::kNoticeMs);
}

} // namespace

// One window's transform in progress: the text it will replace, and the token that tells which
// answer is the newest. The worker holds only a weak reference to it, never the window.
struct ScratchpadTransforms::Run {
    QPointer<ScratchpadWindow> window;
    QTextCursor cursor;
    QString name;
    qt::TokenGate gate;
};

ScratchpadTransforms::ScratchpadTransforms(TransformEnvironment environment)
    : environment_(std::move(environment)),
      rewrite_(std::make_shared<const TransformEnvironment::Rewriter>(environment_.rewrite)),
      context_(std::make_unique<QObject>()) {}

ScratchpadTransforms::~ScratchpadTransforms() = default;

ScratchpadWindow::TransformMenuFiller ScratchpadTransforms::filler() {
    return [self = shared_from_this()](QMenu* menu, ScratchpadWindow& window) {
        const std::vector<text::Transform> transforms =
            text::load_transforms(*self->environment_.settings->snapshot());
        if (transforms.empty()) {
            menu->addAction(QStringLiteral("No transforms yet"))->setEnabled(false);
            return;
        }
        for (const text::Transform& transform : transforms) {
            QAction* action = menu->addAction(QString::fromStdString(transform.name));
            QObject::connect(action, &QAction::triggered, menu,
                             [self, window = QPointer<ScratchpadWindow>(&window), transform] {
                                 if (window != nullptr) {
                                     self->apply(*window, transform);
                                 }
                             });
        }
    };
}

std::shared_ptr<ScratchpadTransforms::Run> ScratchpadTransforms::runOf(ScratchpadWindow& window) {
    std::shared_ptr<Run>& run = runs_[&window];
    if (run == nullptr) {
        run = std::make_shared<Run>();
        run->window = &window;
        QObject::connect(&window, &QObject::destroyed, context_.get(),
                         [this, key = &window] { runs_.erase(key); });
    }
    return run;
}

void ScratchpadTransforms::apply(ScratchpadWindow& window, const text::Transform& transform) {
    QPlainTextEdit* editor = window.editor();
    QTextCursor cursor(editor->textCursor());
    if (!cursor.hasSelection()) {
        cursor.select(QTextCursor::Document);
    }
    // selectedText() separates paragraphs with U+2029.
    QString text = cursor.selectedText();
    text.replace(QChar(0x2029), QLatin1Char('\n'));
    if (text.trimmed().isEmpty()) {
        notice(window, QStringLiteral("Write or dictate something to transform"));
        return;
    }
    // The result replaces exactly the text read here. Anchored at its start and keeping its end,
    // the cursor leaves out text added at either edge meanwhile, such as an action's answer
    // (showScratchpadText).
    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.setKeepPositionOnInsert(true);

    const std::shared_ptr<Run> run = runOf(window);
    const std::uint64_t token = run->gate.next();
    run->cursor = cursor;
    run->name = QString::fromStdString(transform.name);
    editor->setReadOnly(true);
    window.transformButton()->setEnabled(false);
    window.statusLabel()->setText(QStringLiteral(u"Applying %1…").arg(run->name));
    window.noticeTimer()->stop();

    Json settings = *environment_.settings->snapshot();
    kLog.info("Scratchpad transform started ({} characters)", text.toUcs4().size());

    // The worker holds neither the window nor this object: the rewriter and what it needs are
    // copies, and the way back is a weak reference to the run, which the window's deletion drops.
    environment_.jobs->post([rewrite = rewrite_, ui = environment_.ui,
                             weak = std::weak_ptr<Run>(run), gate = run->gate, token,
                             text = qt::toUtf8(text), instruction = transform.instruction,
                             settings = std::move(settings)] {
        std::string result;
        std::string error;
        try {
            auto [rewritten, message] = (*rewrite)(text, instruction, settings);
            result = std::move(rewritten);
            error = message.value_or("");
        } catch (const std::exception& failure) {
            // Expected failures come back as the message; this is a bug, and what it says could
            // quote the draft.
            kLog.warning("Scratchpad transform failed ({})", typeid(failure).name());
            error = "Couldn't transform the text";
        } catch (...) {
            kLog.warning("Scratchpad transform failed (unknown)");
            error = "Couldn't transform the text";
        }
        ui->post([weak, gate, token, result = qt::toQString(result), error = qt::toQString(error)] {
            const std::shared_ptr<Run> run = weak.lock();
            if (run == nullptr || run->window == nullptr || !gate.isCurrent(token)) {
                return;
            }
            ScratchpadWindow& window = *run->window;
            QTextCursor cursor = std::move(run->cursor);
            run->cursor = QTextCursor();
            window.editor()->setReadOnly(false);
            window.transformButton()->setEnabled(true);
            if (!error.isEmpty() || result.trimmed().isEmpty()) {
                notice(window,
                       error.isEmpty() ? QStringLiteral("The transform returned nothing") : error);
                return;
            }
            if (!cursor.isNull()) {
                cursor.setKeepPositionOnInsert(false); // so it ends after the result
                cursor.insertText(result);
                window.editor()->setTextCursor(cursor);
            }
            const QString undo =
                QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText);
            notice(window, undo.isEmpty()
                               ? QStringLiteral("%1 applied").arg(run->name)
                               : QStringLiteral(u"%1 applied · %2 to undo").arg(run->name, undo));
        });
    });
}

} // namespace fv
