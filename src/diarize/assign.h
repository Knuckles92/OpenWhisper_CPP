#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "diarize/clustering.h"

// Loopback speaker assignment shared by live chunks, the end-of-meeting re-decode and the Past
// Meetings retry, so that the three slice audio per segment and refresh labels the same way.
namespace fv::diarize {

// Labels `segments` in place from `frames` (mono int16 at `rate` Hz, frames[0] at meeting time
// `origin_s`): each segment's slice, from round((start - origin) * rate) to
// round((end - origin) * rate) clamped to the buffer, is converted to float 16 kHz and handed to
// the diarizer. A labelled segment gets the participant and speaker_source "diarizer"; a segment
// whose slice is empty is skipped without a word; a diarizer that throws is logged and counts as
// no label; `on_unlabeled` hears of every segment left without one. Returns the ids of the
// labelled segments, in input order.
std::vector<std::string> assign_from_frames(Diarizer& diarizer, std::vector<Segment>& segments,
                                            std::span<const std::int16_t> frames, int rate,
                                            double origin_s,
                                            std::function<void(const Segment&)> on_unlabeled = {});

// Re-reads the labels a later re-cluster may have changed after assign returned: segments not
// stored yet cannot receive relabel ops, so a batch caller refreshes them before committing.
// `segments` are ones that diarizer labelled; a failure or an empty label leaves a segment as it
// is.
void refresh_labels(Diarizer& diarizer, std::vector<Segment>& segments);

} // namespace fv::diarize
