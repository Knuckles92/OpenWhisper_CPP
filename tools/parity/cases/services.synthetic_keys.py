"""Curated inputs for services/synthetic_keys.py (M1.6): which apps are terminals and which
editors copy the caret's line when nothing is selected.

AppIdentity travels as a JSON object of its fields (null for None).
"""

from services.focus_context import AppIdentity


def _identity(fields):
    return None if fields is None else AppIdentity(**fields)


def _app(app_id, name="", platform="windows", **extra):
    return {"app_id": app_id, "name": name, "platform": platform, **extra}


_IDENTITIES = [
    None,
    _app("WindowsTerminal.exe", "Windows Terminal"), _app("conhost.exe", "Console"),
    _app("OpenConsole.exe"), _app("powershell.exe", "Windows PowerShell"), _app("pwsh.exe"),
    _app("cmd.exe", "Command Prompt"), _app("wezterm-gui.exe"), _app("alacritty"),
    _app("kitty", platform="hyprland"), _app("com.mitchellh.ghostty", platform="hyprland"),
    _app("foot", platform="hyprland"), _app("com.googlecode.iterm2", "iTerm2", "macos"),
    _app("com.apple.Terminal", "Terminal", "macos"),
    _app("Code.exe", "Visual Studio Code"), _app("Cursor.exe", "Cursor"), _app("Windsurf.exe"),
    _app("devenv.exe", "Visual Studio"), _app("idea64.exe", "IntelliJ IDEA"),
    _app("pycharm64.exe", "PyCharm"), _app("rider64.exe"), _app("studio64.exe", "Android Studio"),
    _app("sublime_text.exe", "Sublime Text"), _app("zed", platform="linux"),
    _app("jetbrains-pycharm", platform="hyprland"),
    _app("com.jetbrains.goland", "GoLand", "macos"),
    _app("com.microsoft.VSCode", "Code", "macos"),
    _app("outlook.exe", "Outlook"), _app("chrome.exe", "Google Chrome"),
    _app("notepad.exe", "Notepad"), _app("powershell_ise.exe", "Windows PowerShell ISE"),
    _app("C:\\Program Files\\Zed\\zed.exe"), _app("/usr/bin/code"), _app("tool.app.exe", "Zed"),
    _app("fleet64"), _app("dataspell"), _app("aqua64.exe"), _app("clion"), _app("idea"),
    _app("ideas"), _app("intellij idea ultimate"), _app("android studio.app"),
    _app("Code - Insiders.exe"), _app("zeditor"), _app("notepad++.exe", "Notepad++"),
    _app("vim", "Vim"), _app("gvim.exe"), _app("", "Code"), _app("  ", "  "),
    _app("conhost.exe", "Console", is_self=True), _app("openwhisper", "OpenWhisper",
                                                       is_self=True),
    _app("\u00c9DITEUR.EXE", "\u00c9diteur"), _app("STUDIO64.EXE"),
]

CASES = {
    "is_terminal": [[identity] for identity in _IDENTITIES],
    "copies_line_without_selection": [[identity] for identity in _IDENTITIES],
}

WRAPPERS = {
    "is_terminal": lambda module, identity: module.is_terminal(_identity(identity)),
    "copies_line_without_selection": lambda module, identity: (
        module.copies_line_without_selection(_identity(identity))),
}
