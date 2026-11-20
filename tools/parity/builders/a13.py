"""Reference builders of a13's Personalize widgets and dialogs (fv_ui_personalize, M4.3), its
text-model widgets (fv_ui_widgets, M4.6): the endpoint and model picker and the endpoint dialog,
the Dictionary and Snippets pages (fv_ui_personalize, M4.7) and the Apps & styles, Commands and
Profiles pages (fv_ui_personalize, M4.8; see their sections below).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.

The rule activity strip is built the way the learned-rule composer opens it, 400x40, on clocks the
builder sets: the module's QElapsedTimer is swapped for a stand-in whose time is whatever the
builder writes, so the pulse and the orbit sit at phase 0 and the trace at a step boundary. The
trace holds 30 bars of a scripted speech-like level stream (one 60 ms step each, eased once per
step, as the 16 ms timer eases them while the microphone is live). The strip's timers and its
height animation are stopped: render_reference.py runs no event loop, and a tick that landed in
its one processEvents would move the picture.

    listening     "USB Microphone", the clock at "0:12 / 1:00"
    polishing     the same bars held after capture, "OpenRouter · openrouter/free", the sweep at
                  phase 0

The dialogs are shown once and hidden again to learn the size they open at, which is the size
recorded. The review dialog is the dictated confirm ("Always spell my name \"Alex Rivera\"." from
"always spell my name alex rivera") and the plain edit of a saved rule; the prompt editor holds
the Medium preset.

The picker is 520 px wide at the height its layout asks for at that width. It reads no credential
store here: credential_status is replaced by the answer a host that found OpenRouter's key would
hand it (the C++ widget is given the same line through setCredentialStatus, because its store is
blocking and never runs on the GUI thread). Catalogs and the "<n> models available" line are what
the Settings page puts in after a load.

    text_model_picker/openrouter     OpenRouter shown, key found, "openrouter/free" and
                                     "anthropic/claude-sonnet-4.5" listed, sort "Most popular",
                                     the assignment in use openrouter/free (Active badge)
    text_model_picker/local-running  Ollama answering at its default address ("Ollama · Running"),
                                     model "llama3.2": Edit enabled, Delete disabled
    text_endpoint_dialog/add         the empty Add dialog
    text_endpoint_dialog/local-preset  "LM Studio server": address and key variable only
"""

from unittest.mock import patch

STRIP_SIZE = (400, 40)
UNLIMITED_HEIGHT = 16777215

RULE = 'Always spell my name "Alex Rivera".'
ORIGINAL = "always spell my name alex rivera"

# One level per 60 ms step: quiet, a word, a pause, a louder word.
LEVELS = [
    0.0, 0.004, 0.02, 0.08, 0.15, 0.06, 0.01, 0.0, 0.03, 0.12, 0.2, 0.09, 0.02, 0.005, 0.0,
    0.05, 0.1, 0.18, 0.07, 0.015, 0.0, 0.02, 0.06, 0.14, 0.11, 0.04, 0.008, 0.0, 0.03, 0.09,
]
#: 9 pulse periods: the live dot is at phase 0 and "0:12" is on the clock.
LISTENED_MS = 12600

PICKER_WIDTH = 520
OPENROUTER_KEY_FOUND = (True, "OpenRouter API key found")


class _Clock:
    """A QElapsedTimer whose time the builder sets; start() and restart() zero it."""

    def __init__(self):
        self.ms = 0
        self.valid = False

    def start(self):
        self.ms = 0
        self.valid = True

    restart = start

    def elapsed(self):
        return self.ms

    def isValid(self):
        return self.valid


def _strip(state):
    from ui_qt.widgets import rule_activity

    with patch.object(rule_activity, "QElapsedTimer", _Clock):
        strip = rule_activity.RuleActivityStrip(60)
    strip.show_listening("USB Microphone")
    for index, level in enumerate(LEVELS):
        strip.push_level(level)
        strip.trace._clock.ms = 60 * (index + 1)
        strip.trace.advance()
    if state == rule_activity.LISTENING:
        strip._listen_clock.ms = LISTENED_MS
        strip._update_clock()
    else:
        strip.show_polishing("OpenRouter · openrouter/free")
    strip._timer.stop()
    strip._reveal_timer.stop()
    strip._height_anim.stop()
    strip.setMaximumHeight(UNLIMITED_HEIGHT)
    return strip, STRIP_SIZE, None


