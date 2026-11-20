#include "http/abortable_client.h"

#include <stdexcept>
#include <utility>

#include "base/subscription.h"

namespace fv::http {

AbortableClient::AbortableClient(std::shared_ptr<Client> inner) : inner_(std::move(inner)) {
    if (inner_ == nullptr) {
        throw std::invalid_argument("AbortableClient needs a client");
    }
}

std::expected<Response, Error> AbortableClient::send(const Request& request,
                                                     const CancelToken& cancel, BodySink* stream) {
    // A child of the abort source, cancelled by the caller's token too for as long as the
    // request runs.
    CancelSource transfer(abort_.token());
    const Subscription forward = cancel.on_cancel([transfer]() mutable { transfer.cancel(); });
    return inner_->send(request, transfer.token(), stream);
}

void AbortableClient::abort_all() {
    abort_.cancel();
}

bool AbortableClient::aborted() const {
    return abort_.cancelled();
}

} // namespace fv::http
