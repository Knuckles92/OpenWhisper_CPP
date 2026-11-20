#include "screenshot/screenshot.h"

#include "screenshot/dumped_widgets.h"
#include "testkit/testkit.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStyleHints>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fv::screenshot {
namespace {

// Swallows the timer events of every QTimer below a widget for as long as it lives, so none of
// them emits timeout().
class HeldTimers : public QObject {
public:
    explicit HeldTimers(QWidget& root) {
        for (QTimer* timer : root.findChildren<QTimer*>()) {
            timer->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override { return event->type() == QEvent::Timer; }
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("screenshot: cannot read " + path.string());
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

QString to_qstring(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
}

std::string utf8(const QString& text) {
    return text.toUtf8().toStdString();
}

std::filesystem::path reference_file(std::string_view name, std::string_view variant,
                                     std::string_view suffix) {
    return testkit::reference_path() / std::string(name) /
           (std::string(variant) + std::string(suffix));
}

// Python's repr of a float: the shortest text that reads back, with ".0" on integral values.
std::string python_float(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".en") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// str() of the Python value PyQt6 makes of a property's QVariant.
std::string python_str(const QVariant& value) {
    if (!value.isValid()) {
        return "None";
    }
    switch (value.metaType().id()) {
    case QMetaType::Bool:
        return value.toBool() ? "True" : "False";
    case QMetaType::Double:
    case QMetaType::Float:
        return python_float(value.toDouble());
    default:
        return utf8(value.toString());
    }
}

const char* qt_base_class(const QWidget& widget) {
    const QMetaObject* meta = widget.metaObject();
    while (meta != nullptr && meta->className()[0] != 'Q') {
        meta = meta->superClass();
    }
    return meta != nullptr ? meta->className() : "QWidget";
}

std::string without_namespace(std::string_view name) {
    const auto colon = name.rfind("::");
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 2));
}

std::optional<QVariant> read_property(const QWidget& widget, const char* name) {
    const int index = widget.metaObject()->indexOfProperty(name);
    if (index < 0) {
        return std::nullopt;
    }
    return widget.metaObject()->property(index).read(&widget);
}

// The alignment() methods PyQt6 exposes on widgets, as the int of the flag value.
std::optional<int> alignment_of(const QWidget& widget) {
    if (const auto* w = qobject_cast<const QLabel*>(&widget)) {
        return w->alignment().toInt();
    }
    if (const auto* w = qobject_cast<const QLineEdit*>(&widget)) {
        return w->alignment().toInt();
    }
    if (const auto* w = qobject_cast<const QAbstractSpinBox*>(&widget)) {
        return w->alignment().toInt();
    }
    if (const auto* w = qobject_cast<const QProgressBar*>(&widget)) {
        return w->alignment().toInt();
    }
    if (const auto* w = qobject_cast<const QTextEdit*>(&widget)) {
        return w->alignment().toInt();
    }
    if (const auto* w = qobject_cast<const QGroupBox*>(&widget)) {
        return w->alignment();
    }
    if (const auto* w = qobject_cast<const QScrollArea*>(&widget)) {
        return w->alignment().toInt();
    }
    return std::nullopt;
}

std::vector<std::string_view> split_fields(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (;;) {
        const auto comma = text.find(',', start);
        fields.push_back(text.substr(start, comma - start));
        if (comma == std::string_view::npos) {
            return fields;
        }
        start = comma + 1;
    }
}

// QFont::toString grows fields between Qt minors: 6.11 appends the style name and the feature
// and variable-axis counts ("...,1,,0,0") where 6.10 stops at "...,1", and 6.10's fromString
// rejects the longer form. Two descriptions are the same font when their common fields agree and
// every extra field holds its default (empty or 0).
bool same_font(std::string_view expected, std::string_view actual) {
    const auto a = split_fields(expected);
    const auto b = split_fields(actual);
    const auto common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    const auto& longer = a.size() > b.size() ? a : b;
    for (std::size_t i = common; i < longer.size(); ++i) {
        if (!longer[i].empty() && longer[i] != "0") {
            return false;
        }
    }
    return true;
}

// A QFont from a description another Qt minor wrote: trailing default fields this kit does not
// know are dropped until fromString accepts the rest.
QFont font_from(std::string_view description) {
    auto fields = split_fields(description);
    QFont font;
    for (;;) {
        std::string joined;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            joined += (i == 0 ? "" : ",") + std::string(fields[i]);
        }
        if (font.fromString(QString::fromStdString(joined)) || fields.size() <= 1 ||
            (!fields.back().empty() && fields.back() != "0")) {
            return font;
        }
        fields.pop_back();
    }
}

std::string node_path(const Json& node, const std::string& parent, std::size_t index) {
    const auto name = node.value("objectName", std::string());
    const auto label =
        name.empty() ? node.value("qtClass", std::string("?")) + "[" + std::to_string(index) + "]"
                     : name;
    return parent.empty() ? label : parent + "/" + label;
}

void compare_node(const Json& expected, const Json& actual, const std::string& path,
                  const std::vector<std::string>& ignore, std::vector<std::string>& out) {
    const auto ignored = [&](const std::string& key) {
        return std::find(ignore.begin(), ignore.end(), key) != ignore.end();
    };
    for (const auto& [key, value] : expected.items()) {
        if (key == "children" || ignored(key)) {
            continue;
        }
        const auto it = actual.find(key);
        if (it == actual.end()) {
            out.push_back(path + ": no " + key + " (expected " + value.dump() + ")");
            continue;
        }
        Json mine = *it;
        Json want = value;
        if (key == "class" && mine.is_string()) {
            mine = without_namespace(mine.get<std::string>());
        } else if (key == "font" && mine.is_string() && want.is_string() &&
                   same_font(want.get<std::string>(), mine.get<std::string>())) {
            mine = want;
        }
        if (mine != want) {
            out.push_back(path + ": " + key + " expected " + want.dump() + ", got " + mine.dump());
        }
    }
    for (const auto& [key, value] : actual.items()) {
        if (key != "children" && !ignored(key) && !expected.contains(key)) {
            out.push_back(path + ": unexpected " + key + " " + value.dump());
        }
    }
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    if (want.size() != have.size()) {
        out.push_back(path + ": " + std::to_string(want.size()) + " children expected, got " +
                      std::to_string(have.size()));
    }
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        compare_node(want[i], have[i], node_path(want[i], path, i), ignore, out);
    }
}

