#include "walkthrough/renderer.h"

#include <mutex>
#include <utility>
#include <vector>

namespace fv::walkthrough {

namespace {

struct Installed {
    std::mutex mutex;
    std::shared_ptr<DocumentRenderer> renderer;
    // Renderers replaced while a caller may still hold the reference document_renderer() gave
    // it: kept until the process exits, so that reference never dangles.
    std::vector<std::shared_ptr<DocumentRenderer>> replaced;
};

Installed& installed() {
    static Installed value;
    return value;
}

} // namespace

void set_document_renderer(std::shared_ptr<DocumentRenderer> renderer) {
    auto& slot = installed();
    std::lock_guard lock(slot.mutex);
    if (slot.renderer) {
        slot.replaced.push_back(std::move(slot.renderer));
    }
    slot.renderer = std::move(renderer);
}

DocumentRenderer& document_renderer() {
    auto& slot = installed();
    std::lock_guard lock(slot.mutex);
    if (!slot.renderer) {
        throw std::logic_error("No walkthrough document renderer is installed");
    }
    return *slot.renderer;
}

} // namespace fv::walkthrough
