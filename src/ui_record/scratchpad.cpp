#include "ui_record/scratchpad.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScreen>
#include <QSizeGrip>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/scratchpad_text.h"
#include "focus/join.h"
#include "focus/snapshot.h"
#include "pyport/number.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_record/python_exception_name.h"
#include "ui_record/scratchpad_saver.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.scratchpad"};

QString fromUtf8(const std::string& text) {
    return qt::toQString(text);
}

// bytes.decode("utf-8", errors="replace"), as _load's open() reads the notes: a leading BOM is
// a character (U+FEFF, saved back as it came), and each maximal subpart of an ill-formed
// sequence is one U+FFFD (Qt's decoder gives one per byte of a truncated sequence and skips the
// BOM). Well-formed sequences are those of the Unicode standard's table 3-7.
QString decodeNotes(std::string_view bytes) {
    constexpr char16_t kReplacement = 0xFFFD;
    QString out;
    out.reserve(static_cast<qsizetype>(bytes.size()));
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            out.append(QChar(lead));
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0Fu;
            low = lead == 0xE0 ? 0xA0 : 0x80;  // no overlong form
            high = lead == 0xED ? 0x9F : 0xBF; // no surrogate
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07u;
            low = lead == 0xF0 ? 0x90 : 0x80;  // no overlong form
            high = lead == 0xF4 ? 0x8F : 0xBF; // nothing past U+10FFFF
        } else {
            out.append(QChar(kReplacement));
            ++i;
            continue;
        }
        std::size_t taken = 1;
        while (taken < length && i + taken < bytes.size()) {
            const auto next = static_cast<unsigned char>(bytes[i + taken]);
            if (next < low || next > high) {
                break;
            }
            code = (code << 6) | (next & 0x3Fu);
            low = 0x80;
            high = 0xBF;
            ++taken;
        }
        if (taken < length) {
            out.append(QChar(kReplacement));
        } else if (code > 0xFFFF) {
            out.append(QChar(QChar::highSurrogate(code)));
            out.append(QChar(QChar::lowSurrogate(code)));
        } else {
            out.append(QChar(static_cast<char16_t>(code)));
        }
        i += taken;
    }
    return out;
}

// type(exc).__name__ of the OSError Python's open() or read() would raise for the errno a
// failed stream left.
std::string readFailureName(int error) {
    return pythonExceptionName(std::system_error(std::error_code(error, std::generic_category())));
}

} // namespace

// The title strip of the frameless window; dragging it moves the window (_Header).
class ScratchpadWindow::Header : public QFrame {
public:
    explicit Header(QWidget* window) : QFrame(window), window_(window) {
        setObjectName(QStringLiteral("scratchpadHeader"));
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            dragOffset_ = event->globalPosition().toPoint() - window_->frameGeometry().topLeft();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragOffset_ && (event->buttons() & Qt::LeftButton)) {
            window_->move(event->globalPosition().toPoint() - *dragOffset_);
            event->accept();
            return;
        }
        QFrame::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        dragOffset_.reset();
        setCursor(Qt::OpenHandCursor);
        QFrame::mouseReleaseEvent(event);
    }

private:
    QWidget* window_;
    std::optional<QPoint> dragOffset_;
};

ScratchpadWindow::ScratchpadWindow(ScratchpadEnvironment environment, CopyText copyText,
                                   QWidget* parent)
    : QWidget(parent), environment_(std::move(environment)), copyText_(std::move(copyText)) {
    transformMenuFiller_ = [](QMenu* menu, ScratchpadWindow&) {
        menu->addAction(QStringLiteral("No transforms yet"))->setEnabled(false);
    };
    saver_ = std::make_unique<ScratchpadSaver>(*environment_.persistence, environment_.file);

    setObjectName(QStringLiteral("scratchpadWindow"));
    setWindowTitle(QStringLiteral("Scratchpad"));
    // Hidden in the tray, closing this must not quit the app.
    setAttribute(Qt::WA_QuitOnClose, false);
    // Dictation and the in-app shortcut fallback treat it as the application's own.
    ui::setStyleProperty(this, ui::prop::fiorivoceHotkeyWindow, true);
    applyWindowFlags(environment_.settings->get(keys::scratchpad_always_on_top));
#ifdef Q_OS_MACOS
    // A Tool window hides whenever another app is frontmost; the Scratchpad sits beside the app
    // being worked in.
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    setAttribute(Qt::WA_TranslucentBackground);

    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setSingleShot(true);
    autosaveTimer_->setInterval(kAutosaveMs);
    connect(autosaveTimer_, &QTimer::timeout, this, [this] { saveSoon(); });
    geometryTimer_ = new QTimer(this);
    geometryTimer_->setSingleShot(true);
    geometryTimer_->setInterval(kGeometrySaveMs);
    connect(geometryTimer_, &QTimer::timeout, this, [this] { saveWindowPlace(); });
    noticeTimer_ = new QTimer(this);
    noticeTimer_->setSingleShot(true);
    connect(noticeTimer_, &QTimer::timeout, this, [this] { showWordCount(); });
    undoTimer_ = new QTimer(this);
    undoTimer_->setSingleShot(true);
    undoTimer_->setInterval(kUndoClearMs);
    connect(undoTimer_, &QTimer::timeout, this, [this] { forgetCleared(); });

    build();
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this] { flush(); });
    }
}

