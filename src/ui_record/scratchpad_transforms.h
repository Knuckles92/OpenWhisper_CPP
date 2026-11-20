#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/executor.h"
#include "base/json.h"
#include "text/transforms.h"
#include "ui_record/scratchpad.h"

namespace fv {

class SettingsStore;

// What the Scratchpad's Transform menu reads and runs on (OpenWhisper reaches the same through
// its settings manager and text_rewrite).
struct TransformEnvironment {
    // Rewrites `text` by `instruction` with `settings` as they were when the transform started:
    // (result, nullopt) or ("", the short reason). llm::rewrite_standalone over the HTTP client
    // and the key sources. It blocks, and never sees the GUI thread. Required.
    using Rewriter = std::function<std::pair<std::string, std::optional<std::string>>(
        std::string_view text, std::string_view instruction, const Json& settings)>;

    // text_transforms and the settings the rewrite reads. Required.
    SettingsStore* settings = nullptr;
    // Where the rewrite runs (the jobs strand): it waits on the network for up to a minute. It
    // outlives the transforms and the work queued on it. Required.
    Executor* jobs = nullptr;
    // Where the result is delivered. Required.
    Executor* ui = nullptr;
    Rewriter rewrite;
};

// The Scratchpad's Transform menu and what a choice does (scratchpad.py fill_transform_menu,
// apply_transform, _on_transform_finished): the menu lists the saved transforms, and the choice
// rewrites the selection, or all the text, with the AI cleanup model while the editor is
// read-only. The result replaces exactly the text that was read, even when text was added at
// either end meanwhile. It never logs what was rewritten, only its length, and a window deleted
// while the model works is not held by the work: the result is dropped.
// Affinity: GUI thread (the rewrite runs on the jobs strand).
class ScratchpadTransforms : public std::enable_shared_from_this<ScratchpadTransforms> {
public:
    explicit ScratchpadTransforms(TransformEnvironment environment);
    ~ScratchpadTransforms();
    ScratchpadTransforms(const ScratchpadTransforms&) = delete;
    ScratchpadTransforms& operator=(const ScratchpadTransforms&) = delete;

    // What ScratchpadWindow::setTransformMenuFiller takes: one action per saved transform, or
    // the disabled "No transforms yet". It holds this object, which must be owned by a
    // shared_ptr. A load that throws is the window's to log and answer with "No transforms yet".
    ScratchpadWindow::TransformMenuFiller filler();

    // Rewrites the selection of `window` (its whole text when nothing is selected) with
    // `transform`. Says so in the status line when there is nothing to transform.
    void apply(ScratchpadWindow& window, const text::Transform& transform);

private:
    struct Run;

    // The run state of `window`, made on first use and dropped with the window.
    std::shared_ptr<Run> runOf(ScratchpadWindow& window);

    TransformEnvironment environment_;
    std::shared_ptr<const TransformEnvironment::Rewriter> rewrite_;
    // The connections of the windows seen: dropped with this object.
    std::unique_ptr<QObject> context_;
    std::map<const ScratchpadWindow*, std::shared_ptr<Run>> runs_;
};

} // namespace fv