def rule_activity_strip_listening(variant):
    from ui_qt.widgets import rule_activity

    return _strip(rule_activity.LISTENING)


def rule_activity_strip_polishing(variant):
    from ui_qt.widgets import rule_activity

    return _strip(rule_activity.POLISHING)


def _credential_status(profile):
    """What credential_status answers with OpenRouter's key found and no other key set."""
    if not profile.api_key_env:
        return True, "No API key required"
    if profile.id == "openrouter":
        return OPENROUTER_KEY_FOUND
    return False, f"Requires {profile.api_key_env} - add it in Settings"


def _picker():
    from services.text_llm import list_profiles
    from ui_qt.widgets import text_model_picker

    text_model_picker.credential_status = _credential_status
    picker = text_model_picker.TextModelPicker()
    picker.set_profiles(list_profiles({}))
    return picker


def _fit(widget, width):
    """The widget at `width`, as tall as its layout asks for once it is styled."""
    widget.ensurePolished()
    layout = widget.layout()
    if layout.hasHeightForWidth():
        return width, layout.totalHeightForWidth(width)
    return width, layout.sizeHint().height()


def _natural_size(dialog):
    """The size a dialog opens at: shown once, then hidden again."""
    from PyQt6.QtCore import QCoreApplication

    dialog.show()
    QCoreApplication.processEvents()
    size = (dialog.width(), dialog.height())
    dialog.hide()
    return size


def cleanup_rule_dialog_confirm(variant):
    from ui_qt.dialogs.cleanup_rule_dialog import CleanupRuleDialog

    dialog = CleanupRuleDialog(RULE, original=ORIGINAL, dictated=True)
    return dialog, _natural_size(dialog), None


def cleanup_rule_dialog_edit(variant):
    from ui_qt.dialogs.cleanup_rule_dialog import CleanupRuleDialog

    dialog = CleanupRuleDialog(RULE)
    return dialog, _natural_size(dialog), None


def cleanup_prompt_dialog_default(variant):
    from services import cleanup_prompts
    from ui_qt.dialogs.cleanup_prompt_dialog import CleanupPromptDialog

    dialog = CleanupPromptDialog(cleanup_prompts.preset(cleanup_prompts.CleanupLevel.MEDIUM))
    return dialog, _natural_size(dialog), None


def text_model_picker_openrouter(variant):
    picker = _picker()
    picker.set_provider("openrouter")
    picker.set_sort("most-popular")
    picker.set_active_selection("openrouter", "openrouter/free")
    picker.set_models(["openrouter/free", "anthropic/claude-sonnet-4.5"])
    picker.status_label.setText("2 models available")
    return picker, _fit(picker, PICKER_WIDTH), None


def text_model_picker_local_running(variant):
    picker = _picker()
    picker.set_running_servers({"http://localhost:11434/v1": "ollama"})
    picker.set_provider("ollama", "llama3.2")
    picker.set_models(["llama3.2"])
    picker.status_label.setText("1 models available")
    return picker, _fit(picker, PICKER_WIDTH), None


def text_endpoint_dialog_add(variant):
    from ui_qt.dialogs.text_endpoint_dialog import TextEndpointDialog

    dialog = TextEndpointDialog()
    return dialog, _natural_size(dialog), None


def text_endpoint_dialog_local_preset(variant):
    from services.text_llm import get_profile
    from ui_qt.dialogs.text_endpoint_dialog import TextEndpointDialog

    dialog = TextEndpointDialog(get_profile("lm_studio", {}))
    return dialog, _natural_size(dialog), None


