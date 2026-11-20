// Qt's own messages in the app log (ui_qt/bootstrap.py _qt_context_suffix and
// _install_qt_message_handler, map row A01-26).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QMessageLogger>
#include <QtLogging>

#include "base/log.h"
#include "ui_shell/qt_log.h"

namespace {

struct Line {
    fv::log::Level level;
    std::string logger;
    std::string message;
};

class Captured final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        const std::lock_guard lock(mutex);
        lines.push_back({record.level, std::string(record.logger), std::string(record.message)});
    }
    std::mutex mutex;
    std::vector<Line> lines;
};

} // namespace

TEST_CASE("a Qt message's location is ' (file:line function)' only when Qt gave one", "[unit]") {
    CHECK(fv::qtContextSuffix(nullptr, 0, nullptr).empty());
    CHECK(fv::qtContextSuffix("", 0, "").empty());
    CHECK(fv::qtContextSuffix("widget.cpp", 12, "paint") == " (widget.cpp:12 paint)");
    // Python formats a missing part as None.
    CHECK(fv::qtContextSuffix(nullptr, 7, nullptr) == " (None:7 None)");
    CHECK(fv::qtContextSuffix("a.cpp", 0, nullptr) == " (a.cpp:0 None)");
}

TEST_CASE("Qt's messages go to the qt logger at Python's levels, installed once", "[unit]") {
    auto captured = std::make_shared<Captured>();
    fv::log::add_sink(captured);

    fv::installQtMessageHandler();
    fv::installQtMessageHandler();
    QMessageLogger("style.cpp", 41, "polish").warning("Could not parse stylesheet");
    QMessageLogger("net.cpp", 9, "connect").critical("socket failed");
    QMessageLogger(nullptr, 0, nullptr).info("plain note");
    qInstallMessageHandler(nullptr);
    fv::log::remove_sink(captured);

    const std::lock_guard lock(captured->mutex);
    std::vector<Line> qt;
    int installs = 0;
    for (const Line& line : captured->lines) {
        if (line.logger == "qt") {
            qt.push_back(line);
        } else if (line.logger == "root" && line.message == "Qt message handler installed") {
            ++installs;
        }
    }
    CHECK(installs == 1);
    REQUIRE(qt.size() == 3);
    CHECK(qt[0].level == fv::log::Level::Warning);
    CHECK(qt[0].message == "Could not parse stylesheet (style.cpp:41 polish)");
    CHECK(qt[1].level == fv::log::Level::Error);
    CHECK(qt[1].message == "socket failed (net.cpp:9 connect)");
    CHECK(qt[2].level == fv::log::Level::Info);
    CHECK(qt[2].message == "plain note");
}
