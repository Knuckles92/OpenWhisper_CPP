"""Reference builders of a21's action widgets (fv_ui_automation, M5.10) and the agent mark
(fv_ui_widgets, M5.10). M5.13, M5.17 and M5.21 extend this file with the Actions, Hooks and API &
MCP pages.

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data:

    actions     "Ask Claude" (open_link claude://claude.ai/new?q={text}, shortcut ctrl+alt+c,
                spoken "Claude"), "Home automation" (webhook https://example.com/hooks/home,
                output none, off) and "Log it" (program C:\\Tools\\log.exe, no shortcut)
    agent scan  Claude Code and Codex found, OpenCode missing
    shaping     one cleanup profile "Email" and one transform "Formal"

A widget is `width` wide and as tall as it asks for there: it is shown once at that width and
hidden again to learn its height (_fit), and the C++ tests repeat that step before comparing.

OpenWhisper names itself in two places these widgets show; the builders put FioriVoce's in their
place, as the port draws it: two starting points of the gallery ("…running the OpenWhisper
channel.", "…listening to OpenWhisper's events.") and the new-folder line of the agent fields
("~/OpenWhisper Agent Tasks").

    action_card/enabled           "Ask Claude", 340 px
    action_card/off               "Home automation", switched off, 340 px
    action_gallery/two-columns    every starting point, 680 px
    agent_fields/handoff-claude   Claude Code, Open a session, Auto, 560 px
    agent_fields/ask-codex        Codex, Quick answer, answer to the Scratchpad, 560 px
    shaping_field/profile         the profile "Email" chosen, 480 px
    action_status_line/attention  Route by voice waiting for Fast judgments, with its link, 560 px
    agent_mark/claude-dark        Claude Code's mark at warmth 1.0 (rendered dark only)
    agent_mark/codex-light        Codex's mark at warmth 1.0 (rendered light only)
"""

from dataclasses import replace

PRODUCT = "FioriVoce"
LEGACY = "OpenWhisper"

CARD_WIDTH = 340
GALLERY_WIDTH = 680
FIELDS_WIDTH = 560
SHAPING_WIDTH = 480
STATUS_WIDTH = 560
MEASURE_HEIGHT = 600

SCAN = {"claude_code": True, "codex": True, "opencode_cli": False}
SETTINGS = {
    "transcript_cleanup_profiles": [
        {"id": "email", "name": "Email", "instructions": "Write it as a short email."},
    ],
    "text_transforms": [
        {"id": "formal", "name": "Formal", "instruction": "Make it formal."},
    ],
}
ROUTE_MESSAGE = ("Route by voice needs Fast judgments. Turn them on in Settings → Meeting Mode → "
                 "Fast judgments.")


def _fit(widget, width):
    """`width` wide and as tall as the widget asks for there, learned by showing it once."""
    from PyQt6.QtCore import QCoreApplication

    widget.resize(width, MEASURE_HEIGHT)
    widget.show()
    QCoreApplication.processEvents()
    height = widget.sizeHint().height()
    widget.hide()
    return width, height


def _actions():
    from services.actions.model import Action, ActionKind, Output

    ask = Action("ask-claude", "Ask Claude", ActionKind.OPEN_LINK,
                 {"template": "claude://claude.ai/new?q={text}"}, hotkey="ctrl+alt+c",
                 spoken_names=("Claude",))
    home = Action("home", "Home automation", ActionKind.WEBHOOK,
                  {"url": "https://example.com/hooks/home", "output": Output.NONE},
                  enabled=False)
    log = Action("log-it", "Log it", ActionKind.PROGRAM, {"program": "C:\\Tools\\log.exe"})
    return ask, home, log


def _card(action):
    from ui_qt.widgets.action_list import ActionCard

    card = ActionCard(action)
    return card, _fit(card, CARD_WIDTH), None


def action_card_enabled(variant):
    return _card(_actions()[0])


def action_card_off(variant):
    return _card(_actions()[1])


def action_gallery_two_columns(variant):
    from ui_qt.widgets import action_gallery

    rebranded = tuple(replace(preset, description=preset.description.replace(LEGACY, PRODUCT))
                      for preset in action_gallery.PRESETS)
    saved = action_gallery.PRESETS
    action_gallery.PRESETS = rebranded
    try:
        gallery = action_gallery.PresetGallery()
    finally:
        action_gallery.PRESETS = saved
    return gallery, _fit(gallery, GALLERY_WIDTH), None


