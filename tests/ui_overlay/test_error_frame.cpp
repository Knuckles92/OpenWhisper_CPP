// The overlay's fallback frame (map row A07-43: waveform_overlay.py paintEvent's except branch,
// lines 280-292): a frame that cannot be drawn is logged once, and the pill reads "Error".
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPainter>

#include "base/log.h"
#include "overlay_test_support.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// A look whose drawing fails, as a broken painter call does in Python.
class BrokenLook final : public fv::Visual {
public:
    using fv::Visual::Visual;
    bool particles() const override { return false; }
    void draw(QPainter& /*painter*/, double /*grow*/, double /*width*/) override {
        throw std::runtime_error("the look cannot draw");
    }
    void drawWork(QPainter& /*painter*/, double /*morph*/, double /*clock*/,
                  double /*width*/) override {}
};

// The overlay's error records written while the capture lived.
class ErrorLog {
public:
    ErrorLog() : sink_(std::make_shared<Sink>()) { fv::log::add_sink(sink_); }
    ~ErrorLog() { fv::log::remove_sink(sink_); }
    ErrorLog(const ErrorLog&) = delete;
    ErrorLog& operator=(const ErrorLog&) = delete;

    std::vector<std::string> errors() const {
        const std::lock_guard lock(sink_->mutex);
        return sink_->errors;
    }

private:
    struct Sink final : fv::log::Sink {
        void write(const fv::log::Record& record) override {
            if (record.logger == "ui_qt.overlays.waveform_overlay" &&
                record.level == fv::log::Level::Error) {
                const std::lock_guard lock(mutex);
                errors.emplace_back(record.message);
            }
        }
        mutable std::mutex mutex;
        std::vector<std::string> errors;
    };
    std::shared_ptr<Sink> sink_;
};

bool hasInk(const QImage& frame, const QColor& ground) {
    for (int y = frame.height() / 4; y < frame.height() * 3 / 4; ++y) {
        for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
            if (frame.pixelColor(x, y) != ground) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("a frame that cannot be drawn is logged once and the pill reads Error", "[widget]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->timer().stop();
    const QImage whole = overlay->grab().toImage();
    // The pill's rounded corner leaves the window's corner clear.
    REQUIRE(whole.pixelColor(0, 0).alpha() == 0);
    REQUIRE_FALSE(overlay.access.paintErrorLogged());

    const fv::LookContext context{&overlay.access.ribbon(), &overlay.access.style(),
                                  overlay->width(), overlay.access.baseHeight()};
    overlay.access.visualSlot() = std::make_unique<BrokenLook>(context);
    const ErrorLog log;

    const QImage first = overlay->grab().toImage();
    const QImage second = overlay->grab().toImage();

    CHECK(overlay.access.paintErrorLogged());
    // paintEvent runs every frame: only the first failure is logged.
    REQUIRE(log.errors().size() == 1);
    CHECK(log.errors()[0].starts_with("Error drawing waveform frame"));
    // The whole rectangle is filled with the overlay's background, with "Error" on it.
    const QColor ground = first.pixelColor(0, 0);
    CHECK(ground.alpha() > 200);
    CHECK(first.pixelColor(first.width() - 1, first.height() - 1) == ground);
    CHECK(hasInk(first, ground));
    CHECK(second == first);
}