ScratchpadWindow::~ScratchpadWindow() = default;

void ScratchpadWindow::applyWindowFlags(bool onTop) {
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
    if (onTop) {
        flags |= Qt::WindowStaysOnTopHint;
    }
    setWindowFlags(flags);
}

void ScratchpadWindow::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    surface_ = new QFrame;
    surface_->setObjectName(QStringLiteral("scratchpadSurface"));
    ui::setStyleProperty(surface_, ui::prop::framed, true);
    outer->addWidget(surface_);
    auto* layout = new QVBoxLayout(surface_);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    header_ = new Header(this);
    auto* row = new QHBoxLayout(header_);
    row->setContentsMargins(14, 8, 8, 8);
    row->setSpacing(8);
    auto* icon = new QLabel;
    icon->setObjectName(QStringLiteral("scratchpadIcon"));
    icon->setPixmap(ui::designIcon(QStringLiteral("notes-blue.svg")).pixmap(16, 16));
    row->addWidget(icon);
    auto* title = new QLabel(QStringLiteral("Scratchpad"));
    title->setObjectName(QStringLiteral("scratchpadTitle"));
    row->addWidget(title);
    row->addStretch(1);
    onTopButton_ = new QToolButton;
    onTopButton_->setObjectName(QStringLiteral("scratchpadOnTop"));
    onTopButton_->setText(QStringLiteral("On top"));
    onTopButton_->setCheckable(true);
    onTopButton_->setChecked(environment_.settings->get(keys::scratchpad_always_on_top));
    onTopButton_->setCursor(Qt::PointingHandCursor);
    onTopButton_->setToolTip(QStringLiteral("Keep the Scratchpad above other windows"));
    connect(onTopButton_, &QToolButton::toggled, this, [this](bool on) { setAlwaysOnTop(on); });
    row->addWidget(onTopButton_);
    closeButton_ = new QToolButton;
    closeButton_->setObjectName(QStringLiteral("scratchpadClose"));
    closeButton_->setIcon(ui::designIcon(QStringLiteral("x-gray.svg")));
    closeButton_->setIconSize(QSize(14, 14));
    closeButton_->setCursor(Qt::PointingHandCursor);
    closeButton_->setToolTip(QStringLiteral("Hide the Scratchpad"));
    closeButton_->setAccessibleName(QStringLiteral("Hide the Scratchpad"));
    connect(closeButton_, &QToolButton::clicked, this, [this] { hide(); });
    row->addWidget(closeButton_);
    layout->addWidget(header_);

    editor_ = new QPlainTextEdit;
    editor_->setObjectName(QStringLiteral("scratchpadEditor"));
    editor_->setPlaceholderText(QStringLiteral("Dictate or type notes…"));
    editor_->setFrameShape(QFrame::NoFrame);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] { onTextChanged(); });
    layout->addWidget(editor_, 1);

    auto* footer = new QFrame;
    footer->setObjectName(QStringLiteral("scratchpadFooter"));
    footerRow_ = new QHBoxLayout(footer);
    footerRow_->setContentsMargins(14, 8, 6, 8);
    footerRow_->setSpacing(2);
    statusLabel_ = new ElidingLabel(QString());
    statusLabel_->setObjectName(QStringLiteral("scratchpadStatus"));
    footerRow_->addWidget(statusLabel_, 1);
    transformButton_ = makeAction(QStringLiteral("Transform ▾"), QStringLiteral("wand-purple.svg"));
    connect(transformButton_, &QToolButton::clicked, this, [this] { openTransformMenu(); });
    transformMenu_ = new QMenu(this);
    copyButton_ = makeAction(QStringLiteral("Copy all"), QStringLiteral("copy-gray.svg"));
    connect(copyButton_, &QToolButton::clicked, this, [this] { copyAll(); });
    clearButton_ = makeAction(QStringLiteral("Clear"), QString());
    connect(clearButton_, &QToolButton::clicked, this, [this] { clearOrUndo(); });
    sizeGrip_ = new QSizeGrip(footer);
    sizeGrip_->setObjectName(QStringLiteral("scratchpadGrip"));
    footerRow_->addWidget(sizeGrip_, 0, Qt::AlignBottom | Qt::AlignRight);
    layout->addWidget(footer);
    showWordCount();
}

