"""Curated inputs for services/automation/permissions.py (M5.3 automation-core).

    require(settings, permission)   None when granted, else the PermissionDenied's
                                    [permission, message]
    tables()                        ALL in order and PUBLIC_NAMES
"""


def _require(module, settings, permission):
    try:
        module.require(settings, permission)
    except module.PermissionDenied as exc:
        return [exc.permission, exc.message]
    return None


WRAPPERS = {
    "require": _require,
    "tables": lambda m: {"ALL": list(m.ALL), "PUBLIC_NAMES": dict(m.PUBLIC_NAMES)},
}

_KEYS = ["api_live_events", "api_run_actions", "api_control_recording", "api_text_filters",
         "api_agent_prompts"]
_VALUES = [True, False, "true", "false", 1, 0, None, [], {}, "yes", 1.0]
_SETTINGS = [None, {}] + [{key: value} for key in _KEYS for value in _VALUES]
_SETTINGS += [
    {key: True for key in _KEYS},
    {key: False for key in _KEYS},
    {"api_live_events": False, "api_run_actions": True, "api_agent_prompts": "on"},
]

CASES = {
    "granted": (
        [[settings, key] for settings in _SETTINGS for key in _KEYS]
        + [[{}, "mcp_enabled"], [{"mcp_enabled": True}, "mcp_enabled"], [{}, ""],
           [{}, "API_LIVE_EVENTS"], [None, "transcript_cleanup_enabled"]]
    ),
    "require": (
        [[{}, key] for key in _KEYS]
        + [[{key: True}, key] for key in _KEYS]
        + [[{}, "mcp_enabled"], [{"mcp_enabled": True}, "mcp_enabled"]]
    ),
    "describe": [[settings] for settings in _SETTINGS],
    "tables": [[]],
}
