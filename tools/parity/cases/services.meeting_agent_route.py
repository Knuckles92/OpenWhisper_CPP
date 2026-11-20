"""Curated inputs for services/meeting_agent_route.py (M8.4 checkpoint-scheduler).

    resolve_meeting_agent_route(settings, meeting)
        {kind, provider, model, endpoint, installed}: the route without payload_dir, which
        FioriVoce does not have (its built-in loop has no packaged payload). The packaged
        payload lookup answers None and a server the user runs is not asked about its models
        (the C++ function compared is the one that asks nothing).
"""

_BASE = {"meeting_llm_provider": "openrouter", "meeting_llm_model": "deepseek/current"}


def _route(module, settings, meeting):
    import services.components as components
    import services.text_llm as text_llm

    saved = (components.meeting_agent_payload_dir, text_llm.learn_server_models)
    components.meeting_agent_payload_dir = lambda kind="pi": None
    text_llm.learn_server_models = lambda *args, **kwargs: None
    try:
        route = module.resolve_meeting_agent_route(settings, meeting)
    finally:
        components.meeting_agent_payload_dir, text_llm.learn_server_models = saved
    return {"kind": route.kind, "provider": route.provider, "model": route.model,
            "endpoint": route.endpoint, "installed": route.installed}


_ENDPOINT_ROW = {
    "agent_provider": "custom_box",
    "agent_model": "local-model",
    "agent_endpoint_json": (
        '{"profile_id": "custom_box", "name": "My box", "kind": "custom", '
        '"base_url": "http://127.0.0.1:9000/v1", "api_key_env": ""}'
    ),
}

CASES = {
    "resolve_meeting_agent_route": [
        # An installed agent chosen now, with and without a model of its own.
        [{"meeting_agent_core": "codex",
          "meeting_agent_models": {"codex": "gpt-6-sol", "bogus": "x"}}, None],
        [{"meeting_agent_core": "claude_code"}, None],
        [{"meeting_agent_core": "opencode_cli",
          "meeting_agent_models": {"opencode_cli": " prov/fast "}}, None],
        # A meeting recorded with an installed agent keeps it, whatever is chosen now.
        [{"meeting_agent_core": "direct"},
         {"agent_provider": "opencode_cli", "agent_model": "prov/fast"}],
        [dict(_BASE, meeting_agent_core="pi"), {"agent_provider": "claude_code"}],
        # A text-endpoint meeting re-run on a newly chosen agent.
        [{"meeting_agent_core": "claude_code"},
         {"agent_provider": "openrouter", "agent_model": "deepseek/v4"}],
        # The built-in loop for new meetings: pi, the packaged opencode, the retired direct.
        [dict(_BASE, meeting_agent_core="pi"), None],
        [dict(_BASE, meeting_agent_core="opencode"), None],
        [dict(_BASE, meeting_agent_core="direct"), None],
        [dict(_BASE), None],
        [{}, None],
        [dict(_BASE, meeting_agent_core="unknown"), None],
        # ... and for re-runs of stored meetings.
        [dict(_BASE, meeting_agent_core="pi"),
         {"agent_provider": "openrouter", "agent_model": "deepseek/saved"}],
        [dict(_BASE, meeting_agent_core="direct"),
         {"agent_provider": "openrouter", "agent_model": "deepseek/saved"}],
        [dict(_BASE, meeting_agent_core="opencode"), {"agent_provider": "openrouter"}],
        [dict(_BASE, meeting_agent_core="pi"), {"agent_model": "old/model"}],
        [dict(_BASE, meeting_agent_core="pi"), {"id": "m1"}],
        [dict(_BASE, meeting_agent_core="pi"), _ENDPOINT_ROW],
        [dict(_BASE, meeting_agent_core="pi"), {}],
    ],
}

WRAPPERS = {"resolve_meeting_agent_route": _route}