void write_png(const QImage& image, const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    image.save(to_qstring(path), "PNG");
}

QWidget* build_node(const Json& node, QWidget* parent, QWidget*& focus) {
    const auto cls = node.value("qtClass", std::string("QWidget"));
    const auto text = QString::fromStdString(node.value("text", std::string()));
    const bool has_text = node.contains("text");
    QWidget* widget = nullptr;
    if (cls == "QLabel") {
        auto* label = new QLabel(text, parent);
        if (node.contains("alignment")) {
            label->setAlignment(Qt::Alignment::fromInt(node["alignment"].get<int>()));
        }
        label->setWordWrap(node.value("wordWrap", false));
        widget = label;
    } else if (cls == "QPushButton") {
        auto* button = new QPushButton(text, parent);
        button->setAutoDefault(false);
        button->setDefault(node.value("isDefault", false));
        button->setChecked(node.value("isChecked", false));
        widget = button;
    } else if (cls == "QCheckBox") {
        auto* box = new QCheckBox(text, parent);
        box->setChecked(node.value("isChecked", false));
        widget = box;
    } else if (cls == "QRadioButton") {
        auto* radio = new QRadioButton(text, parent);
        radio->setChecked(node.value("isChecked", false));
        widget = radio;
    } else if (cls == "QToolButton") {
        auto* tool = new QToolButton(parent);
        tool->setText(text);
        tool->setChecked(node.value("isChecked", false));
        widget = tool;
    } else if (cls == "QLineEdit") {
        widget = new QLineEdit(text, parent);
    } else if (cls == "QDialog") {
        widget = has_text ? new detail::DumpedDialog(text, parent) : new QDialog(parent);
    } else if (cls == "QFrame") {
        widget = has_text ? new detail::DumpedFrame(text, parent) : new QFrame(parent);
    } else {
        widget = has_text ? new detail::DumpedWidget(text, parent) : new QWidget(parent);
    }
    widget->setObjectName(QString::fromStdString(node.value("objectName", std::string())));
    if (const auto it = node.find("properties"); it != node.end()) {
        for (const auto& [key, value] : it->items()) {
            const auto string = value.get<std::string>();
            // The dump holds Python's str() of each value; a bool must be a bool again for
            // [prop="true"] selectors to match.
            const QVariant variant = string == "True"    ? QVariant(true)
                                     : string == "False" ? QVariant(false)
                                                         : QVariant(QString::fromStdString(string));
            widget->setProperty(key.c_str(), variant);
        }
    }
    if (node.contains("styleSheet")) {
        widget->setStyleSheet(QString::fromStdString(node["styleSheet"].get<std::string>()));
    }
    widget->setFont(font_from(node.value("font", std::string())));
    const auto& g = node.at("geometry");
    if (parent != nullptr) {
        widget->setGeometry(g[0].get<int>(), g[1].get<int>(), g[2].get<int>(), g[3].get<int>());
    } else {
        widget->setFixedSize(g[2].get<int>(), g[3].get<int>());
    }
    if (const auto it = node.find("children"); it != node.end()) {
        for (const auto& child : *it) {
            build_node(child, widget, focus);
        }
    }
    if (parent != nullptr) {
        widget->setVisible(node.value("visible", true));
    }
    // Focus draws a ring and :focus rules; the first focusable widget would take it otherwise.
    if (node.value("hasFocus", false)) {
        focus = widget;
    }
    return widget;
}

} // namespace

