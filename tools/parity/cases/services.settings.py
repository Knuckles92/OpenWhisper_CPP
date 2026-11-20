"""Curated inputs for services/settings.py (M0.6 settings): the defaults table, the model
selection migration, the hotkey load, and the generic resolvers over every key.

The manager cases run a real SettingsManager on a file in out/parity (never OpenWhisper's) and
record what it returns and what the file holds afterwards. load_model_selection depends on the
date (OpenAI retires three models on 2027-02-26), so each case pins the day through
services.openai_retirement._today. The C++ replay is tests/settings/test_golden.cpp.
"""

import json
import shutil
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

WORK = ow_isolation.SCRATCH / "settings-cases"
#: Stands for "no settings file" in a case's arguments.
NO_FILE = None

JUNK = [
    None, True, False, 0, 1, -3, 7, 100, 3.7, "", " ", "yes", "no", "0", "12",
    "auto", "  EN ", "garbage", [], ["a"], [1, 2], {}, {"a": 1}, {"claude-code": "m"},
]


def _manager(stored):
    from services.settings import SettingsManager

    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    path = WORK / "settings.json"
    if stored is not NO_FILE:
        path.write_text(json.dumps(stored), encoding="utf-8")
    return SettingsManager(str(path)), path


def _file(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def _setting_defaults(module):
    return dict(module.SETTING_DEFAULTS)


def _load_model_selection(module, stored, today):
    from services import openai_retirement

    manager, path = _manager(stored)
    pinned = date.fromisoformat(today)
    original = openai_retirement._today
    openai_retirement._today = lambda: pinned
    try:
        result = manager.load_model_selection()
    finally:
        openai_retirement._today = original
    return {"result": result, "file": _file(path)}


def _load_hotkey_settings(module, stored):
    manager, _path = _manager(stored)
    return manager.load_hotkey_settings()


def _setting_value(module, key, settings):
    return module.setting_value(key, settings)


def _resolve_bool_setting(module, key, settings):
    return module.resolve_bool_setting(key, settings)


def _resolve_choice_setting(module, key, choices, settings):
    return module.resolve_choice_setting(key, tuple(choices), settings)


class _LazyCases(dict):
    """Filled when first read, after ow_isolation.isolate() has made the import safe."""

    def _fill(self):
        if dict.__len__(self):
            return
        from services import settings as module
        from services.setting_types import BoolSetting

        stored_selections = [
            NO_FILE, {}, {"selected_model": "api_whisper"},
            {"selected_model": "api_gpt4o"}, {"selected_model": "api_gpt4o_mini"},
            {"selected_model": "api_whisper", "api_transcription_model": "gpt-4o-mini-transcribe"},
            {"selected_model": "api_gpt4o", "api_transcription_model": "not-a-model"},
            {"selected_model": "api_gpt4o", "api_transcription_model": 7},
            {"selected_model": "local_whisper"}, {"selected_model": "parakeet"},
            {"selected_model": "remote"}, {"selected_model": "api"}, {"selected_model": 7},
            {"selected_model": "not-a-backend"}, {"selected_model": None},
            {"selected_model": "api_whisper", "unrelated": True, "ui_theme": "light"},
        ]
        dict.__setitem__(self, "SETTING_DEFAULTS", [[]])
        dict.__setitem__(self, "load_model_selection", [
            [stored, today] for today in ("2026-11-14", "2027-02-26")
            for stored in stored_selections])
        dict.__setitem__(self, "load_hotkey_settings", [
            [NO_FILE], [{}], [{"hotkeys": {"record_toggle": "f1"}}],
            [{"hotkeys": {"record_toggle": "f1", "cancel": "f2", "enable_disable": "ctrl+f3"}}],
            [{"hotkeys": "x"}], [{"hotkeys": None}], [{"hotkeys": []}], [{"other": 1}],
        ])
        value_cases = []
        bool_cases = []
        for setting in module.ALL_SETTINGS:
            key = str(setting)
            value_cases += [[key, {}], [key, {key: "junk"}], [key, {key: None}]]
            if isinstance(setting, BoolSetting):
                bool_cases.append([key, {}])
                bool_cases += [[key, {key: value}] for value in JUNK]
            else:
                bool_cases.append([key, {}])
        value_cases.append(["not_a_setting", {"not_a_setting": 1}])
        bool_cases.append(["not_a_setting", {}])
        dict.__setitem__(self, "setting_value", value_cases)
        dict.__setitem__(self, "resolve_bool_setting", bool_cases)
        dict.__setitem__(self, "resolve_choice_setting", [
            ["meeting_server_bind", ["localhost", "lan"], {}],
            ["meeting_server_bind", ["localhost", "lan"], {"meeting_server_bind": "lan"}],
            ["meeting_server_bind", ["localhost", "lan"], {"meeting_server_bind": "everywhere"}],
            ["ui_font_scale", [90, 100], {"ui_font_scale": 90.0}],
            ["ui_font_scale", [90, 100], {"ui_font_scale": True}],
            ["ui_font_scale", [1, 100], {"ui_font_scale": True}],
            ["auto_paste", [False], {"auto_paste": 0}],
            ["mcp_port", [1], {}],
        ])
        # M2.4: the API model functions, each with an ISO day for its `today`.
        days = ("2026-11-14", "2027-02-25", "2027-02-26", "2027-03-01")
        models = ("gpt-transcribe", "gpt-4o-transcribe", "gpt-4o-mini-transcribe", "whisper-1",
                  "whisper-large-v3", "", "GPT-TRANSCRIBE")
        dict.__setitem__(self, "api_model_choices", [[day] for day in days])
        dict.__setitem__(self, "api_model_label", [[m, day] for m in models for day in days])
        dict.__setitem__(self, "serving_api_model", [[m, day] for m in models for day in days])
        dict.__setitem__(self, "resolve_api_transcription_model", [
            [stored, day] for day in days for stored in (
                {}, {"api_transcription_model": "whisper-1"},
                {"api_transcription_model": "gpt-4o-mini-transcribe"},
                {"api_transcription_model": "gpt-transcribe"},
                {"api_transcription_model": "whisper-large-v3"},
                {"api_transcription_model": 7}, {"api_transcription_model": None},
                {"selected_model": "api_whisper"}, {"selected_model": "api_gpt4o"},
                {"selected_model": "api_gpt4o_mini"}, {"selected_model": "parakeet"},
                {"selected_model": 7},
                {"selected_model": "api_whisper", "api_transcription_model": "gpt-transcribe"},
                {"selected_model": "api_gpt4o", "api_transcription_model": "not-a-model"},
            )])
        # M4.4: the cleanup prompt and learned-rule resolvers.
        rules = "transcript_cleanup_rules"
        dict.__setitem__(self, "resolve_transcript_cleanup_prompt", [[stored] for stored in (
            {}, {"transcript_cleanup_prompt": "Make this a Slack message."},
            {"transcript_cleanup_prompt": "   "}, {"transcript_cleanup_prompt": 7},
            {"transcript_cleanup_level": "light"}, {"transcript_cleanup_level": "bogus"},
            {"transcript_cleanup_level": "high", "transcript_cleanup_enabled": False},
            {"transcript_cleanup_level": "high",
             "transcript_cleanup_prompt": "  Bullet points only. "},
        )])
        dict.__setitem__(self, "resolve_transcript_cleanup_rules", [[stored] for stored in (
            {}, {rules: "not a list"}, {rules: {"a": 1}}, {rules: None},
            {rules: ["  Rule one  ", "", "   ", 42, None, "Rule two"]},
            {rules: [f"Rule {i}" for i in range(60)]},
            {rules: ["　Full width　", " "]},
        )])
        dict.__setitem__(self, "compose_transcript_cleanup_prompt", [
            ["Base prompt.", []],
            ["Base prompt.", ['Spell "Alex Rivera".', "Expand SCWA."]],
            ["", ["Only."]],
            ["Base.", [f"Rule {i}" for i in range(12)]],
        ])
        # M4.2: the text-model resolvers of AI cleanup and meeting intelligence.
        dict.__setitem__(self, "default_transcript_cleanup_model", [[p] for p in (
            "openai", "openrouter", "ollama", "groq", "opencode_go", "claude_code", "codex",
            "opencode_cli", "custom_missing", "", "bogus")])
        for name in ("resolve_transcript_cleanup_provider", "resolve_transcript_cleanup_model",
                     "resolve_transcript_cleanup_reasoning"):
            dict.__setitem__(self, name, [[s] for s in CLEANUP_SETTINGS])
        for name in ("resolve_meeting_llm_provider", "resolve_meeting_llm_model",
                     "resolve_meeting_llm_profile", "resolve_meeting_llm_endpoint"):
            dict.__setitem__(self, name, [[s] for s in MEETING_SETTINGS])
        # M8.3: the Fast judgments gates, wake names, recall, knowledge folder and review.
        gates = [{}] + [
            {"typesafe_enabled": master, f"typesafe_{feature}_enabled": switch}
            for master in (False, True) for switch in (False, True, None, "yes")
            for feature in TYPESAFE_FEATURES]
        dict.__setitem__(self, "resolve_typesafe_enabled", [
            [s] for s in ({}, {"typesafe_enabled": True}, {"typesafe_enabled": False},
                          {"typesafe_enabled": "true"}, {"typesafe_enabled": 1})])
        dict.__setitem__(self, "resolve_typesafe_feature_enabled", [
            [feature, s] for s in gates for feature in TYPESAFE_FEATURES] + [
            ["bogus", {}], ["bogus", {"typesafe_enabled": True}]])
        for name, key in (("resolve_typesafe_topic_shift_enabled", "typesafe_topic_shift_enabled"),
                          ("resolve_typesafe_voice_commands_enabled",
                           "typesafe_voice_commands_enabled")):
            dict.__setitem__(self, name, [
                [{}]] + [[{"typesafe_enabled": master, key: switch}]
                         for master in (False, True) for switch in (False, True, None)] + [
                [{"typesafe_enabled": True}]])
        voice = "typesafe_voice_command_names"
        dict.__setitem__(self, "resolve_typesafe_voice_command_names", [[s] for s in (
            {}, {voice: None}, {voice: []}, {voice: ["  ", ""]}, {voice: "not a list"},
            {voice: ["  Note   TAKER ", "Scribe\tBot", 5, None, "ASSISTANT"]},
            {voice: ["İstanbul", "ÉCHO"]}, {voice: ["　wide　space　"]},
        )])
        for name, key in (("resolve_meeting_past_recall_enabled", "meeting_past_recall_enabled"),
                          ("resolve_meeting_context_folder_enabled",
                           "meeting_context_folder_enabled")):
            dict.__setitem__(self, name, [[s] for s in (
                {}, {key: True}, {key: False}, {key: "yes"}, {key: None})])
        dict.__setitem__(self, "resolve_meeting_context_folder_path", FOLDER_PATH_CASES)
        review = ("meeting_insight_review", "meeting_insight_review_consent",
                  "meeting_insight_review_sensitivity")
        dict.__setitem__(self, "resolve_meeting_insight_review", [[s] for s in (
            {}, {review[0]: True},
            {review[0]: True, review[1]: "typesafe-text-v1"},
            {review[0]: True, review[1]: "typesafe-text-v1", "typesafe_enabled": True},
            {review[0]: False, review[1]: "typesafe-text-v1", "typesafe_enabled": True},
            {review[0]: True, review[1]: "typesafe-text-v0", "typesafe_enabled": True},
            {review[1]: "typesafe-text-v1", review[2]: "thorough"},
            {review[2]: "extreme"}, {review[2]: 3},
        )])

    def __iter__(self):
        self._fill()
        return dict.__iter__(self)

    def __len__(self):
        self._fill()
        return dict.__len__(self)

    def get(self, key, default=None):
        self._fill()
        return dict.get(self, key, default)

    def __getitem__(self, key):
        self._fill()
        return dict.__getitem__(self, key)


_CUSTOM = {"id": "custom_abcd1234", "name": "Local", "base_url": "http://127.0.0.1:1234/v1",
           "api_key_env": ""}

CLEANUP_SETTINGS = [
    {}, {"transcript_cleanup_provider": "openrouter"},
    {"transcript_cleanup_provider": "bogus"},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "  gpt-4.1-mini  "},
    {"transcript_cleanup_model": "  my-model  "},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "gpt-4o-mini"},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "  gpt-4o-mini  "},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "ft:gpt-4o-mini:org:x"},
    {"transcript_cleanup_provider": "openrouter", "transcript_cleanup_model": "gpt-4o-mini"},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "   "},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": 5},
    {"transcript_cleanup_provider": 5, "transcript_cleanup_model": "m"},
    {"transcript_cleanup_provider": "claude_code", "transcript_cleanup_model": ""},
    {"transcript_cleanup_provider": "codex", "transcript_cleanup_model": " gpt-5 "},
    {"transcript_cleanup_provider": "opencode_cli"},
    {"transcript_cleanup_provider": "custom_abcd1234", "transcript_cleanup_model": "local-qwen",
     "text_llm_profiles": [_CUSTOM]},
    {"transcript_cleanup_provider": "custom_abcd1234", "transcript_cleanup_model": "local-qwen"},
    {"transcript_cleanup_provider": "lm_studio", "transcript_cleanup_model": "qwen"},
    {"transcript_cleanup_reasoning": "high"}, {"transcript_cleanup_reasoning": "extreme"},
    {"transcript_cleanup_reasoning": "ultra"}, {"transcript_cleanup_reasoning": None},
]

