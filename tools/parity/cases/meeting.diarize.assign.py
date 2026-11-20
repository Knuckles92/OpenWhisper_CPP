"""Curated inputs for meeting/diarize/assign.py (fv_diarize assign_from_frames).

A case is [segments, frame_count, seed, rate, origin_s]: segments are [segment_id, start_s,
end_s]; the frames are frame_count int16 samples of the 64-bit LCG that tests/dsp/signals.h
(and tests/diarize/test_assign.cpp) repeat exactly. A stub diarizer records each call and
answers "p_<segment_id>", except a segment whose id ends in "_none" (no label) or "_boom"
(raises, which assign_from_frames logs and counts as no label).

Result: {"labeled": the ids returned, "unlabeled": how often on_unlabeled ran, "calls": per
assign [segment_id, sample count, rate, the first and the last four samples], "speakers":
[segment_id, speaker_participant_id, speaker_source] for every segment}. The samples are the
float32 prepare_for_whisper made (resampled to 16 kHz with fft_resample when the rate differs).
"""

import numpy as np

FLOAT_TOLERANCE = 1e-6

_MASK64 = (1 << 64) - 1


def _lcg_int16(count, seed):
    state = seed & _MASK64
    out = []
    for _ in range(count):
        state = (state * 6364136223846793005 + 1442695040888963407) & _MASK64
        out.append((state >> 48) - 32768)
    return np.asarray(out, dtype=np.int16)


class _Stub:
    def __init__(self):
        self.calls = []

    def assign(self, segment, audio, rate):
        audio = np.asarray(audio)
        assert audio.dtype == np.float32
        self.calls.append([segment.segment_id, int(audio.size), int(rate),
                           [float(str(v)) for v in audio[:4]],
                           [float(str(v)) for v in audio[-4:]]])
        if segment.segment_id.endswith("_boom"):
            raise RuntimeError("diarizer failed")
        if segment.segment_id.endswith("_none"):
            return None
        return f"p_{segment.segment_id}"


def _assign_from_frames(module, segments, frame_count, seed, rate, origin_s):
    from meeting.interfaces import TranscriptSegment

    frames = _lcg_int16(frame_count, seed)
    objects = [TranscriptSegment(segment_id=s[0], meeting_id="m_assign", chunk_id=1,
                                 channel="loopback", start_s=s[1], end_s=s[2], text="x")
               for s in segments]
    stub = _Stub()
    unlabeled = []
    labeled = module.assign_from_frames(stub, objects, frames, rate, origin_s,
                                        on_unlabeled=lambda: unlabeled.append(1))
    return {
        "labeled": [s.segment_id for s in labeled],
        "unlabeled": len(unlabeled),
        "calls": stub.calls,
        "speakers": [[s.segment_id, s.speaker_participant_id, s.speaker_source] for s in objects],
    }


WRAPPERS = {"assign_from_frames": _assign_from_frames}

CASES = {
    "assign_from_frames": [
        # 16 kHz: slices by round((t - origin) * rate), half to even at .5 samples.
        [[["a", 0.0, 0.25], ["b", 0.25, 0.5], ["c_none", 0.5, 0.75], ["d", 0.7500312, 1.0]],
         16000, 5, 16000, 0.0],
        # Clamping: before the buffer, past its end, entirely outside, reversed, empty.
        [[["early", -1.0, 0.1], ["late", 0.9, 5.0], ["outside", 2.0, 3.0],
          ["reversed", 0.6, 0.4], ["empty", 0.3, 0.3], ["boom_boom", 0.2, 0.4]],
         16000, 6, 16000, 0.0],
        # A meeting-time origin: frames[0] is 120 s into the meeting.
        [[["o1", 120.0, 120.5], ["o2_none", 120.5, 121.0], ["o3", 119.0, 119.9]],
         16000, 7, 16000, 120.0],
        # 48 kHz and 44.1 kHz loopback audio, resampled to 16 kHz.
        [[["r1", 0.0, 0.1], ["r2", 0.1, 0.25], ["r3", 0.03125, 0.0937]], 14400, 8, 48000, 0.0],
        [[["s1", 0.0, 0.2], ["s2", 0.2, 0.2000113], ["s3", 0.15, 0.4]], 17640, 9, 44100, 0.0],
        # Half-sample positions: 0.5 rounds to even.
        [[["h1", 0.0000625, 0.0001875], ["h2", 0.0001875, 0.0003125]], 64, 10, 8000, 0.0],
    ],
}
