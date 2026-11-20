"""Reference builders of the agent picker (fv_ui_widgets, M5.18).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data. The
picker's blocking seams are replaced while it is built, so no agent is looked for or started:

    scan    Claude Code 2.1.3 signed in as "Claude Max"; Codex 0.100.0, too old ("Update needed",
            with OpenWhisper_CPP's wording of the problem, as the port's scanner writes it); OpenCode
            not installed. The scan is the cached one, so every tile shows its result at once,
            with no shimmer or arrival to wait for.
    models  Claude Code lists "Claude Code default", Haiku and Sonnet.

The choice is made before the picker is first shown, so the chosen tile is raised without its
lift animation. A widget is `WIDTH` wide and as tall as it asks for there: it is shown once at
that width and hidden again to learn its height (_fit), and the C++ tests repeat that step. At
130 % the cards no longer fit four across, so that variant shows the two-by-two layout.

    agent_picker/insights-builtin   purpose "AI insights", the built-in tile (Pi) chosen
    agent_picker/cleanup-claude     purpose "AI cleanup", Claude Code chosen, its model row
"""

PRODUCT = "OpenWhisper_CPP"

WIDTH = 760
MEASURE_HEIGHT = 900


def _scan():
    from services.installed_agents import InstalledAgent

    claude = InstalledAgent("claude_code", "C:/bin/claude.exe", "2.1.3", "Claude Max", True)
    codex = InstalledAgent(
        "codex", "C:/bin/codex.exe", "0.100.0",
        problem=(f"{PRODUCT} needs Codex 0.120.0 or newer. Run `npm install -g @openai/codex` "
                 "or update the Codex app."))
    return {"claude_code": claude, "codex": codex, "opencode_cli": None}


def _models(agent):
    from services.installed_agents import AgentModel

    if agent.id != "claude_code":
        return []
    return [AgentModel("", "Claude Code default"), AgentModel("haiku", "Haiku"),
            AgentModel("sonnet", "Sonnet")]


def _fit(widget, width):
    """`width` wide and as tall as the widget asks for there, learned by showing it once."""
    from PyQt6.QtCore import QCoreApplication

    widget.resize(width, MEASURE_HEIGHT)
    widget.show()
    QCoreApplication.processEvents()
    height = widget.sizeHint().height()
    widget.hide()
    return width, height


def _picker(purpose, choice):
    from ui_qt.widgets import agent_picker

    def refuse(*_args, **_kwargs):
        raise RuntimeError("agents are not scanned while references are rendered")

    saved = (agent_picker.scan_installed_agents, agent_picker.cached_agents,
             agent_picker.list_agent_models)
    scan = _scan()
    agent_picker.scan_installed_agents = refuse
    agent_picker.cached_agents = lambda: dict(scan)
    agent_picker.list_agent_models = _models
    try:
        picker = agent_picker.AgentPicker(purpose=purpose)
        picker.set_choice(choice)
        size = _fit(picker, WIDTH)
    finally:
        (agent_picker.scan_installed_agents, agent_picker.cached_agents,
         agent_picker.list_agent_models) = saved
    return picker, size, None


def agent_picker_insights_builtin(variant):
    from ui_qt.widgets.agent_picker import BUILTIN

    return _picker("AI insights", BUILTIN)


def agent_picker_cleanup_claude(variant):
    return _picker("AI cleanup", "claude_code")


BUILDERS = {
    "agent_picker/insights-builtin": agent_picker_insights_builtin,
    "agent_picker/cleanup-claude": agent_picker_cleanup_claude,
}