std::string qt_family(std::string_view version) {
    const auto first = version.find('.');
    if (first == std::string_view::npos) {
        return std::string(version);
    }
    const auto second = version.find('.', first + 1);
    return std::string(version.substr(0, second));
}

bool qt_kit_matches_reference(std::string* why_not) {
    const Json reference = Json::parse(read_text(testkit::parity_reference_file()));
    const std::string family = qt_family(qVersion());
    bool listed_family = false;
    for (const auto& listed : reference.value("qt_compatible", Json::array())) {
        listed_family =
            listed_family || (listed.is_string() && listed.get<std::string>() == family);
    }
    if (!listed_family) {
        if (why_not != nullptr) {
            *why_not = "Qt " + std::string(qVersion()) +
                       " is not a kit tools/parity/reference.json lists as rendering like the "
                       "references (PyQt6 " +
                       reference.value("pyqt", std::string("?")) +
                       "); pixels are not compared, the tree was";
        }
        return false;
    }
#ifdef Q_OS_WIN
    return true;
#else
    // The references are drawn with Windows' own fonts (Segoe UI), which exist only there (a33
    // map, section 5).
    if (why_not != nullptr) {
        *why_not = "the references are rendered on Windows with its fonts; pixels are compared "
                   "only there, the tree was";
    }
    return false;
#endif
}

bool pixel_comparison_enabled() {
#ifdef FV_SCREENSHOT_PIXELS
    return true;
#else
    return false;
#endif
}

Json dump_tree(const QWidget& widget) {
    Json node = Json::object();
    node["class"] = without_namespace(widget.metaObject()->className());
    node["qtClass"] = qt_base_class(widget);
    node["objectName"] = utf8(widget.objectName());
    node["geometry"] = Json::array({widget.x(), widget.y(), widget.width(), widget.height()});
    node["visible"] = widget.isVisible();
    node["hasFocus"] = widget.hasFocus();
    Json properties = Json::object();
    for (const QByteArray& name : widget.dynamicPropertyNames()) {
        if (!name.startsWith("_q_")) {
            properties[name.toStdString()] = python_str(widget.property(name.constData()));
        }
    }
    node["properties"] = std::move(properties);
    if (const auto text = read_property(widget, "text")) {
        node["text"] = utf8(text->toString());
    }
    if (qobject_cast<const QAbstractButton*>(&widget) != nullptr ||
        qobject_cast<const QGroupBox*>(&widget) != nullptr) {
        node["isChecked"] = read_property(widget, "checked").value_or(false).toBool();
    }
    if (const auto* button = qobject_cast<const QPushButton*>(&widget)) {
        node["isDefault"] = button->isDefault();
    }
    if (const auto alignment = alignment_of(widget)) {
        node["alignment"] = *alignment;
    }
    if (const auto wrap = read_property(widget, "wordWrap")) {
        node["wordWrap"] = wrap->toBool();
    }
    if (!widget.styleSheet().isEmpty()) {
        node["styleSheet"] = utf8(widget.styleSheet());
    }
    node["font"] = utf8(widget.font().toString());
    Json children = Json::array();
    for (const QObject* child : widget.children()) {
        const auto* child_widget = qobject_cast<const QWidget*>(child);
        if (child_widget != nullptr && !child_widget->isWindow()) {
            children.push_back(dump_tree(*child_widget));
        }
    }
    node["children"] = std::move(children);
    return node;
}

std::vector<std::string> compare_trees(const Json& expected, const Json& actual,
                                       const std::vector<std::string>& ignore_keys) {
    std::vector<std::string> out;
    compare_node(expected, actual, node_path(expected, "", 0), ignore_keys, out);
    return out;
}

