#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "net/tls.h"
#include "net/types.h"
#include "pyport/json.h"
#include "remote_wire/scripted_peer.h"
#include "remote_wire/transcript.h"

// Replaying the host half of M9.3's recorded OpenWhisper exchanges to the client under test.
namespace fv::remote::test {

namespace wire = fv::testing::remote_wire;

inline const net::TlsIdentity& peer_identity() {
    static const net::TlsIdentity identity = [] {
        auto made = net::generate_self_signed_p256("OpenWhisper host studio");
        made.disable_tickets = true;
        return made;
    }();
    return identity;
}

// The steps of a recorded scenario the client's own calls reproduce, in recorded order.
inline wire::Transcript recorded_steps(std::string_view scenario,
                                       std::initializer_list<const char*> steps) {
    std::vector<std::string> names(steps.begin(), steps.end());
    return wire::only_steps(wire::load_transcript(scenario), names);
}

// Recorded frame `index` as the peer played it, parsed: its placeholders hold the values the
// peer bound (the peer's own fingerprint, the token it issued).
inline Json played(const wire::Transcript& transcript, std::size_t index,
                   const wire::ScriptedPeer& peer) {
    Json bindings = peer.bindings();
    return py::loads(wire::render(transcript, index, bindings).text);
}

// The value a placeholder ("<token#1>") was bound to.
inline std::string bound(const wire::ScriptedPeer& peer, const std::string& placeholder) {
    const Json bindings = peer.bindings();
    const auto found = bindings.find(placeholder);
    return found != bindings.end() && found->is_string() ? found->get<std::string>() : "";
}

// Indexes of the recorded frames of `conn` that went `dir` with `kind`.
inline std::vector<std::size_t> frames_where(const wire::Transcript& transcript, int conn,
                                             std::string_view dir, std::string_view kind) {
    std::vector<std::size_t> out;
    for (const std::size_t index : wire::frames_of(transcript, conn)) {
        const auto& frame = transcript.frames[index];
        if (frame.dir == dir && frame.kind == kind) {
            out.push_back(index);
        }
    }
    return out;
}

} // namespace fv::remote::test