# --- The Dictionary and Snippets pages (fv_ui_personalize, M4.7) ---
#
# Each page body is OpenWhisper's own, built by a real SettingsDialog over a throwaway settings
# file in the isolated data folder and taken out of the dialog at a11's PAGE_WIDTH, the way
# builders/a11.py builds the App pages (its _app_page is reused here). OpenWhisper_CPP's page class is
# the body the Settings shell hosts. Fixed settings: Parakeet, AI cleanup on at Medium; the
# dictionary "Ksenia" (starred, sounds like "Ksenya, Xenia"), "Kubernetes" (learned and new) and
# "Alex Rivera" (learned, kept as said); the snippets "my calendar link" and "my sign-off"
# (formatted). No live backend is set.
#
#     settings_dictionary/words        the three words
#     settings_dictionary/empty-gated  no words, AI cleanup off: the "AI cleanup is off" gate
#     settings_snippets/editor         both snippets, "my sign-off" selected
#     settings_snippets/empty          no snippets: the starter buttons

DICTIONARY_WORDS = [
    {"id": "ksenia", "term": "Ksenia", "starred": True, "heard": ["Ksenya", "Xenia"],
     "learned": False, "new": False},
    {"id": "kubernetes", "term": "Kubernetes", "starred": False, "heard": [], "learned": True,
     "new": True},
    {"id": "alex", "term": "Alex Rivera", "starred": False, "heard": [], "learned": True,
     "new": False, "protected": True},
]
SNIPPET_LIBRARY = [
    {"id": "cal", "trigger": "my calendar link", "text": "https://cal.example.com/dana",
     "formatted": False},
    {"id": "sig", "trigger": "my sign-off", "text": "**Dana Lee**\nDesigner", "formatted": True},
]
PARAKEET = {"selected_model": "parakeet"}
CLEANUP_ON = {**PARAKEET, "transcript_cleanup_enabled": True, "transcript_cleanup_level": "medium"}

_A11 = []


