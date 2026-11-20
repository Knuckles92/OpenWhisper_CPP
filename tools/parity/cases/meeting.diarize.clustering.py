"""Curated inputs for meeting/diarize/clustering.py (fv_diarize OnlineDiarizer, average_linkage).

"_average_linkage" takes [matrix, threshold] and returns the groups as lists of row indices.
The matrices exercise ties (row-major first minimum), a distance exactly at the threshold, a
chain, infinities and the empty and single inputs.

"replay" is no function of the module: it drives an OnlineDiarizer through a script of events
and records what it did. Its fake embedder hands out the scripted vectors in order (null: no
embedding), its fake store numbers new participants p_speaker_<n> as OpenWhisper's tests'
FakeStore does, and every assign gets one second of silence at 16 kHz (the embedder ignores
it). The C++ replay (tests/diarize/test_clustering.cpp) runs the same script through
FakeEmbeddingModel and FakeDiarizerHost. Events:

    ["assign", segment_id, start_s, end_s, vector | null]
    ["pin", segment_id, participant_id]

The vectors are unit float32 in 16 dimensions, built so that every similarity the diarizer
compares sits far from its thresholds (orthogonal speakers, small noise): the outcome must not
depend on the last bit of a dot product. The re-cluster runs when its 25-embedding trigger
fires; the replay is far shorter than the 60 s timer.
Result: {"labels": what each assign returned, "ops": each relabel callback's
[segment, participant] pairs, "participants": names created, "final": [segment, current_label]
for every segment the script names, in first-mention order, "clusters": [participant, count,
sorted pinned segments] in the diarizer's order}.
"""

import numpy as np

_DIM = 16


def _unit(vector):
    a = np.asarray(vector, dtype=np.float64)
    return (a / np.linalg.norm(a)).astype(np.float32)


def _speaker_vector(rng, axis, sigma):
    base = np.zeros(_DIM)
    base[axis] = 1.0
    return _unit(base + sigma * rng.normal(size=_DIM))


def _floats(vector):
    return [float(str(v)) for v in vector]


def _three_speakers():
    """Three orthogonal voices with a pin before the segment exists, a pin on a never-embedded
    segment, segments without an embedding that inherit, and human pins whose weight moves whole
    groups to the pinned participants on the re-cluster."""
    rng = np.random.RandomState(1234)
    events = [["pin", "sg_00", "p_human"]]
    order = [0, 1, 0, 2, 1, 0, 2, 2, 1, 0, 1, 2, 0, 0, 1, 2, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 2,
             1, 0, 2, 1, 0, 2, 0, 1, 2, 1, 0, 2]
    t = 0.0
    for i, speaker in enumerate(order):
        segment = f"sg_{i:02d}"
        vector = None if i % 9 == 4 else _floats(_speaker_vector(rng, speaker, 0.08))
        events.append(["assign", segment, t, t + 1.2, vector])
        t += 1.5
        if i == 12:
            events.append(["pin", "mic_1", "p_speaker_1"])
        if i == 15:
            # sg_01 (speaker 1, which seeded p_speaker_1) to speaker 0's participant: the pin's
            # weight carries speaker 1's whole group over on the next re-cluster.
            events.append(["pin", "sg_01", "p_speaker_2"])
    return events


def _max_clusters():
    """Twelve orthogonal voices: the eleventh and twelfth meet ten clusters and are forced into
    the first (all similarities 0.0, numpy's argmax takes the first); the re-cluster at the 25th
    embedding sorts the claims on that participant."""
    events = []
    t = 0.0
    for i, speaker in enumerate(list(range(12)) + list(range(12)) + list(range(12))):
        vector = np.zeros(_DIM, dtype=np.float32)
        vector[speaker] = 1.0
        events.append(["assign", f"mx_{i:02d}", t, t + 1.0, _floats(vector)])
        t += 1.25
    return events


class _Store:
    def __init__(self):
        self.n = 0
        self.names = []

    def apply(self, actor_type, actor_id, ops):
        from meeting.interfaces import OpResult

        self.n += 1
        pid = f"p_speaker_{self.n}"
        self.names.append(ops[0]["display_name"])
        return [OpResult(ok=True, op=ops[0], target_id=pid,
                         effect={"participant": {"id": pid,
                                                 "display_name": ops[0]["display_name"]}})]


