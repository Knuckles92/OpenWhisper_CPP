"""Curated inputs for meeting/voice_help.py (M7.13 meeting-web-hub).

    voice_command_guide(settings)                   the guide with resolve_typesafe_voice_
                                                    command_names reading `settings`
    resolve_typesafe_voice_command_names(settings)  services/settings.py, as a list

`settings` is a settings mapping. OpenWhisper's defaults include its own wake names; the replay
passes legacy_identity() as the source of the defaults (OpenWhisper_CPP's defaults are identity()'s).
"""

_KEY = "typesafe_voice_command_names"

_SETTINGS = [
    {},
    {_KEY: None},
    {_KEY: []},
    {_KEY: ["  ", ""]},
    {_KEY: "not a list"},
    {_KEY: ["Note Taker", "notetaker"]},
    {_KEY: ["notetaker"]},
    {_KEY: ["notetaker", "note taker"]},
    {_KEY: ["  Open   Whisper  ", "OPENWHISPER"]},
    {_KEY: ["scribe", "assistant", "Assistant", "helper"]},
    {_KEY: ["helper", 5, None, "ASSISTANT"]},
    {_KEY: ["écho", "ÉCHO"]},
    {_KEY: ["note taker", "scribe\tbot"]},
    {_KEY: ["İstanbul"]},
    {_KEY: ["open whisper", "assistant", "note taker"]},
]


def _guide(module, settings):
    real = module.resolve_typesafe_voice_command_names
    module.resolve_typesafe_voice_command_names = lambda: real(settings)
    try:
        return module.voice_command_guide()
    finally:
        module.resolve_typesafe_voice_command_names = real


def _names(module, settings):
    from services.settings import resolve_typesafe_voice_command_names

    return list(resolve_typesafe_voice_command_names(settings))


CASES = {
    "voice_command_guide": [[settings] for settings in _SETTINGS],
    "resolve_typesafe_voice_command_names": [[settings] for settings in _SETTINGS],
}

WRAPPERS = {
    "voice_command_guide": _guide,
    "resolve_typesafe_voice_command_names": _names,
}
