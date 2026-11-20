"""Write src/settings/keys.inc and src/settings/data/enums.json from OpenWhisper's settings.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/settings_keys.py [--check]

OpenWhisper is imported through tools/parity/ow_isolation.py, so nothing it resolves points at
the owner's data. The declarations are read by walking ``services.settings.ALL_SETTINGS`` (not
the source text), so a default that reads ``config`` comes out as config evaluated on this
machine; the script refuses to run anywhere but Windows x64, whose values OpenWhisper_CPP ships.

keys.inc holds, inside namespace fv::keys, one ``inline constexpr`` declaration per setting in
OpenWhisper's declaration order, then the OpenWhisper_CPP-only keys of tools/gen/fv_settings_keys.json,
the choice and alias tables they point to, ``declared`` (every setting, in that order) and
``legacy_streaming`` (LEGACY_STREAMING_KEYS). OpenWhisper_CPP's deliberate differences are applied
here and nowhere else:

- ``selected_model`` keeps only the backends OpenWhisper_CPP has (architecture 7.2), in OpenWhisper's
  order; any other stored backend reads as the default, as ChoiceSetting already does.
- The keys of dropped engines are declared ``dropped``: preserved on rewrite, never read.

enums.json holds the value classes of services/settings.py (ALL, LABELS, DETAILS, ALIASES,
INSTALLED, CHOICES, in their order) and the module constants other packages need.

--check compares with the committed files instead of writing them, and fails on a difference.
A normalizer, kind or key this script does not know stops it: a new one needs a C++ port first.
"""

from __future__ import annotations

import argparse
import json
import keyword
import math
import platform
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

KEYS_INC = ROOT / "src" / "settings" / "keys.inc"
ENUMS_JSON = ROOT / "src" / "settings" / "data" / "enums.json"
FV_KEYS = ROOT / "tools" / "gen" / "fv_settings_keys.json"

#: The backends OpenWhisper_CPP has (architecture 7.2): Parakeet in process, the API, a paired host.
FV_BACKENDS = ("parakeet", "api", "remote")
#: Keys that served only engines OpenWhisper_CPP dropped (the a10 map's engine-specific list).
DROPPED_KEYS = (
    "whisper_model", "custom_whisper_models", "whisper_device", "whisper_compute_type",
    "whisper_gpu_offer_declined", "meeting_whisper_model",
)
#: Value classes of services/settings.py, in source order, and the attributes kept of each.
ENUM_CLASSES = (
    "RecordingRetentionMode", "RecordingTriggerMode", "UiFontScale", "UiTheme",
    "WalkthroughCopyFormat", "WalkthroughSnapMode", "WalkthroughPageSize",
    "RecordingOverlayLook", "StreamingOverlayLayout", "TranscriptCleanupProvider",
    "TranscriptCleanupLevel", "TranscriptCleanupModelSort", "TranscriptCleanupReasoning",
    "MeetingAgentCore", "TypeSafeProvider", "TypeSafeTextModel", "MeetingSpeakerIdBackend",
    "MeetingLanguage", "MeetingServerBind", "HuggingFaceAccessPolicy",
)
CONSTANTS = (
    "MCP_DEFAULT_PORT", "REMOTE_HOST_DEFAULT_PORT", "WALKTHROUGH_CAPTURE_DELAYS",
    "MEETING_LINUX_PREVIEW_ACK_VERSION", "INSIGHT_REVIEW_CONSENT", "DEFAULT_VOICE_COMMAND_NAMES",
)
# C++ names a key may not take, beside the keywords: the names keys.h and keys.inc define in
# namespace fv::keys.
RESERVED = {"choices", "int_choices", "aliases", "declared", "legacy_streaming", "all",
            "by_key", "defaults_table"}
CXX_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
    "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
    "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
}


class GeneratorError(RuntimeError):
    """The declarations hold something the C++ side has no port of."""


def cxx_string(text: str) -> str:
    if "openwhisper" in text.lower():
        raise GeneratorError(f"{text!r} names OpenWhisper; lint.brand refuses it in keys.inc")
    out = []
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            raise GeneratorError(f"{text!r}: only printable ASCII is written to keys.inc")
    return '"' + "".join(out) + '"'


