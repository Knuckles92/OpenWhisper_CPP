#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/utf8.h"
#include "walkthrough/renderer.h"
#include "walkthrough/storage.h"

namespace fv::walkthrough {

// A DocumentRenderer that writes a small marker file per call ("fake <kind>\n" and the title)
// and records every call. Told to, it throws DocumentError for one kind of call, or for all.
// Affinity: thread-safe.
class FakeRenderer final : public DocumentRenderer {
public:
    enum class Kind { Pdf, Docx, MarkdownPdf, MarkdownDocx };

    struct Call {
        Kind kind;
        std::filesystem::path target; // the file written (or that would have been)
        std::filesystem::path folder;
        std::string title;    // the document's title (format.title for a walkthrough)
        std::string markdown; // the Markdown given, for the Markdown kinds
        DocumentFormat format;
        Look look;
        PaperSize paper;
        int sections = 0; // a walkthrough's section count
    };

    // The next calls of `kind` (every kind when nullopt) throw DocumentError(message).
    void fail(std::optional<Kind> kind, std::string message) {
        std::lock_guard lock(mutex_);
        failing_ = true;
        failing_kind_ = kind;
        message_ = std::move(message);
    }
    void stop_failing() {
        std::lock_guard lock(mutex_);
        failing_ = false;
    }

    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    std::filesystem::path write_pdf(const Walkthrough& walkthrough,
                                    const std::filesystem::path& folder,
                                    const DocumentFormat& format, PaperSize paper) override {
        const auto target = folder / path_from_utf8(PDF_NAME);
        record({Kind::Pdf,
                target,
                folder,
                format.title,
                "",
                format,
                {},
                paper,
                static_cast<int>(walkthrough.sections.size())});
        return target;
    }

    std::filesystem::path write_docx(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder,
                                     const DocumentFormat& format, PaperSize paper) override {
        const auto target = folder / path_from_utf8(DOCX_NAME);
        record({Kind::Docx,
                target,
                folder,
                format.title,
                "",
                format,
                {},
                paper,
                static_cast<int>(walkthrough.sections.size())});
        return target;
    }

    void write_markdown_pdf(std::string_view text, const std::filesystem::path& target,
                            const std::filesystem::path& folder, std::string_view title,
                            const Look& look, PaperSize paper) override {
        record({Kind::MarkdownPdf,
                target,
                folder,
                std::string(title),
                std::string(text),
                {},
                look,
                paper,
                0});
    }

    void write_markdown_docx(std::string_view text, const std::filesystem::path& target,
                             const std::filesystem::path& folder, std::string_view title,
                             const Look& look, PaperSize paper) override {
        record({Kind::MarkdownDocx,
                target,
                folder,
                std::string(title),
                std::string(text),
                {},
                look,
                paper,
                0});
    }

    static std::string_view name(Kind kind) {
        switch (kind) {
        case Kind::Pdf:
            return "pdf";
        case Kind::Docx:
            return "docx";
        case Kind::MarkdownPdf:
            return "markdown pdf";
        case Kind::MarkdownDocx:
            return "markdown docx";
        }
        return "";
    }

private:
    void record(Call call) {
        std::optional<std::string> message;
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(call);
            if (failing_ && (!failing_kind_ || *failing_kind_ == call.kind)) {
                message = message_;
            }
        }
        if (message) {
            throw DocumentError(*message);
        }
        std::ofstream out(call.target, std::ios::binary | std::ios::trunc);
        out << "fake " << name(call.kind) << "\n" << call.title;
        if (!out) {
            throw DocumentError("Could not write " + path_to_utf8(call.target));
        }
    }

    mutable std::mutex mutex_;
    std::vector<Call> calls_;
    bool failing_ = false;
    std::optional<Kind> failing_kind_;
    std::string message_;
};

} // namespace fv::walkthrough