QToolButton* ScratchpadWindow::makeAction(const QString& text, const QString& icon) {
    auto* button = new QToolButton;
    ui::setStyleProperty(button, ui::prop::scratchpadAction, true);
    button->setText(text);
    if (!icon.isEmpty()) {
        button->setIcon(ui::designIcon(icon));
        button->setIconSize(QSize(15, 15));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    footerRow_->addWidget(button);
    return button;
}

// ---- showing, hiding, geometry ----

void ScratchpadWindow::present() {
    ensureLoaded();
    show();
    raise();
    activateWindow();
    editor_->setFocus(Qt::ActiveWindowFocusReason);
}

void ScratchpadWindow::reveal() {
    ensureLoaded();
    if (!isVisible()) {
        // Qt reads it when showing; cleared after, so the user's own opens (present, the
        // on-top re-show) still activate.
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        show();
        setAttribute(Qt::WA_ShowWithoutActivating, false);
    }
    raise();
}

void ScratchpadWindow::ensureLoaded() {
    if (!loaded_) {
        load();
        restoreWindowPlace();
    }
}

QRect ScratchpadWindow::defaultRect(const QRect& available) const {
    const double scale = ui::currentUiFontScale();
    const int width =
        std::min(static_cast<int>(py::round_half_even(400 * scale)), available.width());
    const int height =
        std::min(static_cast<int>(py::round_half_even(380 * scale)), available.height());
    return QRect(available.right() - width - 40,
                 available.top() + (available.height() - height) / 2, width, height);
}

void ScratchpadWindow::restoreWindowPlace() {
    const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect available =
        screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    QRect rect = defaultRect(available);
    const Json saved = environment_.settings->get(keys::scratchpad_geometry);
    if (saved.is_object() && saved.contains("x") && saved.contains("y") &&
        saved.contains("width") && saved.contains("height")) {
        const auto x = py::py_int(saved.at("x"));
        const auto y = py::py_int(saved.at("y"));
        const auto width = py::py_int(saved.at("width"));
        const auto height = py::py_int(saved.at("height"));
        if (x && y && width && height) {
            rect = QRect(static_cast<int>(*x), static_cast<int>(*y), static_cast<int>(*width),
                         static_cast<int>(*height));
        } else {
            kLog.warning("Ignoring an invalid Scratchpad geometry");
        }
    }
    const QSize minimum = minimumSizeHint();
    const int width = std::max(minimum.width(), std::min(rect.width(), available.width()));
    const int height = std::max(minimum.height(), std::min(rect.height(), available.height()));
    resize(width, height);
    const int x = std::min(std::max(rect.x(), available.left()), available.right() - width + 1);
    const int y = std::min(std::max(rect.y(), available.top()), available.bottom() - height + 1);
    move(x, y);
}

void ScratchpadWindow::saveWindowPlace() {
    const QRect place = geometry();
    const Json value{
        {"x", place.x()}, {"y", place.y()}, {"width", place.width()}, {"height", place.height()}};
    try {
        if (environment_.settings->get(keys::scratchpad_geometry) != value) {
            environment_.settings->save_setting(keys::scratchpad_geometry.key(), value);
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't save the Scratchpad position ({})", typeid(error).name());
    }
}

void ScratchpadWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    if (isVisible()) {
        geometryTimer_->start();
    }
}

void ScratchpadWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (isVisible()) {
        geometryTimer_->start();
    }
}

void ScratchpadWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (geometryTimer_->isActive()) {
        geometryTimer_->stop();
        saveWindowPlace();
    }
    flush();
}

void ScratchpadWindow::closeEvent(QCloseEvent* event) {
    event->accept();
}

void ScratchpadWindow::setAlwaysOnTop(bool on) {
    try {
        environment_.settings->save_setting(keys::scratchpad_always_on_top.key(), on);
    } catch (const std::exception& error) {
        kLog.warning("Couldn't save the Scratchpad's on-top choice ({})", typeid(error).name());
    }
    if (onTopButton_ != nullptr && onTopButton_->isChecked() != on) {
        onTopButton_->setChecked(on);
    }
    const bool wasVisible = isVisible();
    const QRect place = geometry();
    // Changing window flags recreates the native window, which hides it.
    applyWindowFlags(on);
    setGeometry(place);
    if (wasVisible) {
        show();
        raise();
        activateWindow();
    }
}

