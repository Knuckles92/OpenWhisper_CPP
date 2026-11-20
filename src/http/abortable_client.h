#pragma once

#include <expected>
#include <memory>

#include "base/cancel.h"
#include "http/client.h"

namespace fv::http {

// A Client whose every transfer, running or still to come, can be aborted at once: Quit's
// "abort every transfer". Each send listens to the caller's token and to this client's own, so
// a request waiting on a server that never answers (connect 10 s, idle 120 s) ends within about a
// second of abort_all() (curl's transfer callback runs at least once a second), with
// ErrorKind::Cancelled, as a cancel by its caller would. OpenWhisper abandons such requests on
// their daemon threads at quit; a C++ task that uses the app's services cannot be abandoned, so
// the request is ended instead.
// Affinity: thread-safe.
class AbortableClient final : public Client {
public:
    explicit AbortableClient(std::shared_ptr<Client> inner);

    std::expected<Response, Error> send(const Request&, const CancelToken&,
                                        BodySink* stream = nullptr) override;

    // Cancels every transfer in flight and every later one. Not undone.
    void abort_all();
    bool aborted() const;

private:
    std::shared_ptr<Client> inner_;
    CancelSource abort_;
};

} // namespace fv::http
