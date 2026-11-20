"""The pure half of services/backup.py (M3.1 backup-create).

    _scrub(value[, keep_urls])                   the scrubbed value
    _sanitize_settings(settings)                 the sanitized settings (no hotkeys in actions:
                                                 services.actions.model's vectors pin those)
    _member_parts(name)                          the parts, or {"error": message}
    _safe_member(name)                           the name, or {"error": message}; OpenWhisper's
                                                 owned names (the replay reads them through
                                                 legacy_identity())
    _relative_under(path_text, root, group)      the relative text, None, or {"error": message};
                                                 root is a Windows path (pins.platform)
    _meeting_session_updates(columns, rel)       [[column, value]] with each fresh token given
                                                 as "<token:<length>>" when it is URL-safe
                                                 base64, else as itself
    _check_staged_file(manifest, item, files, listed)
                                                 (M3.4) {"listed": [...]} or {"error": message,
                                                 "listed": [...]}: `files` ({path below data/:
                                                 text}) are written into a scratch stage's data
                                                 folder, and `listed` is the set the check
                                                 starts from
"""

import hashlib
import re
import shutil
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "backup"

TOKEN_COLUMNS = ("host_token", "guest_token")


def _error_or(call):
    from services.backup import BackupError

    try:
        return call()
    except BackupError as exc:
        return {"error": str(exc)}


def _member_parts(module, name):
    return _error_or(lambda: module._member_parts(name))


def _safe_member(module, name):
    return _error_or(lambda: module._safe_member(name))


def _relative_under(module, path_text, root, group):
    return _error_or(lambda: module._relative_under(path_text, Path(root), group))


def _meeting_session_updates(module, columns, rel):
    rows = []
    for column, value in module._meeting_session_updates(set(columns), rel).items():
        if column in TOKEN_COLUMNS and isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_-]+", value):
            value = f"<token:{len(value)}>"
        rows.append([column, value])
    return rows


def _check_staged_file(module, manifest, item, files, listed):
    from services.backup import BackupError

    SCRATCH.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=SCRATCH))
    try:
        stage = root / ("stage-" + "0" * 32)
        (stage / "data").mkdir(parents=True)
        for rel, text in files.items():
            path = stage / "data" / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("utf-8"))
        seen = set(listed)
        try:
            module._check_staged_file(stage, manifest, item, seen)
        except BackupError as exc:
            return {"error": str(exc), "listed": sorted(seen)}
        return {"listed": sorted(seen)}
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "_check_staged_file": _check_staged_file,
    "_member_parts": _member_parts,
    "_safe_member": _safe_member,
    "_relative_under": _relative_under,
    "_meeting_session_updates": _meeting_session_updates,
}

_URLS = [
    "https://user:pw@example.com/v1?key=abc#frag",
    "http://example.com",
    "HTTPS://Example.com/Path?q=1",
    "Https://User@Host:80/p;params?q#f",
    "https://example.com?x=1",
    "https://example.com#frag",
    "https://a@b@c.com/x",
    "http://",
    "https://",
    "http:///path",
    "http://user@/path",
    "http://user:pw@?q",
    "http://[::1]:8080/x?y",
    "https://u@[::1]/",
    "https://[::1%25eth0]/a",
    "https://[fe80::1%]/",
    "https://[::ffff:1.2.3.4]/",
    "https://[::ffff:1.2.3.04]/",
    "https://[1:2:3:4:5:6:7:8:9]/",
    "https://[1:2:3:4:5:6:7:8]/",
    "https://[1::2::3]/",
    "https://[:1:2:3:4:5:6:7]/",
    "https://[1:2:3:4:5:6:7::]/",
    "https://[::]/",
    "https://[12345::]/",
    "https://[v1.fe]/",
    "https://[v.x]/",
    "https://[vG.x]/",
    "https://[V1.x]/",
    "https://[1.2.3.4]/",
    "https://[zz]/",
    "https://[::1]x/",
    "https://x[::1]/",
    "https://[x]@host/",
    "http://[bad/x",
    "HTTP://bad[",
    "http://bad]/",
    "xhttp://[",
    "http]://x",
    "http://ex\u2100ample.com/",
    "https://ex\uff0fample.com/",
    "http://\uff20host/",
    "http://h\u2047/x",
    "https://\u00e9xample.com/p?q",
    "https://x.com/\u00e9?q",
    "  https://u:p@h.com/a?b",
    "\x00\x01https://u:p@h.com/a?b",
    "https://h.com/a\tb?c",
    "https://h.com/a\r\nb",
    "https:\\\\host\\path?q",
    "https:host/path?q",
    "ftp://user:pw@host/x?y",
    "mailto:x@y.com",
    "not a url",
    "",
    "https://ex%40mple.com/?q",
    "https://host:99999/x?y",
    "HtTp://Host.Example/A/B/?q=1#z",
]

