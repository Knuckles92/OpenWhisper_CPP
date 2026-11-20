// The storyboard's buttons, the app's one-operation gate, the clipboard and the accessible
// editors (ui_qt/widgets/walkthrough_review.py), from tests/test_walkthrough_library_ui.py. The
// Python tests drive the review inside the library dialog (M6.16's); here it stands alone, and
// the dialog's reject() is the review's cancel(), which is all it does while the review works.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <QAction>
#include <QLabel>
#include <QPushButton>

#include "base/utf8.h"
#include "review_support.h"
#include "ui_widgets/settings_switch.h"
#include "walkthrough/files.h"
#include "walkthrough/library.h"
#include "walkthrough/write_up_names.h"

namespace fvtest {
namespace {

bool contains(const QString& text, const char* part) {
    return text.contains(QString::fromUtf8(part));
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::vector<bool> checks(const Review& review) {
    std::vector<bool> out;
    for (auto* row : review->rows()) {
        out.push_back(row->check()->isChecked());
    }
    return out;
}

} // namespace

// from
// tests/test_walkthrough_library_ui.py::test_review_loads_section_editors_and_screenshot_choices
TEST_CASE("review loads section editors and screenshot choices",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    REQUIRE(review->rows().size() == 3);
    CHECK(review->rows()[0]->editor()->toPlainText() == QStringLiteral("first"));
    CHECK(checks(review) == std::vector<bool>{true, true, false});
    CHECK(review->rows()[0]->editor()->accessibleName() == QStringLiteral("Section 1 transcript"));
    CHECK(review->saveButton()->isEnabled());
    CHECK_FALSE(review->recoverButton()->isEnabled());
}

// from tests/test_walkthrough_library_ui.py::test_copy_buttons_send_explicit_clipboard_formats
TEST_CASE("copy buttons send explicit clipboard formats", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->markdownButton()->click();
    REQUIRE(review.host.copies.size() == 1);
    CHECK(review.host.copies.back().files.empty());
    CHECK(contains(review.host.copies.back().text, "first"));
    review->pdfButton()->click();
    REQUIRE(review.host.copies.size() == 2);
    CHECK(review.host.copies.back().files ==
          std::vector<std::filesystem::path>{capture.result.pdf});
    CHECK(review.host.copies.back().text.isEmpty());
    review->copyAgentAction()->trigger();
    REQUIRE(review.host.copies.size() == 3);
    const auto& agent = review.host.copies.back();
    CHECK(agent.files.empty());
    CHECK(contains(agent.text, "carry out the tasks and changes"));
    auto pdf = std::filesystem::weakly_canonical(capture.result.pdf);
    pdf.make_preferred();
    CHECK(contains(agent.text.toStdString(), fv::path_to_utf8(pdf)));
    CHECK(review->notice()->text() ==
          QStringLiteral("Instructions copied. Paste them, then drag the PDF into your chat."));
}

// from tests/test_walkthrough_library_ui.py::test_unsaved_edits_cannot_be_shared_or_written_up
TEST_CASE("unsaved edits cannot be shared or written up", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    review->markdownButton()->click();
    review->copyAgentAction()->trigger();
    CHECK(review.host.copies.empty());
    CHECK(contains(review->notice()->text(), "Save"));
    review->retryAction()->trigger();
    CHECK_FALSE(review->isBusy());
}

// from tests/test_walkthrough_library_ui.py::test_save_regenerates_and_releases_the_app_gate
TEST_CASE("save regenerates and releases the app gate", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    review->rows()[0]->check()->setChecked(false);
    review->saveButton()->click();
    CHECK_FALSE(review->markdownButton()->isEnabled());
    review.wait();
    CHECK(review.host.acquires == 1);
    CHECK(review.host.releases == 1);
    CHECK_FALSE(review->isDirty());
    const auto result = wt::load_result(capture.folder);
    CHECK(result.shots == 1);
    // The omitted screenshot stays switchable, so the next save can restore it.
    CHECK(checks(review) == std::vector<bool>{false, true, false});
    CHECK(review->rows()[0]->check()->isEnabled());
    review->rows()[0]->check()->setChecked(true);
    review->saveButton()->click();
    review.wait();
    CHECK(wt::load_result(capture.folder).shots == 2);
    CHECK(contains(result.clipboard_text, "corrected"));
}

// from tests/test_walkthrough_library_ui.py::test_gate_refusal_starts_no_worker
TEST_CASE("gate refusal starts no worker", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review.host.refusal = QStringLiteral("Finish recording first");
    review->saveButton()->click();
    CHECK_FALSE(review->isBusy());
    CHECK(review->notice()->text() == QStringLiteral("Finish recording first"));
    CHECK(review.host.releases == 0);
}

// from tests/test_walkthrough_library_ui.py::test_executor_submission_failure_releases_the_gate
TEST_CASE("executor submission failure releases the gate",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    auto host = review.host.callbacks();
    host.submit = [](fv::Task) -> bool {
        throw std::runtime_error("shutting down");
    };
    review->bind(host);
    review->saveButton()->click();
    CHECK_FALSE(review->isBusy());
    CHECK(review.host.releases == 1);
    CHECK(contains(review->notice()->text(), "shutting down"));

    // An executor that says no without a reason reads as Python's after its shutdown.
    host.submit = [](fv::Task) {
        return false;
    };
    review->bind(host);
    review->saveButton()->click();
    CHECK_FALSE(review->isBusy());
    CHECK(review.host.releases == 2);
    CHECK(contains(review->notice()->text(), "cannot schedule new futures after shutdown"));
}

// from tests/test_walkthrough_library_ui.py::
//     test_cancel_before_a_queued_retry_starts_preserves_the_source
TEST_CASE("cancel before a queued retry starts preserves the source",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    const std::string before = readFile(capture.folder / "walkthrough.md");
    review->retryAction()->trigger();
    CHECK(review->isBusy());
    review->cancel();
    CHECK(review->cancelRequested());
    review.wait();
    CHECK_FALSE(review->isBusy());
    CHECK(review.agent.calls.empty());
    CHECK(contains(review->notice()->text(), "canceled"));
    CHECK(readFile(capture.folder / "walkthrough.md") == before);
}

// from tests/test_walkthrough_library_ui.py::
//     test_render_failure_preserves_unsaved_editors_and_button_states
TEST_CASE("render failure preserves unsaved editors and button states",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    capture.renderer->fail(std::nullopt, "full disk");
    review->rows()[0]->editor()->setPlainText(QStringLiteral("keep this correction"));
    review->saveButton()->click();
    review.wait();
    CHECK(review->isDirty());
    CHECK(review->rows()[0]->editor()->toPlainText() == QStringLiteral("keep this correction"));
    CHECK_FALSE(review->recoverButton()->isEnabled());
    CHECK(contains(review->notice()->text(), "full disk"));
    // The buttons are as they were before the work, not left off.
    CHECK(review->saveButton()->isEnabled());
    CHECK(review->pdfButton()->isEnabled());
}

// from
// tests/test_walkthrough_library_ui.py::test_review_write_up_retry_is_off_thread_and_cancelable
TEST_CASE("review write-up retry is off thread and cancelable",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review.agent.reply = "# Redone";
    bool canceled = false;
    review->setWriteUpRunner([&](const wt::write_up::AgentTaskRequest& request) {
        // The review's own cancel reaches the agent's run.
        review->cancel();
        canceled = request.cancel.cancelled();
        return review.agent.runner()(request);
    });
    review->retryAction()->trigger();
    // Nothing runs until the executor does.
    CHECK(review.agent.calls.empty());
    review.wait();
    REQUIRE(review.agent.calls.size() == 1);
    CHECK(canceled);
    // The plain redo uses Settings' prompt (here the default words), reading the folder.
    const auto& request = review.agent.calls[0];
    CHECK(std::filesystem::equivalent(request.folder, capture.folder));
    CHECK(contains(request.user_prompt, wt::write_up::DEFAULT_INSTRUCTIONS));
}

// from tests/test_walkthrough_library_ui.py::test_recovery_requests_the_selected_draft
TEST_CASE("recovery requests the selected draft", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    wt::atomic_json(capture.folder / "draft.json", fv::Json{{"version", 1},
                                                            {"started_at", "2026-10-21T14:32:05"},
                                                            {"shots", fv::Json::array()}});
    writeFile(capture.folder / "audio.wav", "audio");
    Review review(true, true);
    review->showFolder(capture.folder);
    CHECK(review->recoverButton()->isEnabled());
    review->recoverButton()->click();
    CHECK(review.host.recovered == std::vector<QString>{QString::fromStdString(
                                       fv::path_to_utf8(preferred(capture.folder)))});
}

// from tests/test_walkthrough_library_ui.py::test_a_failed_redo_after_saving_shows_what_was_saved
TEST_CASE("a failed redo after saving shows what was saved",
          "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review.agent.error = "agent missing";
    review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    review->save(true);
    review.wait();
    // The words are on disk, so the board is reloaded and nothing is unsaved: a second save
    // would otherwise repeat the same edit and reordering.
    CHECK_FALSE(review->isDirty());
    CHECK(review->rows()[0]->editor()->toPlainText() == QStringLiteral("corrected"));
    CHECK(review->notice()->text() ==
          QStringLiteral("Saved, but the write-up wasn't redone: agent missing"));
}

// from tests/test_walkthrough_library_ui.py::test_delete_does_not_say_saved
TEST_CASE("delete does not say saved", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->deleteFolder();
    review.wait();
    CHECK(review.questions == 1);
    CHECK_FALSE(contains(review->notice()->text(), "Saved"));
    CHECK(review->notice()->text() == QStringLiteral("Deleted."));
    CHECK_FALSE(std::filesystem::exists(capture.folder));
    CHECK_FALSE(review->folder().has_value());
}

// from tests/test_walkthrough_library_ui.py::test_a_draft_shows_that_it_is_recovering
TEST_CASE("a draft shows that it is recovering", "[test_walkthrough_library_ui][widget]") {
    Capture capture;
    writeFile(capture.folder / "draft.json", "{}");
    writeFile(capture.folder / "audio.wav", "RIFF");
    std::filesystem::remove(capture.folder / "walkthrough.json");
    Review review(true, true);
    review->show();
    review->showFolder(capture.folder);
    CHECK(review->recoverButton()->isEnabled());
    review->recoverButton()->click();
    CHECK(review.host.recovered.size() == 1);
    CHECK(review->notice()->isVisible());
    CHECK(contains(review->notice()->text(), "Recovering"));
    CHECK_FALSE(review->recoverButton()->isEnabled());
}

// Not in OpenWhisper's tests: a refused delete asks once and leaves the walkthrough alone.
TEST_CASE("a delete that is not confirmed keeps the walkthrough", "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review.answer = QMessageBox::No;
    review->deleteFolder();
    CHECK(review.questions == 1);
    CHECK_FALSE(review->isBusy());
    CHECK(std::filesystem::is_directory(capture.folder));
}

} // namespace fvtest
