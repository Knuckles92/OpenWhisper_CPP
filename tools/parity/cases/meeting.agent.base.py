"""Curated inputs for meeting/agent/base.py merge_usage (M8.2 agent-prompts-and-policy).

    merge_usage(start, usages)   a copy of `start` after merge_usage(total, usage) for each
                                 usage in order; a dict usage becomes an object with those
                                 attributes (Python reads the response's attributes), anything
                                 else is passed as it is
"""

from types import SimpleNamespace


def _merge(module, start, usages):
    total = dict(start)
    for usage in usages:
        module.merge_usage(total, SimpleNamespace(**usage) if isinstance(usage, dict) else usage)
    return total


CASES = {
    "merge_usage": [
        [{}, []],
        [{}, [None]],
        [{}, [{"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}]],
        [{}, [{"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
              {"prompt_tokens": 1, "completion_tokens": 2, "total_tokens": 3}]],
        [{}, [{"prompt_tokens": 10.5, "completion_tokens": "5", "total_tokens": None}]],
        [{}, [{"prompt_tokens": 7}, None, {}]],
        [{}, [{"prompt_tokens": True, "completion_tokens": False}]],
        [{}, ["not an object", 3, []]],
        [{"prompt_tokens": 1.5, "requests": 2, "cost": 0.1}, [{"prompt_tokens": 4}]],
        [{"total_tokens": 100}, [{"total_tokens": -10}, {"total_tokens": 0}]],
        [{"requests": 1.0}, [{"cached_tokens": 9}]],
    ],
}

WRAPPERS = {"merge_usage": _merge}
