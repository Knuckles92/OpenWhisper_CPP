#pragma once

#include "base/json.h"

#include <QImage>
#include <QPalette>
#include <QString>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

// fv_screenshot: compares a C++ widget with a reference rendered from OpenWhisper by
// tools/parity/render_reference.py (tests/reference/<widget>/<variant>.{png,json}). The tree is
// compared first, for a readable failure (class, objectName, properties, geometry, text,
// visibility, focus, font), then the pixels, after converting both images to ARGB32. Variant
// names are <theme>-<style>-dpr<n>-s<scale>, for example dark-windows11-dpr1-s100.
namespace fv::screenshot {

struct Options {
    // Given focus after show(), as the reference's builder pinned it.
    QWidget* focus = nullptr;
    // Tree keys not compared (for example "class" for a tree rebuilt from plain Qt widgets).
    std::vector<std::string> ignore_keys;
    // Fail when the installed application stylesheet's SHA-1 is not the reference's
    // stylesheet_sha1 (the resolved theme.qss for the variant's theme and font scale).
    bool check_app_stylesheet = true;
    // Keep the widget's QTimer children from firing between show() and the grab. The reference
    // is grabbed before any of its timers has fired; without this a tick that lands in between
    // on a busy machine moves a painted animation and the comparison fails now and then. Turn
    // off only for a widget whose reference depends on a zero-interval timer having fired.
    bool hold_timers = true;
};

// Sizes the widget to the reference's logical size, shows it, pins focus, processes events
// once, then checks the tree, the application stylesheet and the pixels. Pixels are skipped with
// a message when the build has OPENWHISPER_CPP_SCREENSHOT_TESTS off or the kit is not one that
// renders like the reference (qt_kit_matches_reference); the tree is compared either way. The skip
// is Catch2's SKIP, which ends the running test case or section, so give each variant its own
// TEST_CASE or SECTION: a second call after a skip would not run. On a pixel difference the
// widget's own grab and a magenta diff image are written to
// <build>/test-artifacts/screenshots/<widget>/.
void expect_matches(QWidget& widget, std::string_view name, std::string_view variant);
void expect_matches(QWidget& widget, std::string_view name, std::string_view variant,
                    const Options& options);

// The widget tree in the schema render_reference.py dumps: class (without the fv:: namespace),
// qtClass, objectName, geometry, visible, hasFocus, properties, and where the widget has them
// text, isChecked, isDefault, alignment, wordWrap, styleSheet; then font and children.
Json dump_tree(const QWidget& widget);

// Differences between two dumped trees, one line each, paths as objectName or class[index].
std::vector<std::string> compare_trees(const Json& expected, const Json& actual,
                                       const std::vector<std::string>& ignore_keys = {});

// The reference sidecar (pins and tree) of tests/reference/<name>/<variant>.json.
Json load_reference(std::string_view name, std::string_view variant);

struct Tolerance {
    long long max_differing_pixels = 0;
    int max_channel_delta = 255;
    std::string reason;
};
// A <variant>.tolerance.json: {"max_differing_pixels": n, "max_channel_delta": d (optional),
// "reason": "..."}. The reason is required: the lead approves every exception
// (architecture amendment 5). Throws std::runtime_error on a malformed file.
Tolerance parse_tolerance(std::string_view json_text);
// The widget's tolerance file, or the default of 0 differing pixels.
Tolerance load_tolerance(std::string_view name, std::string_view variant);

struct PixelDiff {
    bool same_size = false;
    long long differing = 0;
    int max_channel_delta = 0;
    QImage diff; // differing pixels magenta over a dimmed copy of the reference
};
PixelDiff compare_images(const QImage& reference, const QImage& actual);

// "6.10.3" -> "6.10". The kit renders like the reference when it runs on Windows (the references'
// fonts are Windows') and its family is listed in reference.json's qt_compatible; why_not says
// otherwise.
std::string qt_family(std::string_view version);
bool qt_kit_matches_reference(std::string* why_not = nullptr);
bool pixel_comparison_enabled();

// Rebuilds a dumped tree with plain Qt widgets at the dumped geometry, names, properties, texts
// and fonts (the a33 trial's render_tree.cpp): checks rendering alone, with layout taken out.
struct RebuiltTree {
    std::unique_ptr<QWidget> root;
    QWidget* focus = nullptr;
};
RebuiltTree rebuild_tree(const Json& tree);

// Installs the dumped application stylesheet, palette (every colour group), colour scheme and
// font a reference was rendered with (tests/reference/app_stylesheet/<theme>-s<scale>.qss and
// palette-<theme>.json, the pins' color_scheme and app_font), and restores the previous ones on
// destruction. Until fv_ui_theme exists this is how a test reproduces the application's look.
class ScopedReferenceTheme {
public:
    explicit ScopedReferenceTheme(const Json& pins);
    ~ScopedReferenceTheme();
    ScopedReferenceTheme(const ScopedReferenceTheme&) = delete;
    ScopedReferenceTheme& operator=(const ScopedReferenceTheme&) = delete;

private:
    QString previous_sheet_;
    QPalette previous_palette_;
    QString previous_font_;
    bool pinned_scheme_ = false;
};

} // namespace fv::screenshot