def cxx_int(value: int) -> str:
    if isinstance(value, bool) or not isinstance(value, int):
        raise GeneratorError(f"{value!r} is not an int")
    if not -(2 ** 63) <= value < 2 ** 63:
        raise GeneratorError(f"{value} does not fit std::int64_t")
    return str(value)


def cxx_bound(value) -> str:
    return "std::nullopt" if value is None else cxx_int(value)


def cxx_double(value: float) -> str:
    if not math.isfinite(value):
        raise GeneratorError(f"{value!r}: a float default must be finite")
    text = repr(float(value))
    return text if any(c in text for c in ".e") else text + ".0"


def identifier(key: str) -> str:
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", key) or key in CXX_KEYWORDS or key in RESERVED:
        raise GeneratorError(f"the key {key!r} cannot be a C++ name in namespace fv::keys")
    if keyword.iskeyword(key):
        raise GeneratorError(f"the key {key!r} is a Python keyword")
    return key


def normalizer(settings_module, config, fn) -> str:
    """The C++ spelling of a declaration's normalize callable (spec setters)."""
    if fn is None:
        return ""
    if fn is str.strip:
        return ".normalized(Normalize::Strip)"
    if fn is getattr(settings_module, "_lower_stripped", None):
        return ".normalized(Normalize::LowerStripped)"
    if fn is getattr(settings_module, "_batch_instructions", None):
        limit = config.MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS
        # The port of value.strip()[:limit]; checked here so a changed body is noticed.
        for sample in ("  x  ", " " + "y" * (limit + 3) + " ", "\u00e9" * (limit + 1)):
            if fn(sample) != sample.strip()[:limit]:
                raise GeneratorError("_batch_instructions is no longer strip()[:limit]")
        return f".stripped_and_cut({cxx_int(limit)})"
    raise GeneratorError(f"no C++ port of the normalizer {fn!r}")


class Declaration:
    """One setting as keys.inc writes it."""

    def __init__(self, setting, kind: str, spec: str, tables: list[str], comment: str = ""):
        self.key = str(setting.key)
        self.kind = kind
        self.spec = spec
        self.tables = tables
        self.comment = comment