MEETING_SETTINGS = [
    {}, {"meeting_llm_provider": "ollama", "meeting_llm_model": "llama3.2"},
    {"meeting_llm_provider": "ollama", "ollama_base_url": "http://host-a:11434/v1/",
     "meeting_llm_model": " llama3.2 "},
    {"meeting_llm_provider": "openai", "meeting_llm_model": "gpt-4o-mini"},
    {"meeting_llm_provider": "openai", "meeting_llm_model": "o4-mini"},
    {"meeting_llm_provider": "claude_code", "meeting_llm_model": "sonnet"},
    {"meeting_llm_provider": "custom_abcd1234", "meeting_llm_model": "m",
     "text_llm_profiles": [_CUSTOM]},
    {"meeting_llm_provider": "custom_gone", "meeting_llm_model": "m"},
    {"meeting_llm_provider": "groq", "meeting_llm_model": ""},
    {"meeting_llm_provider": "opencode_zen", "meeting_llm_model": "claude-opus-5"},
    {"meeting_llm_provider": "openrouter"},
]


TYPESAFE_FEATURES = ("citations", "semantic_search", "highlights", "question_radar")

# resolve_meeting_context_folder_path: [settings, environment]. The environment pins the four
# variables ntpath.expanduser reads (None removes one); results show the working directory as
# "<cwd>", so the replay does not depend on where it runs. Windows paths: ntpath's rules.
_HOME = {"USERPROFILE": "C:\\Users\\Pat", "HOMEDRIVE": None, "HOMEPATH": None, "USERNAME": "Pat"}
_DRIVE_HOME = {"USERPROFILE": None, "HOMEDRIVE": "D:", "HOMEPATH": "\\Home\\Pat",
               "USERNAME": "Pat"}