bool ScratchpadWindow::isActive() const {
    return isVisible() && (activityProbe_ ? activityProbe_() : isActiveWindow());
}

// ---- text, autosave ----

void ScratchpadWindow::load() {
    loaded_ = true;
    const std::filesystem::path file = environment_.file();
    std::string bytes;
    {
        errno = 0;
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            const int openError = errno;
            std::error_code error;
            if (!std::filesystem::exists(file, error) && !error) {
                return;
            }
            kLog.warning("Couldn't read the Scratchpad ({})", readFailureName(openError));
            notice(QStringLiteral("Couldn't open your saved notes"));
            return;
        }
        bool failed = false;
        try {
            bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        } catch (const std::exception&) {
            // libstdc++ opens a folder and throws on the first read (EISDIR); MSVC's open fails.
            failed = true;
        }
        if (failed || stream.bad()) {
            kLog.warning("Couldn't read the Scratchpad ({})", readFailureName(errno));
            notice(QStringLiteral("Couldn't open your saved notes"));
            return;
        }
    }
    QString text = decodeNotes(bytes);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    suppressSave_ = true;
    editor_->setPlainText(text);
    suppressSave_ = false;
    editor_->moveCursor(QTextCursor::End);
    showWordCount();
}

void ScratchpadWindow::onTextChanged() {
    if (!noticeTimer_->isActive()) {
        showWordCount();
    }
    if (suppressSave_) {
        return;
    }
    if (clearedText_ && !editor_->toPlainText().isEmpty()) {
        forgetCleared();
    }
    autosaveTimer_->start();
}

void ScratchpadWindow::saveSoon() {
    saver_->submit(qt::toUtf8(editor_->toPlainText()));
}

void ScratchpadWindow::flush() {
    if (!loaded_) {
        return;
    }
    if (autosaveTimer_->isActive()) {
        autosaveTimer_->stop();
        saver_->writeNow(qt::toUtf8(editor_->toPlainText()));
    }
}

QString ScratchpadWindow::text() const {
    return editor_->toPlainText();
}

void ScratchpadWindow::showWordCount() {
    statusLabel_->setText(fromUtf8(dictation::word_count_text(qt::toUtf8(editor_->toPlainText()))));
}

void ScratchpadWindow::notice(const QString& text, int ms) {
    statusLabel_->setText(text);
    noticeTimer_->start(ms);
}

void ScratchpadWindow::insertDictation(const QString& text) {
    QTextCursor cursor = editor_->textCursor();
    const std::int64_t start = cursor.selectionStart();
    const std::int64_t end = cursor.selectionEnd();
    // Qt's positions count UTF-16 units, as the slices do.
    const std::string document = qt::toUtf8(editor_->toPlainText());
    focus::TextContext context;
    context.before =
        dictation::utf16_slice(document, start - dictation::SCRATCHPAD_CONTEXT_BEFORE, start);
    context.selected = dictation::utf16_slice(document, start, end);
    context.after =
        dictation::utf16_slice(document, end, end + dictation::SCRATCHPAD_CONTEXT_AFTER);
    context.caret_known = true;
    context.selection_known = true;
    context.source = "scratchpad";
    std::string joined = qt::toUtf8(text);
    try {
        joined = joiner_ ? joiner_(joined, context) : focus::join_with_context(joined, context);
    } catch (const std::exception& error) {
        kLog.warning("Couldn't join the dictation to the Scratchpad text ({})",
                     typeid(error).name());
        joined = qt::toUtf8(text);
    }
    cursor.insertText(fromUtf8(dictation::spaced(context.before, joined, context.after)));
    editor_->setTextCursor(cursor);
    editor_->ensureCursorVisible();
}