def declare(setting, settings_module, types, config, *, dropped: bool, fv_note: str = ""):
    kind = type(setting).__name__
    key = identifier(str(setting.key))
    tables: list[str] = []
    comments: list[str] = []
    default = setting._default
    if kind == "BoolSetting":
        if not isinstance(default, bool):
            raise GeneratorError(f"{key}: a bool default must be a bool")
        spec = f"spec::boolean({'true' if default else 'false'})"
    elif kind == "IntSetting":
        spec = f"spec::integer({cxx_int(default)})"
        if setting.strict:
            spec += ".strict()"
        if setting.valid != (None, None):
            spec += f".valid({cxx_bound(setting.valid[0])}, {cxx_bound(setting.valid[1])})"
        if setting.clamp != (None, None):
            spec += f".clamp({cxx_bound(setting.clamp[0])}, {cxx_bound(setting.clamp[1])})"
    elif kind == "FloatSetting":
        if isinstance(default, bool) or not isinstance(default, (int, float)):
            raise GeneratorError(f"{key}: a float default must be a number")
        spec = f"spec::number({cxx_double(default)})"
    elif kind == "StrSetting":
        if not isinstance(default, str):
            raise GeneratorError(f"{key}: a str default must be a str")
        spec = f"spec::text({cxx_string(default)})" + normalizer(
            settings_module, config, setting.normalize)
    elif kind == "ChoiceSetting":
        choices = tuple(setting.choices)
        if key == "selected_model":
            kept = tuple(c for c in choices if c in FV_BACKENDS)
            if set(kept) != set(FV_BACKENDS):
                raise GeneratorError(f"selected_model lost a OpenWhisper_CPP backend: {choices}")
            gone = [c for c in choices if c not in FV_BACKENDS]
            comments.append("OpenWhisper's choices less the dropped engines ("
                            + ", ".join(gone) + "), which read as the default (architecture 7.2).")
            choices = kept
        if default not in choices:
            raise GeneratorError(f"{key}: default {default!r} is not one of its choices")
        tables.append(f"inline constexpr std::string_view {key}[] = {{"
                      + ", ".join(cxx_string(c) for c in choices) + "};")
        spec = f"spec::choice({cxx_string(default)}, choices::{key})"
        if setting.aliases:
            for source, target in setting.aliases.items():
                if target not in choices:
                    raise GeneratorError(f"{key}: alias {source!r} names {target!r}")
            spec += f".with_aliases(aliases::{key})"
        spec += normalizer(settings_module, config, setting.normalize)
    elif kind == "IntChoiceSetting":
        # Unlike ChoiceSetting, Python does not require the default among the choices
        # (meeting_linux_preview_ack_version reads 0 until the one accepted version is saved).
        tables.append(f"inline constexpr std::int64_t {key}[] = {{"
                      + ", ".join(cxx_int(c) for c in setting.choices) + "};")
        spec = f"spec::int_choice({cxx_int(default)}, int_choices::{key})"
        if setting.strict:
            spec += ".strict()"
    elif kind in ("ListSetting", "DictSetting"):
        expected = list if kind == "ListSetting" else dict
        if not isinstance(default, expected):
            raise GeneratorError(f"{key}: a {kind} default must be a {expected.__name__}")
        maker = "list" if kind == "ListSetting" else "dict"
        spec = f"spec::{maker}({cxx_string(json.dumps(default))})"
    elif kind in ("OptionalListSetting", "OptionalDictSetting", "RawSetting"):
        if default is not None:
            raise GeneratorError(f"{key}: a {kind} has no default")
        spec = {"OptionalListSetting": "spec::optional_list()",
                "OptionalDictSetting": "spec::optional_dict()",
                "RawSetting": "spec::raw()"}[kind]
    else:
        raise GeneratorError(f"{key}: no C++ port of the kind {kind}")

    # The kinds that are never in the table say so themselves (setting_types.py).
    if not setting.in_table and kind not in ("OptionalListSetting", "OptionalDictSetting",
                                             "RawSetting"):
        spec += ".not_in_table()"
    if dropped:
        spec += ".dropped()"
        comments.append("A dropped engine's key: kept on rewrite, never read (architecture 7.2).")
    if fv_note:
        comments.append(fv_note)
    alias_table = []
    if kind == "ChoiceSetting" and setting.aliases:
        alias_table.append(f"inline constexpr ChoiceAlias {key}[] = {{"
                           + ", ".join(f"{{{cxx_string(a)}, {cxx_string(b)}}}"
                                       for a, b in setting.aliases.items()) + "};")
    declaration = Declaration(setting, kind, spec, tables, " ".join(comments))
    declaration.alias_tables = alias_table
    return declaration


def fv_settings(types) -> list[tuple[object, str]]:
    document = json.loads(FV_KEYS.read_text(encoding="utf-8"))
    made = []
    for entry in document["settings"]:
        key = entry["key"]
        if not key.startswith("fv_"):
            raise GeneratorError(f"{FV_KEYS.name}: {key} lacks the fv_ prefix")
        cls = getattr(types, entry["kind"])
        kwargs = {}
        if "in_table" in entry:
            kwargs["in_table"] = entry["in_table"]
        if entry["kind"] in ("OptionalListSetting", "OptionalDictSetting", "RawSetting"):
            setting = cls(key)
        elif entry["kind"] in ("ChoiceSetting", "IntChoiceSetting"):
            setting = cls(key, entry["default"], tuple(entry["choices"]), **kwargs)
        else:
            setting = cls(key, entry["default"], **kwargs)
        made.append((setting, entry.get("note", "")))
    return made


def render_keys(declarations: list[Declaration], legacy: tuple[str, ...], commit: str) -> str:
    out = [
        "// Generated by tools/gen/settings_keys.py from OpenWhisper's settings declarations",
        f"// (services/settings.py at {commit[:12]}) and tools/gen/fv_settings_keys.json.",
        "// Do not edit. Included inside namespace fv::keys by settings/keys.h.",
        "// clang-format off",
        "",
        "namespace choices {",
    ]
    for d in declarations:
        out += [t for t in d.tables if " std::string_view " in t]
    out += ["} // namespace choices", "", "namespace int_choices {"]
    for d in declarations:
        out += [t for t in d.tables if " std::int64_t " in t]
    out += ["} // namespace int_choices", "", "namespace aliases {"]
    for d in declarations:
        out += d.alias_tables
    out += ["} // namespace aliases", ""]
    for d in declarations:
        if d.comment:
            out.append(f"// {d.comment}")
        out.append(f"inline constexpr {d.kind} {d.key}{{{cxx_string(d.key)}, {d.spec}}};")
    out += ["", "// Every setting, in declaration order (ALL_SETTINGS, then OpenWhisper_CPP's).",
            "inline constexpr const SettingBase* declared[] = {"]
    for d in declarations:
        out.append(f"    &{d.key},")
    out += ["};", "",
            "// LEGACY_STREAMING_KEYS: dropped whenever the live-preview toggle is written.",
            "inline constexpr std::string_view legacy_streaming[] = {"]
    out += [f"    {cxx_string(k)}," for k in legacy]
    out += ["};", "", "// clang-format on", ""]
    return "\n".join(out)