class _Embedder:
    def __init__(self, vectors):
        self._vectors = vectors
        self._next = 0
        self.available = True

    def embed(self, mono):
        vector = self._vectors[min(self._next, len(self._vectors) - 1)]
        self._next += 1
        return None if vector is None else vector.copy()


def _replay(module, events):
    from meeting.interfaces import TranscriptSegment

    vectors = [None if e[4] is None else np.asarray(e[4], dtype=np.float32)
               for e in events if e[0] == "assign"]
    store = _Store()
    diarizer = module.OnlineDiarizer(_Embedder(vectors), store, None, "m_replay")
    ops_log = []
    diarizer.set_relabel_callback(
        lambda ops: ops_log.append([[op["segment_id"], op["participant_id"]] for op in ops]))
    labels = []
    named = []
    silence = np.zeros(16000, dtype=np.float32)
    for event in events:
        if event[1] not in named:
            named.append(event[1])
        if event[0] == "assign":
            segment = TranscriptSegment(segment_id=event[1], meeting_id="m_replay", chunk_id=1,
                                        channel="loopback", start_s=event[2], end_s=event[3],
                                        text="x")
            labels.append(diarizer.assign(segment, silence, 16000))
        else:
            diarizer.pin(event[1], event[2])
    return {
        "labels": labels,
        "ops": ops_log,
        "participants": store.names,
        "final": [[segment, diarizer.current_label(segment)] for segment in named],
        "clusters": [[c.participant_id, c.count, sorted(c.pinned_segments)]
                     for c in diarizer._clusters],
    }


def _average_linkage(module, matrix, threshold):
    groups = module._average_linkage(np.asarray(matrix, dtype=np.float64).reshape(
        len(matrix), len(matrix)), threshold)
    return [[int(i) for i in group] for group in groups]


_INF = float("inf")

WRAPPERS = {"_average_linkage": _average_linkage, "replay": _replay}

CASES = {
    "_average_linkage": [
        # Two tight pairs far apart.
        [[[0.0, 0.1, 0.9, 0.95], [0.1, 0.0, 0.92, 0.9], [0.9, 0.92, 0.0, 0.05],
          [0.95, 0.9, 0.05, 0.0]], 0.38],
        # Every distance equal: row-major first pairs merge, everything joins one group.
        [[[0.0, 0.2, 0.2, 0.2], [0.2, 0.0, 0.2, 0.2], [0.2, 0.2, 0.0, 0.2],
          [0.2, 0.2, 0.2, 0.0]], 0.38],
        # A tie between (0, 3) and (1, 2): (0, 3) comes first row-major.
        [[[0.0, 0.5, 0.6, 0.1], [0.5, 0.0, 0.1, 0.7], [0.6, 0.1, 0.0, 0.8],
          [0.1, 0.7, 0.8, 0.0]], 0.38],
        # A distance exactly at the threshold still merges.
        [[[0.0, 0.38, 0.9], [0.38, 0.0, 0.9], [0.9, 0.9, 0.0]], 0.38],
        # A chain: average linkage stops once the averages pass the threshold.
        [[[0.0, 0.3, 0.6, 0.9, 1.2], [0.3, 0.0, 0.3, 0.6, 0.9], [0.6, 0.3, 0.0, 0.3, 0.6],
          [0.9, 0.6, 0.3, 0.0, 0.3], [1.2, 0.9, 0.6, 0.3, 0.0]], 0.38],
        [[[0.0, 0.3, 0.6, 0.9, 1.2], [0.3, 0.0, 0.3, 0.6, 0.9], [0.6, 0.3, 0.0, 0.3, 0.6],
          [0.9, 0.6, 0.3, 0.0, 0.3], [1.2, 0.9, 0.6, 0.3, 0.0]], 0.7],
        # Infinite distances never merge.
        [[[0.0, _INF, 0.1], [_INF, 0.0, _INF], [0.1, _INF, 0.0]], 0.38],
        # Weighted averages: a merged pair's distance to the third is the size-weighted mean.
        [[[0.0, 0.05, 0.3, 0.5, 0.45], [0.05, 0.0, 0.4, 0.5, 0.45], [0.3, 0.4, 0.0, 0.2, 0.9],
          [0.5, 0.5, 0.2, 0.0, 0.36], [0.45, 0.45, 0.9, 0.36, 0.0]], 0.38],
        [[[0.0]], 0.38],
        [[], 0.38],
        [[[0.0, 0.0], [0.0, 0.0]], 0.0],
    ],
    "replay": [[_three_speakers()], [_max_clusters()]],
}