Json load_reference(std::string_view name, std::string_view variant) {
    const auto path = reference_file(name, variant, ".json");
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("screenshot: no reference " + path.string() +
                                 "; render it with tools/parity/render_reference.py");
    }
    return Json::parse(read_text(path));
}

Tolerance parse_tolerance(std::string_view json_text) {
    const Json document = Json::parse(json_text);
    if (!document.is_object()) {
        throw std::runtime_error("tolerance: the file must hold an object");
    }
    Tolerance tolerance;
    const auto pixels = document.find("max_differing_pixels");
    if (pixels == document.end() || !pixels->is_number_integer() || pixels->get<long long>() < 0) {
        throw std::runtime_error("tolerance: max_differing_pixels must be a count of pixels");
    }
    tolerance.max_differing_pixels = pixels->get<long long>();
    if (const auto delta = document.find("max_channel_delta"); delta != document.end()) {
        if (!delta->is_number_integer() || delta->get<int>() < 0 || delta->get<int>() > 255) {
            throw std::runtime_error("tolerance: max_channel_delta must be 0 to 255");
        }
        tolerance.max_channel_delta = delta->get<int>();
    }
    const auto reason = document.find("reason");
    if (reason == document.end() || !reason->is_string() ||
        reason->get<std::string>().find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::runtime_error("tolerance: a tolerance needs the reason it was accepted");
    }
    tolerance.reason = reason->get<std::string>();
    return tolerance;
}

Tolerance load_tolerance(std::string_view name, std::string_view variant) {
    const auto path = reference_file(name, variant, ".tolerance.json");
    if (!std::filesystem::is_regular_file(path)) {
        return {};
    }
    return parse_tolerance(read_text(path));
}

PixelDiff compare_images(const QImage& reference, const QImage& actual) {
    PixelDiff result;
    const QImage a = reference.convertToFormat(QImage::Format_ARGB32);
    const QImage b = actual.convertToFormat(QImage::Format_ARGB32);
    if (a.size() != b.size()) {
        return result;
    }
    result.same_size = true;
    result.diff = QImage(a.size(), QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const auto* left = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* right = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(result.diff.scanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = left[x];
            const QRgb q = right[x];
            const int delta =
                std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                          std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
            result.max_channel_delta = std::max(result.max_channel_delta, delta);
            if (delta > 0) {
                ++result.differing;
                out[x] = qRgb(255, 0, 255);
            } else {
                const int dim = qGray(p) / 3;
                out[x] = qRgb(dim, dim, dim);
            }
        }
    }
    return result;
}

void expect_matches(QWidget& widget, std::string_view name, std::string_view variant) {
    expect_matches(widget, name, variant, Options{});
}

void expect_matches(QWidget& widget, std::string_view name, std::string_view variant,
                    const Options& options) {
    const Json pins = load_reference(name, variant);
    const auto& size = pins.at("logical_size");
    widget.setFixedSize(size[0].get<int>(), size[1].get<int>());
    std::optional<HeldTimers> held;
    if (options.hold_timers) {
        held.emplace(widget);
    }
    widget.show();
    if (options.focus != nullptr) {
        options.focus->setFocus();
    }
    QCoreApplication::processEvents();

    const auto differences = compare_trees(pins.at("tree"), dump_tree(widget), options.ignore_keys);
    for (const auto& difference : differences) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
    // The resolved application stylesheet is a pin of the render: with another one installed,
    // the pixels (and usually the geometry above) cannot match, and this says why.
    if (options.check_app_stylesheet) {
        const QByteArray sheet_sha1 =
            QCryptographicHash::hash(qApp->styleSheet().toUtf8(), QCryptographicHash::Sha1).toHex();
        const auto expected_sha1 = pins.value("stylesheet_sha1", std::string());
        if (sheet_sha1.toStdString() != expected_sha1) {
            FAIL_CHECK(name << "/" << variant << ": the application stylesheet's SHA-1 is "
                            << sheet_sha1.toStdString() << ", the reference was rendered with "
                            << expected_sha1 << " (tests/reference/app_stylesheet/"
                            << pins.value("theme", std::string("?")) << "-s"
                            << pins.value("font_scale_percent", 0) << ".qss)");
        }
    }

    if (!pixel_comparison_enabled()) {
        SKIP(
            name << "/" << variant
                 << ": pixels are not compared (OPENWHISPER_CPP_SCREENSHOT_TESTS is OFF); the tree "
                    "was");
    }
    std::string why_not;
    if (!qt_kit_matches_reference(&why_not)) {
        SKIP(name << "/" << variant << ": " << why_not);
    }

    const QImage actual = widget.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const QImage reference(to_qstring(reference_file(name, variant, ".png")));
    REQUIRE_FALSE(reference.isNull());
    const PixelDiff diff = compare_images(reference, actual);
    const auto artifacts = testkit::artifact_path() / "screenshots" / std::string(name);
    if (!diff.same_size || diff.differing > 0) {
        write_png(actual, artifacts / (std::string(variant) + "-actual.png"));
        if (diff.same_size) {
            write_png(diff.diff, artifacts / (std::string(variant) + "-diff.png"));
        }
    }
    INFO(name << "/" << variant << ": reference " << reference.width() << "x" << reference.height()
              << ", grab " << actual.width() << "x" << actual.height() << "; images in "
              << artifacts.string());
    REQUIRE(diff.same_size);
    const Tolerance tolerance = load_tolerance(name, variant);
    INFO(diff.differing << " of " << (static_cast<long long>(actual.width()) * actual.height())
                        << " pixels differ, max channel delta " << diff.max_channel_delta
                        << "; tolerance " << tolerance.max_differing_pixels << " pixels");
    CHECK(diff.differing <= tolerance.max_differing_pixels);
    if (diff.differing > 0) {
        CHECK(diff.max_channel_delta <= tolerance.max_channel_delta);
    }
}

