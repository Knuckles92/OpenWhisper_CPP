#pragma once

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QScreen>
#include <QString>

#if __has_include(<qpa/qplatformnativeinterface.h>)
#include <qpa/qplatformnativeinterface.h>
#define FV_TEST_OFFSCREEN_SCREENS 1
#endif

// More than one offscreen screen for the multi-screen Snap tests. The offscreen platform takes
// its screens from a JSON configuration (the configfile= platform argument, which the widget
// test main does not pass); its native interface also takes one at run time through the
// "setConfiguration" resource, which is how Qt's own high-DPI tests add screens. CTest runs each
// test case in its own process, so a configuration never leaks into another test.
namespace fvtest {

class ScopedScreens {
public:
    // One screen per geometry, in global logical pixels at a device-pixel ratio of 1; the first
    // is primary. available() is false when the platform has no such resource (not offscreen,
    // or the QPA header is missing from the kit), and nothing changes then.
    explicit ScopedScreens(const QList<QRect>& geometries) {
#ifdef FV_TEST_OFFSCREEN_SCREENS
        if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
            return;
        }
        native_ = QGuiApplication::platformNativeInterface();
        if (native_ == nullptr) {
            return;
        }
        auto* get = reinterpret_cast<QJsonObject (*)(QPlatformNativeInterface*)>(
            native_->nativeResourceForIntegration("configuration"));
        set_ = reinterpret_cast<void (*)(const QJsonObject&, QPlatformNativeInterface*)>(
            native_->nativeResourceForIntegration("setConfiguration"));
        if (get == nullptr || set_ == nullptr) {
            return;
        }
        previous_ = get(native_);
        QJsonObject configuration = previous_;
        QJsonArray screens;
        int index = 0;
        for (const QRect& geometry : geometries) {
            screens.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("OffscreenTest%1").arg(++index)},
                {QStringLiteral("x"), geometry.x()},
                {QStringLiteral("y"), geometry.y()},
                {QStringLiteral("width"), geometry.width()},
                {QStringLiteral("height"), geometry.height()},
                {QStringLiteral("logicalDpi"), 96},
                {QStringLiteral("logicalBaseDpi"), 96},
                {QStringLiteral("dpr"), 1},
            });
        }
        configuration.insert(QStringLiteral("screens"), screens);
        set_(configuration, native_);
        available_ = QGuiApplication::screens().size() == geometries.size();
#else
        (void)geometries;
#endif
    }

    ~ScopedScreens() {
#ifdef FV_TEST_OFFSCREEN_SCREENS
        if (set_ != nullptr && !previous_.isEmpty()) {
            set_(previous_, native_);
        }
#endif
    }

    ScopedScreens(const ScopedScreens&) = delete;
    ScopedScreens& operator=(const ScopedScreens&) = delete;

    bool available() const { return available_; }

private:
    bool available_ = false;
#ifdef FV_TEST_OFFSCREEN_SCREENS
    QPlatformNativeInterface* native_ = nullptr;
    void (*set_)(const QJsonObject&, QPlatformNativeInterface*) = nullptr;
    QJsonObject previous_;
#endif
};

} // namespace fvtest