def _rebrand_folder_hint(fields):
    """The new-folder line as FioriVoce words it (its agent-task folder is named after it)."""
    row = fields.new_folder_row
    text = row.detail_label.text().replace(LEGACY, PRODUCT)
    row.detail_label.setText(text)
    row.switch.setAccessibleDescription(text)


def _agent_fields(params):
    from ui_qt.widgets.action_fields import AgentFields

    fields = AgentFields()
    fields.set_params(params)
    fields.set_available(SCAN)
    _rebrand_folder_hint(fields)
    return fields, _fit(fields, FIELDS_WIDTH), None


def agent_fields_handoff_claude(variant):
    return _agent_fields({"agent": "claude_code", "mode": "handoff", "surface": "auto"})


def agent_fields_ask_codex(variant):
    return _agent_fields({"agent": "codex", "mode": "ask", "output": "scratchpad"})


def shaping_field_profile(variant):
    from ui_qt.widgets.action_fields import ShapingField

    field = ShapingField()
    field.set_choices(SETTINGS)
    field.set_value("profile:email")
    return field, _fit(field, SHAPING_WIDTH), None


def action_status_line_attention(variant):
    from ui_qt.widgets.action_fields import StatusLine

    line = StatusLine("Fast judgments", "Open Settings → Meeting Mode → Fast judgments")
    line.show_state(StatusLine.ATTENTION, ROUTE_MESSAGE, link=True)
    return line, _fit(line, STATUS_WIDTH), None


def _mark(agent_id):
    from services.installed_agents import AGENT_SPECS
    from ui_qt.widgets.agent_picker import AgentMark

    spec = AGENT_SPECS[agent_id]
    mark = AgentMark(spec.monogram, spec.accent)
    mark.set_warmth(1.0)
    mark.ensurePolished()
    hint = mark.sizeHint()
    return mark, (hint.width(), hint.height()), None


def agent_mark_claude_dark(variant):
    return _mark("claude_code")


def agent_mark_codex_light(variant):
    return _mark("codex")


# ---- M5.13: the Actions page, the action editor and the Agent hotkey card ----
#
# The same fixed actions, the Agent hotkey for Claude Code where a variant has one, and Route by
# voice on with the route shortcut ctrl+alt+v: Fast judgments on with TypeSafe and a key present
# (typesafe.key_present answers True), so its status line is ready.
#
#     settings_actions_page/invite      the three actions, no agent hotkey, and a scan that found
#                                       no agent ("None found — install one, or use another
#                                       action."), page body 900 px
#     settings_actions_page/configured  the agent hotkey for Claude Code and the three actions,
#                                       the usual scan, page body 900 px
#     action_editor/webhook-new         a new action from the Webhook starting point with
#                                       http://example.com/hook (the unencrypted warning), 640 px
#     action_editor/link-preview        "Ask Claude" opened for editing, with its preview line,
#                                       640 px
#     action_agent_card/configured      the card with the agent hotkey for Claude Code, 640 px
#
# The page body is OpenWhisper's own, built by a real SettingsDialog over a throwaway settings
# file in the isolated data folder (as builders/a31.py builds its page), then taken out of the
# dialog at PAGE_WIDTH with no object name; label texts that name OpenWhisper name FioriVoce.

PAGE_WIDTH = 900
EDITOR_WIDTH = 640
AGENT_CARD_WIDTH = 640
NONE_FOUND = {"claude_code": False, "codex": False, "opencode_cli": False}
ROUTE_SETTINGS = {
    "voice_routing_enabled": True,
    "typesafe_enabled": True,
    "hotkeys": {"route_record": "ctrl+alt+v"},
}

_KEEP = []
_WIDGETS = []
_APP = []


def _release():
    from PyQt6 import sip

    for widget in reversed(_WIDGETS):
        if not sip.isdeleted(widget):
            sip.delete(widget)
    _WIDGETS.clear()
    for stack in reversed(_KEEP):
        stack.close()
    _KEEP.clear()
    _APP.clear()


def _keep(widget):
    import atexit

    from PyQt6.QtWidgets import QApplication

    if not _APP:
        _APP.append(QApplication.instance())
        atexit.register(_release)
    _WIDGETS.append(widget)
    return widget


def _agent_action():
    from services.actions.model import Action, ActionKind

    return Action("agent", "Agent hotkey", ActionKind.AGENT,
                  {"agent": "claude_code", "mode": "handoff", "surface": "auto"},
                  hotkey="ctrl+alt+shift+space", spoken_names=("Claude Code",),
                  description="Coding tasks for Claude Code")


