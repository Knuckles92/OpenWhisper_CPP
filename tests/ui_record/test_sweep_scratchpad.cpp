// Sweep H2: the Scratchpad reads its notes as Python's open(encoding="utf-8", errors="replace")
// does (open item "ui_record/scratchpad.cpp, scratchpad_saver.cpp: QString::fromUtf8 drops a
// leading BOM"): a leading BOM is a character of the notes and is saved back, and each maximal
// invalid subpart of a sequence becomes one U+FFFD.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <QString>

#include "base/log.h"
#include "base/testing/manual_executor.h"
#include "scratchpad_support.h"
#include "ui_record/scratchpad_saver.h"

namespace {

using fv::ScratchpadWindow;
using fvtest::PadRig;

// The warnings the log facade gets while the test lives.
class Warnings final : public fv::log::Sink {
public:
    Warnings() : self_(std::shared_ptr<Sink>(std::shared_ptr<Sink>(), this)) {
        fv::log::add_sink(self_);
    }
    ~Warnings() override { fv::log::remove_sink(self_); }
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        if (record.level == fv::log::Level::Warning) {
            text_ += std::string(record.message) + "\n";
        }
    }
    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
    std::shared_ptr<Sink> self_;
};

QString loaded(const std::string& bytes) {
    PadRig rig;
    rig.write(bytes);
    return rig.open().text();
}

} // namespace

TEST_CASE("a leading BOM in the notes is kept and saved back", "[sweep_scratchpad][widget]") {
    PadRig rig;
    rig.write("\xEF\xBB\xBFNotes\r\nmore");
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.text() == QStringLiteral(u"﻿Notes\nmore"));

    pad.editor()->moveCursor(QTextCursor::End);
    pad.editor()->insertPlainText(QStringLiteral("!"));
    fvtest::fire(pad.autosaveTimer());
    rig.settle();
    CHECK(rig.saved() == "\xEF\xBB\xBFNotes\nmore!");
}

TEST_CASE("invalid UTF-8 in the notes is replaced as Python's errors='replace'",
          "[sweep_scratchpad][widget]") {
    const QChar r(0xFFFD);
    // A truncated sequence at the end is one maximal subpart.
    CHECK(loaded("ab\xE2\x82") == QStringLiteral("ab") + r);
    CHECK(loaded("ab\xF0\x9F\x98") == QStringLiteral("ab") + r);
    // A truncated sequence before ASCII, and before another lead byte.
    CHECK(loaded("\xE2\x82x") == r + QStringLiteral("x"));
    CHECK(loaded("\xE2\x82\xC3\xA9") == r + QStringLiteral(u"é"));
    // Bytes that can never start or continue a sequence are one each.
    CHECK(loaded("\xFF\xFE") == QString(2, r));
    CHECK(loaded("\xC0\xAF") == QString(2, r));
    // An encoded surrogate: ED may only be followed by 80-9F, so every byte is its own subpart.
    CHECK(loaded("\xED\xA0\x80") == QString(3, r));
    // An overlong three-byte form and a code point past U+10FFFF.
    CHECK(loaded("\xE0\x80\xAF") == QString(3, r));
    CHECK(loaded("\xF4\x90\x80\x80") == QString(4, r));
    // A stray continuation byte inside text.
    CHECK(loaded("a\x80z") == QStringLiteral("a") + r + QStringLiteral("z"));
}

// The failure logs name the OSError subclass Python's type(exc).__name__ gives, never a C++ type.
TEST_CASE("a notes file that cannot be read is logged by Python's exception name",
          "[sweep_scratchpad][widget]") {
    Warnings warnings;
    PadRig rig;
    std::filesystem::create_directory(rig.file());
    rig.open();
#ifdef _WIN32
    CHECK(warnings.text() == "Couldn't read the Scratchpad (PermissionError)\n");
#else
    CHECK(warnings.text() == "Couldn't read the Scratchpad (IsADirectoryError)\n");
#endif
}

TEST_CASE("a save that fails is logged by Python's exception name", "[sweep_scratchpad]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path target = root.path() / "scratchpad.txt";
    // A folder where the notes go: replacing it with the written file fails.
    std::filesystem::create_directories(target / "inside");
    fv::ManualExecutor strand;
    Warnings warnings;
    {
        fv::ScratchpadSaver saver(strand, [target] { return target; });
        saver.writeNow("notes");
    }
#ifdef _WIN32
    CHECK(warnings.text() == "Couldn't save the Scratchpad (PermissionError)\n");
#else
    CHECK(warnings.text() == "Couldn't save the Scratchpad (IsADirectoryError)\n");
#endif
}
