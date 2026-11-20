"""Curated inputs for meeting/agent/evidence.py (M8.2 agent-prompts-and-policy).

    repair_evidence_ids(ops, citable_ids, existing)   `exists` is membership in `existing`;
                                                     the result is [repaired_ops, count]
    _repair_one(bad_id, citable)
"""

_KNOWN = [
    "sg_5f2c721de88ac0aa2321",
    "sg_e991e94d6decd27d036a",
    "sg_025f2c0b2c30e3e8dcb3",
    "sg_5f2c721de88ac0aa2329",
]
_REBUILT = _KNOWN + ["sg_97bd44598ee4da9dc5f9"]
# Two ids one edit apart from the same corrupted id: the margin guard must refuse both.
_TWINS = ["sg_aaaaaaaaaaaaaaaaaaaa", "sg_aaaaaaaaaaaaaaaaaaab"]
# Close in ratio but more than the margin apart.
_NEAR = ["sg_0123456789abcdef0123", "sg_0123456789abcdffffff", "sg_fedcba9876543210fedc"]


def _repair(module, ops, citable, existing):
    before = repr(ops)
    repaired, count = module.repair_evidence_ids(ops, citable, lambda sid: sid in existing)
    assert repr(ops) == before, "repair_evidence_ids changed its input"
    return [repaired, count]


CASES = {
    "repair_evidence_ids": [
        [[{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], _KNOWN, _KNOWN],
        [[{"op": "set_topic", "evidence": ["sg_025f2c0b2c30e3e8dcb4"]}], _KNOWN, _KNOWN],
        [[{"op": "set_topic", "evidence": ["sg_97bd44598ee4f6c7091f"]}], _REBUILT, _REBUILT],
        [[{"op": "set_topic", "evidence": ["sg_5f2c921de38ac0af2321"]}], _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_5f2c721de88ac0aa23"]}], _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_ffffffffffffffffffff"]}], _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": [_KNOWN[0], "q_123", 7, None, ["x"]]}], _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_025f2c0b2c30e3e8dcb4"]},
          {"op": "add_item", "evidence": ["sg_025f2c0b2c30e3e8dcb4", "sg_e991e94d6decd27d03"]}],
         _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_aaaaaaaaaaaaaaaaaaac"]}], _TWINS, _TWINS],
        [[{"op": "add_item", "evidence": ["sg_0123456789abcdef0124"]}], _NEAR, _NEAR],
        [[{"op": "add_item", "evidence": ["sg_0123456789abcd"]}], _NEAR, _NEAR],
        [[{"op": "add_item", "evidence": ["789abcdef"]}], _NEAR, _NEAR],
        [[{"op": "add_item", "evidence": ["sg_89abcdef01"]}], _NEAR, _NEAR],
        [[{"op": "add_item", "evidence": []}, {"op": "add_item"},
          {"op": "add_item", "evidence": "sg_e991e94d6decd27d03"}, "junk", None, 5],
         _KNOWN, _KNOWN],
        # The citable universe may hold ids that do not exist yet (existence is separate).
        [[{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], _KNOWN, []],
        [[{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], [], _KNOWN],
        # Duplicated citable ids count once, so the unique prefix still resolves.
        [[{"op": "add_item", "evidence": ["sg_e991"]}],
         [_KNOWN[1], _KNOWN[1], _KNOWN[0]], _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_"]}], _KNOWN, _KNOWN],
        [[{"op": "add_item", "evidence": ["sg_x", "sg_x"], "text": "keeps other keys",
           "data": {"start_s": 1}}], _KNOWN, _KNOWN],
    ],
    "_repair_one": [
        ["sg_e991e94d6decd27d03", _KNOWN],
        ["sg_025f2c0b2c30e3e8dcb4", _KNOWN],
        ["sg_97bd44598ee4f6c7091f", _REBUILT],
        ["sg_5f2c921de38ac0af2321", _KNOWN],
        ["sg_5f2c721de88ac0aa23", _KNOWN],
        ["sg_aaaaaaaaaaaaaaaaaaac", _TWINS],
        ["sg_0123456789abcdef0124", _NEAR],
        ["sg_0123456789abcdffff", _NEAR],
        ["sg_", _NEAR],
        ["q_0123456789abcdef0123", _NEAR],
        ["", _NEAR],
        ["sg_zz", []],
    ],
}

WRAPPERS = {
    "repair_evidence_ids": _repair,
}
