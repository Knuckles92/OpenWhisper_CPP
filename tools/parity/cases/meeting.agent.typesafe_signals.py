"""Curated inputs for meeting/agent/typesafe_signals.py (M8.4 checkpoint-scheduler).

    constants()                              the instruction and criteria strings and limits
    topic_shift_probability(older, window, answer)
    voice_command_choice(segment, previous, names, answer)
        [result, calls]: the helper's result over a recording judge that answers `answer`
        (null: no judgment) and the questions it was asked, each [method, state,
        instructions, criteria or null, timeout_s or null]. A choice answer is
        [choice, confidence] and its result {choice, confidence, probabilities}.
"""

from services.typesafe import ChoiceAnswer


class _Judge:
    def __init__(self, answer):
        self.answer = answer
        self.calls = []

    def noul(self, state, instructions, criteria=None, timeout_s=None):
        self.calls.append(["noul", state, instructions, criteria, timeout_s])
        return self.answer

    def choice(self, state, instructions, criteria, timeout_s=None):
        self.calls.append(["choice", state, instructions, dict(criteria), timeout_s])
        if self.answer is None:
            return None
        return ChoiceAnswer(choice=self.answer[0], confidence=self.answer[1])


def _topic(module, older, window, answer):
    judge = _Judge(answer)
    return [module.topic_shift_probability(judge, older, window), judge.calls]


def _voice(module, segment, previous, names, answer):
    judge = _Judge(answer)
    result = module.voice_command_choice(judge, segment, previous, names)
    if result is not None:
        result = {"choice": result.choice, "confidence": result.confidence,
                  "probabilities": result.probabilities}
    return [result, judge.calls]


def _constants(module):
    return {
        "TOPIC_CHANGED_INSTRUCTIONS": module.TOPIC_CHANGED_INSTRUCTIONS,
        "TOPIC_TIMEOUT_S": module.TOPIC_TIMEOUT_S,
        "_WINDOW_MAX_CHARS": module._WINDOW_MAX_CHARS,
        "VOICE_COMMAND_CONFIDENCE": module.VOICE_COMMAND_CONFIDENCE,
        "VOICE_COMMAND_CRITERIA": dict(module.VOICE_COMMAND_CRITERIA),
        "VOICE_COMMAND_INSTRUCTIONS": module.VOICE_COMMAND_INSTRUCTIONS,
    }


_LONG_OLD = "o" * 2000 + "middle" + "n" * 4500 + "TAIL"
_LONG_NEW = "HEAD" + "h" * 6100 + "end"

CASES = {
    "constants": [[]],
    "topic_shift_probability": [
        ["budget talk", "hiring plans", 0.7],
        ["budget talk", "hiring plans", None],
        ["  budget talk \n", "\thiring plans  ", 0.12],
        ["", "hiring plans", 0.5],
        ["budget talk", "   ", 0.5],
        [_LONG_OLD, _LONG_NEW, 0.91],
        ["é" * 6001, "ü" * 6002, 0.3],
        ["x" * 6000, "y" * 6000, 1.0],
    ],
    "voice_command_choice": [
        ["Assistant, mark that as a decision.", ["We ship Friday.", "", "Agreed."],
         ["assistant", "openwhisper-cpp"], ["mark_decision", 0.93]],
        ["  note taker, replace Acme with Acme Labs  ", ["a", "b", "c", "d", ""],
         ["note taker"], ["fix_transcript", 0.71]],
        ["Assistant recap please", [], [], None],
        ["   ", ["a"], ["assistant"], ["none", 0.9]],
        ["", [], ["assistant"], ["none", 0.9]],
        ["Assistant, new topic: hiring", ["", "", ""], ["assistant", "helper"],
         ["set_topic", 0.6]],
    ],
}

WRAPPERS = {
    "constants": _constants,
    "topic_shift_probability": _topic,
    "voice_command_choice": _voice,
}
