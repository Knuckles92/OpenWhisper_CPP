"""Import OpenWhisper from a read-only checkout without touching its data.

Mirrors OpenWhisper's tests/conftest.py: every writable path OpenWhisper resolves comes from
config.data_root(), and OPENWHISPER_DATA_DIR wins over the checkout and %LOCALAPPDATA%. That
variable must be set before ``config`` is first imported, because AppConfig resolves its paths
once, at import; ``isolate`` refuses to run once ``config`` is imported.

Also:
- LOCALAPPDATA, APPDATA and XDG_DATA_HOME point into the scratch folder, so components_root()
  and any per-user fallback land there too;
- no .pyc files: Python would otherwise write __pycache__ next to the checkout's sources;
- the credential store is in memory (the real one is Windows Credential Manager) and
  credentials.env_file_path() points into scratch, because a source checkout's own .env is the
  lookup's fallback and holds real keys;
- focus capture is the null service (no foreground-window or caret reads).

The checkout comes from OPENWHISPER_CPP_OW_ROOT. Scratch folders are under out/parity/ of this
repository (git-ignored). Call ``isolate()`` first, then ``after_import_guards()`` once
OpenWhisper modules can be imported. Scripts that use it run with ``python -I -B``.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools" / "parity"
SCRATCH = REPO / "out" / "parity"
REFERENCE_FILE = TOOLS / "reference.json"
ROOT_ENV = "OPENWHISPER_CPP_OW_ROOT"
PYTHON_ENV = "OPENWHISPER_CPP_OW_PYTHON"
PROXY_VARIABLES = ("http_proxy", "https_proxy", "all_proxy")


class IsolationError(RuntimeError):
    """The environment would let OpenWhisper write outside the scratch folder."""


def reference() -> dict:
    """tools/parity/reference.json: the OpenWhisper commit and the versions references pin."""
    return json.loads(REFERENCE_FILE.read_text(encoding="utf-8"))


def checkout() -> Path:
    value = os.environ.get(ROOT_ENV, "").strip()
    if not value:
        raise IsolationError(f"Set {ROOT_ENV} to the OpenWhisper checkout (read-only).")
    root = Path(value).resolve()
    if not (root / "config.py").is_file():
        raise IsolationError(f"{root} is not an OpenWhisper checkout (no config.py).")
    return root


def head_commit(root: Path) -> str:
    """The checkout's HEAD commit, read from .git without running git, which may take a lock or
    refresh the index inside the checkout."""
    git = root / ".git"
    if git.is_file():  # a worktree: "gitdir: <path>"
        git = Path(git.read_text(encoding="utf-8").split(":", 1)[1].strip())
    head = (git / "HEAD").read_text(encoding="utf-8").strip()
    if not head.startswith("ref:"):
        return head
    ref = head.split(":", 1)[1].strip()
    loose = git / ref
    if loose.is_file():
        return loose.read_text(encoding="utf-8").strip()
    common = git
    if (git / "commondir").is_file():
        common = (git / (git / "commondir").read_text(encoding="utf-8").strip()).resolve()
        if (common / ref).is_file():
            return (common / ref).read_text(encoding="utf-8").strip()
    packed = common / "packed-refs"
    if packed.is_file():
        for line in packed.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == ref:
                return parts[0]
    raise IsolationError(f"Cannot resolve {ref} in {git}.")


def check_reference_commit(root: Path, allow_other: bool = False) -> str:
    commit = head_commit(root)
    expected = reference()["ow_commit"]
    if commit != expected and not allow_other:
        raise IsolationError(
            f"{root} is at {commit[:12]}, but tools/parity/reference.json pins {expected[:12]}. "
            "Vectors and references are made only at the pinned commit; move the pin first.")
    return commit


def _inside(path: Path, folder: Path) -> bool:
    try:
        path.resolve().relative_to(folder.resolve())
        return True
    except ValueError:
        return False


def real_openwhisper_appdata() -> Path | None:
    """The owner's %LOCALAPPDATA%\\OpenWhisper, as it was before isolation moved LOCALAPPDATA."""
    base = os.environ.get("OPENWHISPER_CPP_REAL_LOCALAPPDATA") or os.environ.get("LOCALAPPDATA")
    return Path(base) / "OpenWhisper" if base else None


def scratch_environment(scratch: Path = SCRATCH) -> dict[str, str]:
    """The variables that move every OpenWhisper path into ``scratch``, for this process and
    for child processes (pytest runs)."""
    data = scratch / "ow-data"
    appdata = scratch / "ow-appdata"
    data.mkdir(parents=True, exist_ok=True)
    appdata.mkdir(parents=True, exist_ok=True)
    return {
        "OPENWHISPER_DATA_DIR": str(data),
        "LOCALAPPDATA": str(appdata),
        "APPDATA": str(appdata),
        "XDG_DATA_HOME": str(appdata),
        "HF_HUB_OFFLINE": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
    }


def isolate(scratch: Path = SCRATCH) -> Path:
    """Point every OpenWhisper path into ``scratch`` and put the checkout on sys.path. Returns
    the data root OpenWhisper will use."""
    if "config" in sys.modules:
        raise IsolationError("config was imported before isolation; nothing may import "
                             "OpenWhisper before ow_isolation.isolate().")
    root = checkout()
    if _inside(scratch, root):
        raise IsolationError(f"The scratch folder {scratch} is inside the checkout {root}.")
    owner = real_openwhisper_appdata()
    if owner is not None and _inside(scratch, owner):
        raise IsolationError(f"The scratch folder {scratch} is inside {owner}.")
    if owner is not None:
        os.environ.setdefault("OPENWHISPER_CPP_REAL_LOCALAPPDATA", str(owner.parent))
    sys.dont_write_bytecode = True
    os.environ.update(scratch_environment(scratch))
    for name in list(os.environ):
        if name.lower() in PROXY_VARIABLES:
            del os.environ[name]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    return Path(os.environ["OPENWHISPER_DATA_DIR"])


def after_import_guards(data: Path) -> None:
    """Check that OpenWhisper resolved its paths into scratch, then swap the services that would
    read or write the owner's machine for their in-memory or null versions."""
    import config as ow_config
    from services import credentials, focus_context

    root = Path(ow_config.data_root())
    if root != data:
        raise IsolationError(f"OpenWhisper's data root is {root}, not {data}.")
    for name in ("SETTINGS_FILE", "DATABASE_FILE"):
        value = getattr(ow_config.config, name)
        if not value.startswith(str(data) + os.sep):
            raise IsolationError(f"config.{name} is {value}, outside {data}.")
    if not ow_config.components_root().startswith(os.environ["LOCALAPPDATA"]):
        raise IsolationError(f"components_root() is {ow_config.components_root()}.")
    credentials.set_store(credentials.CredentialStore(backend_factory=credentials.memory_backend))
    env_file = str(data / ow_config.ENV_FILE_NAME)
    credentials.env_file_path = lambda: env_file
    focus_context.set_service(focus_context.NullCaptureService())