def _a11():
    """builders/a11.py, for its App-page body builder."""
    import importlib.util
    from pathlib import Path

    if not _A11:
        path = Path(__file__).resolve().with_name("a11.py")
        spec = importlib.util.spec_from_file_location("builders_a11_for_a13", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _A11.append(module)
    return _A11[0]


def settings_dictionary_words(variant):
    from ui_qt.dialogs.settings_destinations import DICTIONARY

    return _a11()._app_page(DICTIONARY, {**CLEANUP_ON, "dictation_dictionary": DICTIONARY_WORDS})


def settings_dictionary_empty_gated(variant):
    from ui_qt.dialogs.settings_destinations import DICTIONARY

    return _a11()._app_page(DICTIONARY, dict(PARAKEET))


def settings_snippets_editor(variant):
    from ui_qt.dialogs.settings_destinations import SNIPPETS

    def select_sign_off(dialog):
        dialog.snippets_panel.snippet_list.setCurrentRow(1)

    return _a11()._app_page(SNIPPETS, {**CLEANUP_ON, "dictation_snippets": SNIPPET_LIBRARY},
                            select_sign_off)


def settings_snippets_empty(variant):
    from ui_qt.dialogs.settings_destinations import SNIPPETS

    return _a11()._app_page(SNIPPETS, dict(CLEANUP_ON))


# --- The Apps & styles, Commands and Profiles pages (fv_ui_personalize, M4.8) ---
#
# Built the same way, over these settings, with what the pages ask of the computer pinned: no app
# seen this session (recent_apps), text near the cursor readable (text_reading_supported, true on
# Windows), and a cleanup model ready (provider_readiness), so the Commands page shows no setup
# notice. OpenWhisper_CPP's pages are given the same answers.
#
#     settings_styles/default            awareness on, read text off, cleanup on, Work messages
#                                        Casual, "Notion" moved to Work messages, "1Password"
#                                        never read from
#     settings_commands/transforms       Command Mode shortcut ctrl+alt+space, the four starter
#                                        transforms, "Polish" given ctrl+alt+p and selected
#     settings_cleanup_profiles/starters the two starter profiles, "Support ticket" selected

STYLES_SETTINGS = {
    "transcript_cleanup_enabled": True,
    "app_context_enabled": True,
    "app_context_read_text": False,
    "app_style_tones": {"email": "formal", "work": "casual", "personal": "formal",
                        "other": "formal"},
    "app_style_overrides": [{"match": "Notion", "category": "work"}],
    "app_context_excluded_apps": ["1Password"],
}


def _starter_transforms_with_polish_shortcut():
    from services.text_transforms import STARTER_TRANSFORMS

    rows = [transform.to_dict() for transform in STARTER_TRANSFORMS]
    rows[0]["hotkey"] = "ctrl+alt+p"
    return rows


def _personalize_page(key, values, prepare=None):
    from contextlib import ExitStack

    from services import text_rewrite
    from ui_qt.dialogs import settings_styles

    stack = ExitStack()
    stack.enter_context(patch.object(settings_styles, "recent_apps", lambda: ()))
    stack.enter_context(patch.object(settings_styles, "text_reading_supported", lambda: True))
    stack.enter_context(patch.object(text_rewrite, "provider_readiness", lambda _settings: True))
    _a11()._KEEP.append(stack)
    return _a11()._app_page(key, values, prepare)


def settings_styles_default(variant):
    from ui_qt.dialogs.settings_destinations import STYLES

    return _personalize_page(STYLES, dict(STYLES_SETTINGS))


def settings_commands_transforms(variant):
    from ui_qt.dialogs.settings_destinations import COMMANDS

    def select_polish(dialog):
        dialog.transforms_panel.transform_list.setCurrentRow(0)

    values = {"hotkeys": {"command_mode": "ctrl+alt+space"},
              "text_transforms": _starter_transforms_with_polish_shortcut()}
    return _personalize_page(COMMANDS, values, select_polish)


def settings_cleanup_profiles_starters(variant):
    from ui_qt.dialogs.settings_destinations import CLEANUP_PROFILES

    def select_ticket(dialog):
        dialog.cleanup_profiles_panel.profile_list.setCurrentRow(0)

    return _personalize_page(CLEANUP_PROFILES, {}, select_ticket)


# --- The Learned rules page (fv_ui_personalize, M4.12) ---
#
# The page body as builders/a11.py's _app_page builds it, AI cleanup on, three saved rules and the
# composer idle (its activity strip closed).
#
#     settings_cleanup_rules/library

RULES = [
    'Always spell my name "Alex Rivera".',
    "Use British spelling.",
    "Keep acronyms uppercase.",
]


def settings_cleanup_rules_library(variant):
    from ui_qt.dialogs.settings_destinations import CLEANUP_RULES

    return _a11()._app_page(CLEANUP_RULES, {**CLEANUP_ON, "transcript_cleanup_rules": RULES})


BUILDERS = {
    "settings_cleanup_rules/library": settings_cleanup_rules_library,
    "settings_styles/default": settings_styles_default,
    "settings_commands/transforms": settings_commands_transforms,
    "settings_cleanup_profiles/starters": settings_cleanup_profiles_starters,
    "settings_dictionary/words": settings_dictionary_words,
    "settings_dictionary/empty-gated": settings_dictionary_empty_gated,
    "settings_snippets/editor": settings_snippets_editor,
    "settings_snippets/empty": settings_snippets_empty,
    "rule_activity_strip/listening": rule_activity_strip_listening,
    "rule_activity_strip/polishing": rule_activity_strip_polishing,
    "cleanup_rule_dialog/confirm": cleanup_rule_dialog_confirm,
    "cleanup_rule_dialog/edit": cleanup_rule_dialog_edit,
    "cleanup_prompt_dialog/default": cleanup_prompt_dialog_default,
    "text_model_picker/openrouter": text_model_picker_openrouter,
    "text_model_picker/local-running": text_model_picker_local_running,
    "text_endpoint_dialog/add": text_endpoint_dialog_add,
    "text_endpoint_dialog/local-preset": text_endpoint_dialog_local_preset,
}