RebuiltTree rebuild_tree(const Json& tree) {
    RebuiltTree rebuilt;
    rebuilt.root.reset(build_node(tree, nullptr, rebuilt.focus));
    return rebuilt;
}

ScopedReferenceTheme::ScopedReferenceTheme(const Json& pins)
    : previous_sheet_(qApp->styleSheet()), previous_palette_(QApplication::palette()),
      previous_font_(QApplication::font().toString()) {
    const auto theme = pins.at("theme").get<std::string>();
    const auto scale = pins.at("font_scale_percent").get<int>();
    const auto folder = testkit::reference_path() / "app_stylesheet";
    const Json colors = Json::parse(read_text(folder / ("palette-" + theme + ".json")));
    const auto sheet = read_text(folder / (theme + "-s" + std::to_string(scale) + ".qss"));

    // {group: {role: "#aarrggbb"}}: the Disabled group has text colours of its own.
    QPalette palette = QApplication::palette();
    const QMetaEnum roles = QMetaEnum::fromType<QPalette::ColorRole>();
    const QMetaEnum groups = QMetaEnum::fromType<QPalette::ColorGroup>();
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const char* group_key = groups.valueToKey(group);
        if (group_key == nullptr || !colors.contains(group_key)) {
            throw std::runtime_error(std::string("screenshot: palette-") + theme + ".json has no " +
                                     (group_key ? group_key : "?") + " group");
        }
        const Json& group_colors = colors[group_key];
        for (int role = 0; role < QPalette::NColorRoles; ++role) {
            const char* key = roles.valueToKey(role);
            if (key != nullptr && group_colors.contains(key)) {
                palette.setColor(
                    group, static_cast<QPalette::ColorRole>(role),
                    QColor(QString::fromStdString(group_colors[key].get<std::string>())));
            }
        }
    }
    QApplication::setPalette(palette);
    // After the palette, as ThemeManager.apply_platform_palette does: windows11 draws what the
    // stylesheet leaves to it in the scheme's colours. The pin is the scheme in effect, which the
    // offscreen platform leaves "Unknown" whatever is asked; then nothing is requested here.
    const auto scheme = pins.value("color_scheme", std::string());
    if (scheme == "Dark" || scheme == "Light") {
        QGuiApplication::styleHints()->setColorScheme(scheme == "Dark" ? Qt::ColorScheme::Dark
                                                                       : Qt::ColorScheme::Light);
        pinned_scheme_ = true;
    }
    qApp->setStyleSheet(QString::fromUtf8(sheet.data(), static_cast<qsizetype>(sheet.size())));
    QApplication::setFont(font_from(pins.at("app_font").get<std::string>()));
}

ScopedReferenceTheme::~ScopedReferenceTheme() {
    if (pinned_scheme_) {
        QGuiApplication::styleHints()->unsetColorScheme();
    }
    QApplication::setPalette(previous_palette_);
    qApp->setStyleSheet(previous_sheet_);
    QFont font;
    font.fromString(previous_font_);
    QApplication::setFont(font);
}

} // namespace fv::screenshot
