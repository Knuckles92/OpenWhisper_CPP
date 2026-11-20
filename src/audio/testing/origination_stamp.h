#pragma once

#include <chrono>
#include <functional>

#include "audio/wav_metadata.h"

// A seam for tests of the save: the stamp WavWriter::commit applies to the WAV it has written.
namespace fv::audio::testing {

// Called with the WAV and the local time of the save; append_origination while none is set.
using OriginationStamp = std::function<void(RiffFile&, std::chrono::local_seconds)>;

// Replaces the stamp (null: append_origination again) and returns the previous one, as
// OpenWhisper's tests patch services.recorder.append_origination to make it fail. Process-wide.
// Affinity: thread-safe.
OriginationStamp exchange_origination_stamp(OriginationStamp replacement);

} // namespace fv::audio::testing
