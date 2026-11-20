"""Curated inputs for services/agent_mcp/setup.py (M5.5 history-api).

The connection snippets with a fixed URL, token and host name; the channel entry with a fixed
executable, data root and per-user folder. setup.py reads those three from sys.executable,
config.is_frozen(), config.data_root() and config.local_app_dir(), so the wrappers pin them for
the call and put them back. FioriVoce is always a built executable: only the frozen branch of
channel_command_line is recorded.

    channel_command_line(executable)
    _channel_env(data_root, local_app_dir)
    claude_channel_config(executable, data_root, local_app_dir)
    TOKEN_PLACEHOLDER(), CHANNEL_SERVER_NAME()     the constants
"""
import contextlib
import sys


@contextlib.contextmanager
def _pinned(executable=None, data_root=None, local_app_dir=None):
    import config

    saved = (sys.executable, config.is_frozen, config.data_root, config.local_app_dir)
    try:
        if executable is not None:
            sys.executable = executable
            config.is_frozen = lambda: True
        if data_root is not None:
            config.data_root = lambda: data_root
        if local_app_dir is not None:
            config.local_app_dir = lambda: local_app_dir
        yield
    finally:
        sys.executable, config.is_frozen, config.data_root, config.local_app_dir = saved


def _command_line(module, executable):
    with _pinned(executable=executable):
        return module.channel_command_line()


def _env(module, data_root, local_app_dir):
    with _pinned(data_root=data_root, local_app_dir=local_app_dir):
        return module._channel_env()


def _channel_config(module, executable, data_root, local_app_dir):
    with _pinned(executable, data_root, local_app_dir):
        return module.claude_channel_config()


WRAPPERS = {
    "channel_command_line": _command_line,
    "_channel_env": _env,
    "claude_channel_config": _channel_config,
    "TOKEN_PLACEHOLDER": lambda m: m.TOKEN_PLACEHOLDER,
    "CHANNEL_SERVER_NAME": lambda m: m.CHANNEL_SERVER_NAME,
}

LOCAL = "http://127.0.0.1:8767/mcp"
TAILSCALE = "http://100.101.102.103:8767/mcp"
TOKEN = "Zq3_x-9AbCdEfGhIjKlMnOpQrStUvWxYz0123456789"
EXE = "C:\\Users\\Dana\\AppData\\Local\\Programs\\OpenWhisper\\OpenWhisper.exe"
LOCAL_APP = "C:\\Users\\Dana\\AppData\\Local\\OpenWhisper"

_SNIPPETS = [
    [LOCAL, TOKEN],
    [LOCAL],
    [TAILSCALE, TOKEN],
    ["http://localhost:9000/mcp", "<PASTE_TOKEN>"],
    ["http://desk.tail1234.ts.net:8767/mcp", 'quote " and \\ backslash'],
    ["http://café.example/mcp", "tökén"],
]

CASES = {
    "client_config": _SNIPPETS,
    "claude_command": _SNIPPETS,
    "chatgpt_config": _SNIPPETS,
    "agent_prompt": [
        [LOCAL, "DESKTOP-7", ""],
        [LOCAL, "DESKTOP-7", TOKEN],
        ["http://localhost:8767/mcp", "studio", ""],
        ["http://LOCALHOST:8767/mcp", "studio", TOKEN],
        ["http://[::1]:8767/mcp", "laptop", ""],
        ["http://user:pw@127.0.0.1:8767/mcp", "laptop", ""],
        [TAILSCALE, "DESKTOP-7", ""],
        [TAILSCALE, None, TOKEN],
        ["http://desk.tail1234.ts.net:8767/mcp", "desk", TOKEN],
        ["not a url", "desk", ""],
        ["http://127.0.0.2:8767/mcp", "desk", ""],
    ],
    "channel_command_line": [[EXE], ["D:\\Apps\\Ö W\\OpenWhisper.exe"]],
    "_channel_env": [
        [LOCAL_APP, LOCAL_APP],
        ["D:\\Data\\OpenWhisper", LOCAL_APP],
        ["c:/users/dana/appdata/local/openwhisper", LOCAL_APP],
        [LOCAL_APP + "\\", LOCAL_APP],
        [LOCAL_APP + "\\..\\OpenWhisper", LOCAL_APP],
        ["D:\\Dätä\\東京", LOCAL_APP],
    ],
    "claude_channel_config": [
        [EXE, LOCAL_APP, LOCAL_APP],
        [EXE, "D:\\Data\\OpenWhisper", LOCAL_APP],
        ["D:\\Apps\\Ö W\\OpenWhisper.exe", "D:\\Dätä\\東京", LOCAL_APP],
    ],
    "claude_channel_command": [[]],
    "TOKEN_PLACEHOLDER": [[]],
    "CHANNEL_SERVER_NAME": [[]],
}