_SETTINGS_BASE = {
    "ui_theme": "dark",
    "remote_engine_client": {"host": "elsewhere", "fingerprint": "old"},
    "remote_host_enabled": True,
    "remote_host_devices": [{"token_hash": "old-token-digest"}],
    "remote_records_location": "host",
    "mcp_enabled": True,
    "meeting_context_folder_path": "C:\\Users\\dana\\external",
    "legacy_secret": "do-not-export",
}

CASES = {
    "_scrub": [[url] for url in _URLS] + [
        ["https://u:p@h/x?y", True],
        [1], [1.5], [None], [True], [[]], [{}],
        [["https://h/x?y", 1, None, True, ["http://a:b@c/d?e"]]],
        [{"a": {"api_key": "x", "url": "https://u:p@h/x?y", "Headers": {"X": "1"},
                "list": ["https://h/x?y", 1, None, True]},
          "dictation_snippets": [{"text": "https://x/?a=1", "password": "p"}],
          "nested": {"dictation_snippets": {"deep": "https://h/?q", "inner": {"u": "https://h/?q"}}},
          "MyToken": "t", "cookieJar": 1, "Authorization": "b", "PRIVATE_KEY_PATH": "k",
          "credentials_file": "c", "secretive": 2, "\u0130api_key": 3, "headerless": 4,
          "plain": "https://u@h/?q#f"}],
        [{"dictation_snippets": "https://h/?q", "other": "https://h/?q"}],
        [{"keep": "https://h/?q"}, True],
    ],
    "_sanitize_settings": [
        [{}],
        [_SETTINGS_BASE],
        [{"ui_theme": "light", "mcp_enabled": False, "meeting_context_folder_path": "",
          "transcript_cleanup_base_url": "https://user:pw@api.example.com/v1?key=abc",
          "dictation_snippets": [{"id": "s1", "trigger": "cal",
                                  "text": "Book: https://cal.example.com/dana?ref=email#slots",
                                  "formatted": False}],
          "text_llm_profiles": [{"base_url": "https://person:password@example.com/v1?token=s#f",
                                 "api_key": "k", "name": "P"}]}],
        [{"automation_webhooks": [{"url": "https://h/x?sig=1", "enabled": True}, "loose", 3,
                                  {"name": "no flag"}],
          "automation_programs": [{"program": "C:\\x.exe", "enabled": True}],
          "automation_filters": "not a list",
          "api_run_actions": True, "api_control_recording": True, "api_text_filters": True,
          "api_agent_prompts": True, "mcp_events_enabled": True, "mcp_tailscale_enabled": True,
          "meeting_server_bind": "0.0.0.0", "meeting_context_folder_enabled": True,
          "remote_host_model_management": True, "remote_host_tailscale_trust": True,
          "remote_host_keep_records": True, "remote_host_manage_mcp": True,
          "remote_client_history": True}],
        [{"actions": None, "ui_theme": "dark"}],
        [{"actions": "not a list"}],
        [{"actions": [], "zzz": 1}],
        [{"ui_theme": "dark", "actions": [
            {"id": "w", "name": "Slack", "kind": "webhook",
             "params": {"url": "https://user:hunter2@hooks.example.com/hook?token=SECRET",
                        "output": "none", "secret": "x"}, "enabled": True},
            {"id": "p", "name": "P", "kind": "program ",
             "params": {"program": "curl.exe", "args": ["https://u:pw@example.com/a?sig=S", 7],
                        "timeout_s": 30}},
            {"id": "a", "name": "A", "kind": "agent", "params": {"agent": "codex"}},
            {"id": "l", "name": "Link", "kind": "open_link",
             "params": {"template": "https://api.example.com/x?key=LINK-KEY&q={text}"},
             "description": "Opens https://u:p@d.example/?q"},
            {"id": "c", "name": "C", "kind": "clipboard", "enabled": False},
            {"id": "x2", "name": "Hook", "kind": " Webhook", "params": {"url": "https://h"}},
            {"id": "s", "name": "S", "kind": "scripts", "spoken_names": ["s cripts", "S CRIPTS"]},
            {"id": "ch", "name": "Ch", "kind": "channel", "shaping": "raw"},
            "loose", 3, None,
            {"id": "w", "name": "Dup", "kind": "webhook"},
        ], "after": "https://u:p@h/?q"}],
    ],
    "_member_parts": [[name] for name in [
        "manifest.json", "data/recordings/a.wav", "data", "a/b/c", "", "/data/x", "data//x",
        "data/./x", "data/../x", "data/x/", "data\\x", "data/x\x00y", "data/\x1fx", "data/\x7fx",
        "data/a:b", "C:/x", "data/.hidden", "data/..x", "data/x..", "data/\u00e9t\u00e9",
        ".", "..", "data/ x", "data/x ",
    ]] + [[None], [5], [["data", "x"]]],
    "_safe_member": [[name] for name in [
        "manifest.json", "data/openwhisper_settings.json", "data/openwhisper.db",
        "data/openwhisper-cpp.db", "data/recordings/a.wav", "data/recordings/devices/peer/r.wav",
        "data/meetings/m1/mic-000.wav", "data/captures/2026/shot.png",
        "data/.recorded_audio-recovery/x/session.json", "data/recorded_audio.wav",
        "data/scratchpad.txt", "data/meetings.json", "data/meetings.json.bak",
        "data/transcription_history.json", "data/transcription_history.json.bak",
        "data/remote_records", "data/remote_records/x.json", "data/remote_records/cache/x",
        "data/scratchpad.txt/x", "data/openwhisper.db-wal", "data/.openwhisper-restore/x",
        "data/Recordings/a.wav", "data", "other/recordings/a.wav", "manifest.json/x",
        "data/recordings", "data/../recordings", "data/recordings/a:b.wav",
        "MANIFEST.json", "data/settings.json",
    ]] + [[None]],
    "_relative_under": [[text, "C:\\Data\\OW", group] for text, group in [
        ("C:\\Data\\OW\\meetings\\m1", "meetings"),
        ("c:/data/ow/meetings/m1", "meetings"),
        ("C:\\Data\\OW\\Meetings\\m1", "meetings"),
        ("meetings/m1", "meetings"),
        ("meetings\\m1\\mic-000.wav", "meetings"),
        ("meetings", "meetings"),
        ("", "meetings"),
        (".", "meetings"),
        ("C:\\Other\\meetings\\m1", "meetings"),
        ("C:\\Data\\OW", "meetings"),
        ("C:\\Data\\OW\\meetings", "meetings"),
        ("C:\\Data\\OW\\meetings\\..\\recordings\\x", "meetings"),
        ("C:\\Data\\OW\\recordings\\a.wav", "recordings"),
        ("C:\\Data\\OW\\recordings\\a.wav", "meetings"),
        ("/meetings/x", "meetings"),
        ("\\Data\\OW\\meetings\\x", "meetings"),
        ("C:meetings\\x", "meetings"),
        ("c:meetings\\x", "meetings"),
        ("C:\\Data\\OW\\meetings\\a:b", "meetings"),
        ("C:\\Data\\OW\\.\\meetings\\m1", "meetings"),
        ("C:\\Data\\OW\\meetings\\m1\\", "meetings"),
        ("C:\\Data\\OW\\\\meetings\\\\m1", "meetings"),
        ("\\\\server\\share\\meetings", "meetings"),
        ("C:\\Data\\OW2\\meetings\\x", "meetings"),
        ("C:/Data/OW/meetings/caf\u00e9", "meetings"),
        ("C:\\Data\\OW\\meetings\\m1\\..\\..\\..\\x", "meetings"),
        ("C:\\Data\\OW\\recordings\\devices\\peer\\r.wav", "recordings"),
    ]] + [
        ["C:\\D\u00c4T\u00c4\\meetings\\x", "C:\\D\u00e4t\u00e4", "meetings"],
        ["C:\\Data\\OW\\meetings\\x", "C:\\Data\\OW\\", "meetings"],
    ],
    "_meeting_session_updates": [
        [["id", "spool_dir", "status", "host_token", "guest_token", "asr_remote_json",
          "agent_endpoint_json", "app_pid", "app_heartbeat_at"], "meetings/m1"],
        [["id", "spool_dir"], "meetings/m2"],
        [["id", "spool_dir", "guest_token", "app_pid"], "meetings/caf\u00e9"],
        [[], "meetings/x"],
    ],
}