def enum_record(cls) -> dict:
    record: dict = {}
    if hasattr(cls, "ALL"):
        record["values"] = list(cls.ALL)
    if hasattr(cls, "CHOICES"):
        record["labels"] = [[code, label] for code, label in cls.CHOICES]
    if hasattr(cls, "LABELS"):
        record["labels"] = [[value, label] for value, label in cls.LABELS.items()]
    if hasattr(cls, "DETAILS"):
        record["details"] = [[value, text] for value, text in cls.DETAILS.items()]
    if hasattr(cls, "ALIASES"):
        record["aliases"] = [[old, new] for old, new in cls.ALIASES.items()]
    if hasattr(cls, "INSTALLED"):
        record["installed"] = list(cls.INSTALLED)
    # Members outside ALL that a stored value may still hold (MeetingAgentCore.DIRECT).
    members = {name: value for name, value in vars(cls).items()
               if name.isupper() and isinstance(value, (str, int))
               and not isinstance(value, bool)}
    if "values" in record:
        legacy = [v for v in members.values()
                  if v not in record["values"] and v not in record.get("installed", ())]
        if legacy:
            record["legacy"] = legacy
    return record


def render_enums(settings_module, commit: str) -> str:
    document = {
        "about": "Generated by tools/gen/settings_keys.py from OpenWhisper's services/settings.py "
                 f"at {commit[:12]}. Do not edit.",
        "enums": {name: enum_record(getattr(settings_module, name)) for name in ENUM_CLASSES},
        "constants": {},
    }
    for name in CONSTANTS:
        value = getattr(settings_module, name)
        document["constants"][name] = list(value) if isinstance(value, tuple) else value
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def generate() -> tuple[str, str]:
    if sys.platform != "win32" or platform.machine().upper() not in ("AMD64", "X86_64"):
        raise GeneratorError("run it on Windows x64: the defaults are config evaluated there")
    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    from config import config
    from services import setting_types as types
    from services import settings as settings_module

    if config.DEFAULT_BACKEND != "parakeet":
        raise GeneratorError(f"DEFAULT_BACKEND is {config.DEFAULT_BACKEND!r}, not parakeet")
    keys = [str(s.key) for s in settings_module.ALL_SETTINGS]
    missing = [k for k in DROPPED_KEYS if k not in keys]
    if missing:
        raise GeneratorError(f"dropped keys no longer declared: {missing}")
    declarations = [declare(s, settings_module, types, config, dropped=str(s.key) in DROPPED_KEYS)
                    for s in settings_module.ALL_SETTINGS]
    for setting, note in fv_settings(types):
        if str(setting.key) in keys:
            raise GeneratorError(f"{setting.key} is already an OpenWhisper key")
        declarations.append(declare(setting, settings_module, types, config, dropped=False,
                                    fv_note=note))
    legacy = tuple(str(k) for k in settings_module.LEGACY_STREAMING_KEYS)
    return render_keys(declarations, legacy, commit), render_enums(settings_module, commit)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed files instead of writing them")
    args = parser.parse_args()
    try:
        keys_text, enums_text = generate()
    except (GeneratorError, ow_isolation.IsolationError) as exc:
        print(f"settings_keys: {exc}", file=sys.stderr)
        return 2
    outputs = ((KEYS_INC, keys_text), (ENUMS_JSON, enums_text))
    if args.check:
        stale = [path for path, text in outputs
                 if not path.is_file() or path.read_bytes() != text.encode("utf-8")]
        for path in stale:
            print(f"{path.relative_to(ROOT).as_posix()} is out of date", file=sys.stderr)
        return 1 if stale else 0
    for path, text in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        print(f"wrote {path.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