_NO_HOME = {"USERPROFILE": None, "HOMEDRIVE": None, "HOMEPATH": None, "USERNAME": None}
_OTHER_USER = {"USERPROFILE": "C:\\Users\\Pat", "HOMEDRIVE": None, "HOMEPATH": None,
               "USERNAME": "Other"}
FOLDER_PATH_CASES = [
    [{}, _HOME], [{"meeting_context_folder_path": "   "}, _HOME],
    [{"meeting_context_folder_path": 5}, _HOME],
    [{"meeting_context_folder_path": "C:\\Notes\\..\\Vault"}, _HOME],
    [{"meeting_context_folder_path": "C:/Notes//Vault/"}, _HOME],
    [{"meeting_context_folder_path": "  C:\\Padded  "}, _HOME],
    [{"meeting_context_folder_path": "C:\\..\\x"}, _HOME],
    [{"meeting_context_folder_path": "D:\\a\\.\\b\\"}, _HOME],
    [{"meeting_context_folder_path": "\\\\server\\share\\x\\..\\y"}, _HOME],
    [{"meeting_context_folder_path": "/rooted/path"}, _HOME],
    [{"meeting_context_folder_path": "~"}, _HOME],
    [{"meeting_context_folder_path": "~/vault"}, _HOME],
    [{"meeting_context_folder_path": "~\\vault\\..\\x"}, _HOME],
    [{"meeting_context_folder_path": "~sam\\notes"}, _HOME],
    [{"meeting_context_folder_path": "~sam\\notes"}, _OTHER_USER],
    [{"meeting_context_folder_path": "~"}, _DRIVE_HOME],
    [{"meeting_context_folder_path": "~/x"}, _NO_HOME],
    [{"meeting_context_folder_path": "notes\\vault"}, _HOME],
    [{"meeting_context_folder_path": "notes/../other"}, _HOME],
]


