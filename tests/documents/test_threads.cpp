// from tests/test_walkthrough_document_threads.py: walkthrough documents built on a worker while
// the main thread handles events, each write-up as a PDF and a Word document.
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "documents/docx.h"
#include "documents/pdf.h"
#include "documents_support.h"

namespace fv::documents::test {
namespace {

constexpr std::string_view kWriteUp =
    "# Steps\n\n1. Sign in.\n2. Invoices hang.\n\n- one\n- two\n\n"
    "| Step | Result |\n|---|---|\n| Sign in | Stalls |\n\n3. Then it recovers.\n";

// Writes `count` write-ups into `folder`; the first error's message, or "" when all were written.
std::string writeMany(int count, const std::filesystem::path& folder) {
    for (int n = 0; n < count; ++n) {
        try {
            const WrittenPdf pdf = writeMarkdownPdf(kWriteUp, folder / "w.pdf", folder, "Steps");
            if (pdf.pages.empty()) {
                return "a write-up came out without pages";
            }
            writeMarkdownDocx(kWriteUp, folder / "w.docx", folder, "Steps");
            if (std::filesystem::file_size(folder / "w.docx") == 0) {
                return "a write-up's Word document came out empty";
            }
        } catch (const std::exception& error) {
            return error.what();
        }
    }
    return {};
}

TEST_CASE("write ups written back to back on a worker stay usable",
          "[test_walkthrough_document_threads]") {
    TempFolder tmp;
    std::future<std::string> done =
        std::async(std::launch::async, [&] { return writeMany(80, tmp.path()); });
    while (done.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        QCoreApplication::processEvents();
    }
    CHECK(done.get().empty());
}

TEST_CASE("a worker never waits for a blocked main thread", "[test_walkthrough_document_threads]") {
    TempFolder tmp;
    std::future<std::string> done =
        std::async(std::launch::async, [&] { return writeMany(3, tmp.path()); });
    // The main thread handles no events while it waits.
    const bool finished = done.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    CHECK(finished);
    CHECK(done.get().empty());
    QCoreApplication::processEvents();
}

TEST_CASE("write ups can be written on the main thread", "[test_walkthrough_document_threads]") {
    TempFolder tmp;
    CHECK(writeMany(3, tmp.path()).empty());
}

} // namespace
} // namespace fv::documents::test