def _sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


_STAGED_FILES = {"openwhisper.db": "abc", "openwhisper_settings.json": "{}",
                 "recordings/a.wav": "RIFF", "meetings/m1/mic-000.wav": "RIFFm"}
_STAGED_MANIFEST = {"format": "openwhisper-backup", "staged_db_size": 3,
                    "staged_db_sha256": _sha("abc"), "staged_settings_size": 2,
                    "staged_settings_sha256": _sha("{}")}
CASES["_check_staged_file"] = [
    [_STAGED_MANIFEST, item, _STAGED_FILES, listed] for item, listed in [
        ({"path": "data/openwhisper.db", "size": 999, "sha256": "x"}, []),
        ({"path": "data/openwhisper_settings.json"}, []),
        ({"path": "data/recordings/a.wav", "size": 4, "sha256": _sha("RIFF")}, []),
        ({"path": "data/meetings/m1/mic-000.wav", "size": 5, "sha256": _sha("RIFFm")},
         ["data/recordings/a.wav"]),
        ({"path": "data/recordings/a.wav", "size": 5, "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "size": 4, "sha256": _sha("RIFX")}, []),
        ({"path": "data/recordings/a.wav", "size": 4, "sha256": _sha("RIFF").upper()}, []),
        ({"path": "data/recordings/a.wav", "size": 4, "sha256": 5}, []),
        ({"path": "data/recordings/a.wav", "size": 4}, []),
        ({"path": "data/recordings/a.wav", "size": True, "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "size": -1, "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "size": 4.0, "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "size": "4", "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings/a.wav", "size": 4, "sha256": _sha("RIFF")},
         ["data/recordings/a.wav"]),
        ({"path": "data/recordings/b.wav", "size": 4, "sha256": _sha("RIFF")}, []),
        ({"path": "data/recordings", "size": 4, "sha256": _sha("RIFF")}, []),
        ({"size": 4, "sha256": _sha("RIFF")}, []),
        ({"path": None, "size": 4}, []),
        ({"path": "manifest.json", "size": 4}, []),
        ({"path": "data/../x", "size": 4}, []),
        ({"path": "data/remote_records/x", "size": 4}, []),
        ({"path": "data/openwhisper-cpp.db", "size": 3}, []),
        ({"path": "data/openwhisper.db-wal", "size": 3}, []),
        ("data/recordings/a.wav", []),
        (None, []),
    ]
] + [
    [{}, {"path": "data/openwhisper.db", "size": 3, "sha256": _sha("abc")}, _STAGED_FILES, []],
    [{"staged_db_size": 3, "staged_db_sha256": None}, {"path": "data/openwhisper.db"},
     _STAGED_FILES, []],
    [{"staged_db_size": 4, "staged_db_sha256": _sha("abc")}, {"path": "data/openwhisper.db"},
     _STAGED_FILES, []],
]

PINS = {"note": "Windows paths: _relative_under's root is a WindowsPath, as on the owner's "
                "machine; the replay runs it on Windows only."}