def _settings(with_agent):
    actions = ([_agent_action()] if with_agent else []) + list(_actions())
    return {**SETTINGS, **ROUTE_SETTINGS, "actions": [action.to_dict() for action in actions]}


def _fiorivoce(widget):
    """Label texts as FioriVoce words them (its product name where OpenWhisper's is)."""
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.wrapped_label import WrappedLabel

    for label in widget.findChildren(QLabel):
        if LEGACY in label.text():
            text = label.text().replace(LEGACY, PRODUCT)
            if isinstance(label, WrappedLabel):
                label.setText(text)
            else:
                QLabel.setText(label, text)


def _patches(stack, scan):
    from unittest.mock import patch

    from services import typesafe
    from ui_qt.widgets import action_fields

    stack.enter_context(patch("services.actions.agents.available", lambda: dict(scan)))
    stack.enter_context(patch.object(typesafe, "key_present", lambda *_a, **_k: True))
    # The agent fields write their new-folder line again whenever they sync (the page's refresh
    # on show among them), so FioriVoce's wording goes in after every sync.
    sync = action_fields.AgentFields._sync

    def _sync(fields):
        sync(fields)
        _rebrand_folder_hint(fields)

    stack.enter_context(patch.object(action_fields.AgentFields, "_sync", _sync))


def _settled_height(widget, width):
    """The height the widget's layout needs at `width`, measured shown."""
    from PyQt6.QtCore import QCoreApplication

    widget.resize(width, 4000)
    widget.show()
    for _ in range(3):
        QCoreApplication.processEvents()
    height = widget.layout().minimumSize().height()
    widget.hide()
    return height