void ScratchpadWindow::appendBlock(const QString& text) {
    QTextCursor cursor(editor_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(
        fromUtf8(dictation::append_block(qt::toUtf8(editor_->toPlainText()), qt::toUtf8(text))));
    editor_->ensureCursorVisible();
}

// ---- footer actions ----

void ScratchpadWindow::copyAll() {
    const QString text = editor_->toPlainText();
    if (text.isEmpty()) {
        notice(QStringLiteral("Nothing to copy yet"));
        return;
    }
    const bool copied = copyText_ ? copyText_(text) : false;
    notice(copied ? QStringLiteral("Copied") : QStringLiteral("Couldn't copy"));
}

void ScratchpadWindow::clearOrUndo() {
    if (clearedText_) {
        undoClear();
    } else {
        clearText();
    }
}

void ScratchpadWindow::clearText() {
    const QString text = editor_->toPlainText();
    if (text.isEmpty()) {
        return;
    }
    QTextCursor cursor = editor_->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();
    clearedText_ = text;
    clearButton_->setText(QStringLiteral("Undo clear"));
    undoTimer_->start();
}

void ScratchpadWindow::undoClear() {
    const std::optional<QString> text = std::exchange(clearedText_, std::nullopt);
    forgetCleared();
    if (text && !text->isEmpty()) {
        QTextCursor cursor = editor_->textCursor();
        cursor.select(QTextCursor::Document);
        cursor.insertText(*text);
        editor_->setTextCursor(cursor);
    }
}

void ScratchpadWindow::forgetCleared() {
    clearedText_.reset();
    undoTimer_->stop();
    clearButton_->setText(QStringLiteral("Clear"));
}

void ScratchpadWindow::setTransformMenuFiller(TransformMenuFiller filler) {
    transformMenuFiller_ = std::move(filler);
}

void ScratchpadWindow::fillTransformMenu() {
    transformMenu_->clear();
    try {
        if (transformMenuFiller_) {
            transformMenuFiller_(transformMenu_, *this);
            return;
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't load the transforms");
        transformMenu_->clear();
    }
    transformMenu_->addAction(QStringLiteral("No transforms yet"))->setEnabled(false);
}

void ScratchpadWindow::openTransformMenu() {
    fillTransformMenu();
    transformMenu_->popup(transformButton_->mapToGlobal(QPoint(0, transformButton_->height())));
}

// ---- the service and the entry points ----

ScratchpadService::ScratchpadService(ScratchpadEnvironment environment,
                                     ScratchpadWindow::CopyText copyText)
    : environment_(std::move(environment)), copyText_(std::move(copyText)) {}

ScratchpadService::~ScratchpadService() {
    if (releaseHook_) {
        releaseHook_();
    }
    for (const QMetaObject::Connection& connection : connections_) {
        QObject::disconnect(connection);
    }
    links_.clear();
    if (window_ != nullptr) {
        window_->flush();
    }
    window_.reset();
}

ScratchpadWindow& ScratchpadService::ensureWindow() {
    if (window_ == nullptr) {
        window_ = std::make_unique<ScratchpadWindow>(environment_, copyText_);
        if (filler_) {
            window_->setTransformMenuFiller(filler_);
        }
    }
    return *window_;
}

void ScratchpadService::setTransformMenuFiller(ScratchpadWindow::TransformMenuFiller filler) {
    filler_ = std::move(filler);
    if (window_ != nullptr && filler_) {
        window_->setTransformMenuFiller(filler_);
    }
}

bool ScratchpadService::firstConnection(const void* source) {
    if (std::ranges::find(sources_, source) != sources_.end()) {
        return false;
    }
    sources_.push_back(source);
    return true;
}

void ScratchpadService::connectWindow(MainWindow& window) {
    if (!firstConnection(&window)) {
        return;
    }
    connections_.push_back(QObject::connect(&window, &MainWindow::scratchpadRequested, &window,
                                            [this] { toggleScratchpad(*this); }));
}

void ScratchpadService::connectTray(SystemTray& tray) {
    if (!firstConnection(&tray)) {
        return;
    }
    connections_.push_back(QObject::connect(&tray, &SystemTray::scratchpadRequested, &tray,
                                            [this] { toggleScratchpad(*this); }));
}

void ScratchpadService::connectHotkey(dictation::HotkeyRuntime& hotkeys, Executor& ui) {
    if (!firstConnection(&hotkeys)) {
        return;
    }
    links_.push_back(
        hotkeys.scratchpad_toggle_requested.connect(ui, [this] { toggleScratchpad(*this); }));
}

bool ScratchpadService::is_focused() const {
    return window_ != nullptr && window_->isActive();
}

bool ScratchpadService::insert(std::string text) {
    return insertIntoScratchpad(*this, fromUtf8(text));
}

void toggleScratchpad(ScratchpadService& service) {
    ScratchpadWindow& pad = service.ensureWindow();
    if (pad.isActive()) {
        pad.hide();
    } else {
        pad.present();
    }
}

void showScratchpadText(ScratchpadService& service, const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    ScratchpadWindow& pad = service.ensureWindow();
    pad.ensureLoaded();
    pad.appendBlock(text);
    pad.reveal();
}

bool insertIntoScratchpad(ScratchpadService& service, const QString& text) {
    ScratchpadWindow* pad = service.window();
    if (pad == nullptr || text.isEmpty() || !pad->isActive()) {
        return false;
    }
    pad->insertDictation(text);
    return true;
}

} // namespace fv