def _context_folder_path(module, settings, environment):
    import os

    saved = {name: os.environ.get(name) for name in environment}
    try:
        for name, value in environment.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        result = module.resolve_meeting_context_folder_path(settings)
    finally:
        for name, value in saved.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
    return result.replace(os.getcwd(), "<cwd>")


def _meeting_profile(module, settings):
    profile = module.resolve_meeting_llm_profile(settings)
    return {"id": profile.id, "name": profile.name, "kind": profile.kind,
            "base_url": profile.base_url, "api_key_env": profile.api_key_env,
            "builtin": profile.builtin, "protocol": profile.protocol}


def _meeting_endpoint(module, settings):
    # Without learning: the server would be asked over the network.
    from services import text_llm

    original = text_llm.learn_server_models
    text_llm.learn_server_models = lambda *args, **kwargs: None
    try:
        return module.resolve_meeting_llm_endpoint(settings)
    finally:
        text_llm.learn_server_models = original


CASES = _LazyCases()

WRAPPERS = {
    "resolve_meeting_llm_profile": _meeting_profile,
    "resolve_meeting_llm_endpoint": _meeting_endpoint,
    "resolve_meeting_context_folder_path": _context_folder_path,
    "SETTING_DEFAULTS": _setting_defaults,
    "load_model_selection": _load_model_selection,
    "load_hotkey_settings": _load_hotkey_settings,
    "setting_value": _setting_value,
    "resolve_bool_setting": _resolve_bool_setting,
    "resolve_choice_setting": _resolve_choice_setting,
    "api_model_choices": lambda module, day: list(module.api_model_choices(date.fromisoformat(day))),
    "api_model_label": lambda module, model, day: module.api_model_label(
        model, date.fromisoformat(day)),
    "serving_api_model": lambda module, model, day: module.serving_api_model(
        model, date.fromisoformat(day)),
    "resolve_api_transcription_model": lambda module, settings, day: (
        module.resolve_api_transcription_model(settings, date.fromisoformat(day))),
}

PINS = {
    "manager": "SettingsManager on out/parity/settings-cases/settings.json; a null first "
               "argument means no file; load_model_selection's second argument pins "
               "openai_retirement._today",
    "text_models": "default_transcript_cleanup_model reads the scratch settings file, which "
                   "holds no custom endpoint; resolve_meeting_llm_endpoint runs without "
                   "learn_server_models",
    "context_folder_path": "resolve_meeting_context_folder_path's second argument pins "
                           "USERPROFILE, HOMEDRIVE, HOMEPATH and USERNAME (null: unset); the "
                           "working directory reads <cwd>",
    "voice_command_names": "OpenWhisper's defaults name OpenWhisper; FioriVoce's name "
                           "identity() (the replay passes legacy_identity())",
}