def _actions_page(values, scan):
    import os
    from contextlib import ExitStack
    from unittest.mock import patch

    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module
    from ui_qt.dialogs.settings_destinations import ACTIONS

    for name in [name for name in os.environ if name.upper().endswith("_API_KEY")]:
        del os.environ[name]
    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a21")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"settings{len(_KEEP)}.json"))
    store.save_all_settings(dict(values))
    stack = ExitStack()
    for module in (settings_dialog_module, models_module, downloads_module):
        stack.enter_context(patch.object(module, "settings_manager", store))
    stack.enter_context(patch.object(settings_dialog_module.history_manager, "set_retention"))
    for module in (models_module, downloads_module):
        stack.enter_context(patch.object(module, "scan_cached_models", return_value={}))
    stack.enter_context(patch("services.local_asr.cache.inventory", return_value={}))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    _patches(stack, scan)
    _KEEP.append(stack)
    dialog = _keep(settings_dialog_module.SettingsDialog(background_cache_scan=False))
    dialog.ensure_page(ACTIONS)
    dialog._page_scrolls[ACTIONS].takeWidget()
    page = dialog._pages[ACTIONS]
    page.setParent(None)
    page.setObjectName("")
    _keep(page)
    _fiorivoce(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def settings_actions_page_invite(variant):
    return _actions_page(_settings(with_agent=False), NONE_FOUND)


def settings_actions_page_configured(variant):
    return _actions_page(_settings(with_agent=True), SCAN)


class _Manager:
    """The settings manager the editor and the card read and write, over fixed settings."""

    def __init__(self, settings):
        self.settings = dict(settings)

    def load_all_settings(self):
        return dict(self.settings)

    def mutate_settings(self, mutate):
        settings = dict(self.settings)
        result = mutate(settings)
        self.settings = settings
        return result


def _try_action(_action, _sample, _done):
    pass


def _editor(action, *, is_new, note=""):
    from contextlib import ExitStack

    from ui_qt.widgets.action_editor import ActionEditor

    stack = ExitStack()
    _patches(stack, SCAN)
    _KEEP.append(stack)
    editor = _keep(ActionEditor(manager=_Manager(_settings(with_agent=False)),
                                try_action=_try_action))
    editor.open(action, is_new=is_new, note=note)
    _fiorivoce(editor)
    return editor


def action_editor_webhook_new(variant):
    from services.actions.model import PRESETS, Action

    preset = next(preset for preset in PRESETS if preset.id == "webhook")
    action = Action("new-webhook", preset.name, preset.kind, dict(preset.params),
                    shaping=preset.shaping)
    editor = _editor(action, is_new=True, note=preset.note)
    editor.url_edit.setText("http://example.com/hook")
    editor.url_edit.setCursorPosition(0)
    return editor, _fit(editor, EDITOR_WIDTH), None


def action_editor_link_preview(variant):
    editor = _editor(_actions()[0], is_new=False)
    return editor, _fit(editor, EDITOR_WIDTH), None


def action_agent_card_configured(variant):
    from contextlib import ExitStack

    from ui_qt.widgets.action_agent_card import AgentHotkeyCard

    stack = ExitStack()
    _patches(stack, SCAN)
    _KEEP.append(stack)
    settings = _settings(with_agent=True)
    card = _keep(AgentHotkeyCard(manager=_Manager(settings), try_action=_try_action))
    card.load(settings)
    _fiorivoce(card)
    return card, _fit(card, AGENT_CARD_WIDTH), None


# ---- M5.17: the Hooks page and two editors of its sections ----
#
# One webhook "Home automation" (https://example.com:8443/hooks; Finished dictations and Actions
# that ran; last delivery 200 · 120 ms), one program "Log my dictations" (C:\Tools\log.exe, off), one
# text filter "Fix product names" (http://127.0.0.1:8080/filter, before AI cleanup, 1500 ms) and one
# lease ending in 42 s. The clock is pinned to 2026-03-15 15:00 UTC (the zone too), so the
# deliveries read 14:58:20 and 14:41:02.
#
#     settings_hooks_page/empty     no hooks and no lease, page body 900 px
#     settings_hooks_page/filled    the three hooks and the lease, page body 900 px
#     hooks_webhook_editor/saved    the webhook opened for editing, with two recent deliveries,
#                                   640 px
#     hooks_filter_editor/program   a program filter "Fix product names" (C:\Tools\fix.exe,
#                                   --strict, before AI cleanup, 1500 ms) opened for editing,
#                                   640 px
#
# The page body is OpenWhisper's own, built by a real SettingsDialog over a throwaway settings file
# (as the Actions page's is) and taken out of the dialog; label texts that name OpenWhisper name
# FioriVoce.

HOOKS_EDITOR_WIDTH = 640
HOOKS_NOW = 1773586800.0  # 2026-03-15T15:00:00Z
HOOKS_WEBHOOK = {"id": "hook01", "name": "Home automation", "url": "https://example.com:8443/hooks",
                 "events": ["transcription.completed", "action.ran"], "enabled": True}
HOOKS_PROGRAM = {"id": "prog01", "name": "Log my dictations", "program": "C:\\Tools\\log.exe",
                 "args": [], "events": ["transcription.completed"], "enabled": False,
                 "timeout_s": 30}
HOOKS_FILTER = {"id": "filt01", "name": "Fix product names", "kind": "http",
                "stage": "before_cleanup", "url": "http://127.0.0.1:8080/filter",
                "timeout_ms": 1500, "enabled": True}
HOOKS_PROGRAM_FILTER = {"id": "filt02", "name": "Fix product names", "kind": "program",
                        "stage": "before_cleanup", "program": "C:\\Tools\\fix.exe",
                        "args": ["--strict"], "timeout_ms": 1500, "enabled": True}


def _hooks_patches(stack, *, filled):
    """The engines' memory and the clock, as the sections read them."""
    from datetime import datetime, timezone
    from types import SimpleNamespace
    from unittest.mock import patch

    from services.automation import filters, programs, webhooks
    from ui_qt.widgets import hooks_panel

    class _UtcDatetime(datetime):
        def astimezone(self, tz=None):
            return super().astimezone(tz or timezone.utc)

    deliveries = [
        webhooks.DeliveryRecord("evt_2", "transcription.completed", "2026-03-15T14:58:20.000Z",
                                True, 200, 120),
        webhooks.DeliveryRecord("evt_1", "action.ran", "2026-03-15T14:41:02.000Z", False, 502,
                                1430, "Bad gateway", 2),
    ] if filled else []
    leases = [filters.Lease("lease01", "http://127.0.0.1:7070/f", "after_cleanup", 1500,
                            ("dictation",), HOOKS_NOW + 42)] if filled else []
    stack.enter_context(patch.object(hooks_panel, "datetime", _UtcDatetime))
    stack.enter_context(patch.object(hooks_panel, "time", SimpleNamespace(time=lambda: HOOKS_NOW)))
    stack.enter_context(patch.object(webhooks, "deliveries", lambda _id: list(deliveries)))
    stack.enter_context(patch.object(programs, "runs", lambda _id: []))
    stack.enter_context(patch.object(filters, "leases", lambda: list(leases)))


def _hooks_page(values, *, filled):
    import os
    from contextlib import ExitStack
    from unittest.mock import patch

    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module
    from ui_qt.dialogs.settings_destinations import HOOKS

    for name in [name for name in os.environ if name.upper().endswith("_API_KEY")]:
        del os.environ[name]
    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a21")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"settings{len(_KEEP)}.json"))
    store.save_all_settings(dict(values))
    stack = ExitStack()
    for module in (settings_dialog_module, models_module, downloads_module):
        stack.enter_context(patch.object(module, "settings_manager", store))
    stack.enter_context(patch.object(settings_dialog_module.history_manager, "set_retention"))
    for module in (models_module, downloads_module):
        stack.enter_context(patch.object(module, "scan_cached_models", return_value={}))
    stack.enter_context(patch("services.local_asr.cache.inventory", return_value={}))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    _hooks_patches(stack, filled=filled)
    _KEEP.append(stack)
    dialog = _keep(settings_dialog_module.SettingsDialog(background_cache_scan=False))
    dialog.ensure_page(HOOKS)
    dialog._page_scrolls[HOOKS].takeWidget()
    page = dialog._pages[HOOKS]
    page.setParent(None)
    page.setObjectName("")
    _keep(page)
    _fiorivoce(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def settings_hooks_page_empty(variant):
    return _hooks_page({}, filled=False)


def settings_hooks_page_filled(variant):
    return _hooks_page({"automation_webhooks": [HOOKS_WEBHOOK],
                        "automation_programs": [HOOKS_PROGRAM],
                        "automation_filters": [HOOKS_FILTER]}, filled=True)


def _hooks_editor(make, item):
    from contextlib import ExitStack

    stack = ExitStack()
    _hooks_patches(stack, filled=True)
    _KEEP.append(stack)
    editor = _keep(make())
    editor.show_item(item, saved=True)
    _fiorivoce(editor)
    return editor, _fit(editor, HOOKS_EDITOR_WIDTH), None


def hooks_webhook_editor_saved(variant):
    from services.automation import models
    from ui_qt.widgets import hooks_panel

    hook = models.load_webhooks({"automation_webhooks": [HOOKS_WEBHOOK]})[0]
    return _hooks_editor(hooks_panel.WebhookEditor, hook)


def hooks_filter_editor_program(variant):
    from services.automation import models
    from ui_qt.widgets import hooks_panel

    text_filter = models.load_filters({"automation_filters": [HOOKS_PROGRAM_FILTER]})[0]
    return _hooks_editor(hooks_panel.FilterEditor, text_filter)


# ---- M5.21: the API & MCP page's local part and its live section ----
#
# A fake MCP listener on port 8767 whose access token is
# fv-test-token-0123456789abcdefghijklmnopqrstu while it runs, two plain event-stream listeners and
# one Claude Code channel session on the event bus, one MCP Events subscription, and "Run actions"
# allowed. The page and the section are OpenWhisper's own widgets, built over a throwaway settings
# file and taken at MCP_WIDTH (two columns); label texts that name OpenWhisper name FioriVoce.
#
#     settings_mcp_page/stopped            MCP off, nothing allowed, 900 px
#     settings_mcp_page/running            MCP running with the Claude Code setup chosen and its
#                                          preview open (the masked token), three permissions on,
#                                          900 px
#     live_automation_section/running      the Live API, MCP Events and channel cards while MCP
#                                          runs, 900 px
#
# The preview shows the Claude Code command rather than the setup prompt, because the prompt names
# the computer it is made on.

MCP_WIDTH = 900
MCP_TOKEN = "fv-test-token-0123456789abcdefghijklmnopqrstu"
MCP_SAVED = {"mcp_enabled": True, "mcp_retitle_transcriptions": True, "mcp_settings_access": True,
             "mcp_writable_settings": {"auto_paste": True}}
MCP_LIVE_SAVED = {"api_run_actions": True, "mcp_events_enabled": True}
_MCP_STORES = []


class _McpServer:
    """The listener the page reads: running on 8767, or off."""

    def __init__(self, state):
        from services.agent_mcp.runtime import ServerStatus

        self.current = ServerStatus(
            state, "Ready for agent connections." if state == "running" else "MCP is off.", 8767)

    def status(self):
        return self.current

    def token(self):
        return MCP_TOKEN if self.current.state == "running" else ""


def _mcp_store(values):
    import os

    from services.settings import SettingsManager

    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a21")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"mcp{len(_MCP_STORES)}.json"))
    store.save_all_settings(dict(values))
    _MCP_STORES.append(store)
    return store


def _mcp_patches(stack):
    """The page's refresh names FioriVoce after every redraw, MCP Events has one subscription, and
    the event bus is the builder's own, with the listeners the section counts."""
    from unittest.mock import patch

    from services.agent_mcp import events_ext
    from services.automation import events
    from ui_qt.dialogs import settings_mcp

    refresh = settings_mcp.McpSettingsPage.refresh

    def _refresh(page):
        refresh(page)
        _fiorivoce(page)
        # The preview's server name is the app's, in lower case.
        text = page.setup_text.toPlainText()
        if LEGACY.lower() in text:
            page.setup_text.setPlainText(text.replace(LEGACY.lower(), PRODUCT.lower()))

    stack.enter_context(patch.object(settings_mcp.McpSettingsPage, "refresh", _refresh))
    stack.enter_context(patch.object(events_ext, "subscription_count", lambda: 1))
    bus = events.EventBus()
    previous = events.set_bus(bus)
    for label in ("sse", "sse", "claude-channel"):
        bus.subscribe(lambda event: None, label=label)

    def restore():
        events.set_bus(previous)
        bus.close()

    stack.callback(restore)


def _mcp_stack():
    from contextlib import ExitStack

    stack = ExitStack()
    _mcp_patches(stack)
    _KEEP.append(stack)
    return stack


def settings_mcp_page_stopped(variant):
    from ui_qt.dialogs import settings_mcp

    _mcp_stack()
    page = _keep(settings_mcp.McpSettingsPage(_mcp_store({}), server=_McpServer("stopped")))
    _fiorivoce(page)
    return page, (MCP_WIDTH, _settled_height(page, MCP_WIDTH)), None


def settings_mcp_page_running(variant):
    from ui_qt.dialogs import settings_mcp

    _mcp_stack()
    page = _keep(settings_mcp.McpSettingsPage(_mcp_store(MCP_SAVED), server=_McpServer("running")))
    page.setup_kind.setCurrentIndex(1)
    page.preview.toggle.setChecked(True)
    _fiorivoce(page)
    return page, (MCP_WIDTH, _settled_height(page, MCP_WIDTH)), None


def live_automation_section_running(variant):
    from types import SimpleNamespace

    from ui_qt.dialogs import settings_mcp
    from ui_qt.widgets.wrapped_label import WrappedLabel

    _mcp_stack()
    store = _mcp_store({**MCP_SAVED, **MCP_LIVE_SAVED})
    section = _keep(settings_mcp.LiveAutomationSection(
        store, SimpleNamespace(server=_McpServer("running"))))
    section.refresh_live()
    _fiorivoce(section)
    # The command names the app in lower case; WrappedLabel keeps the text as set.
    command = section.channel_command
    WrappedLabel.setText(command, command.text().replace(LEGACY.lower(), PRODUCT.lower()))
    return section, (MCP_WIDTH, _settled_height(section, MCP_WIDTH)), None


BUILDERS = {
    "settings_mcp_page/stopped": settings_mcp_page_stopped,
    "settings_mcp_page/running": settings_mcp_page_running,
    "live_automation_section/running": live_automation_section_running,
    "settings_hooks_page/empty": settings_hooks_page_empty,
    "settings_hooks_page/filled": settings_hooks_page_filled,
    "hooks_webhook_editor/saved": hooks_webhook_editor_saved,
    "hooks_filter_editor/program": hooks_filter_editor_program,
    "settings_actions_page/invite": settings_actions_page_invite,
    "settings_actions_page/configured": settings_actions_page_configured,
    "action_editor/webhook-new": action_editor_webhook_new,
    "action_editor/link-preview": action_editor_link_preview,
    "action_agent_card/configured": action_agent_card_configured,
    "action_card/enabled": action_card_enabled,
    "action_card/off": action_card_off,
    "action_gallery/two-columns": action_gallery_two_columns,
    "agent_fields/handoff-claude": agent_fields_handoff_claude,
    "agent_fields/ask-codex": agent_fields_ask_codex,
    "shaping_field/profile": shaping_field_profile,
    "action_status_line/attention": action_status_line_attention,
    "agent_mark/claude-dark": agent_mark_claude_dark,
    "agent_mark/codex-light": agent_mark_codex_light,
}
