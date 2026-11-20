"""Helpers shared by the tools/lint checks: the source tree, fv_module declarations and a small
C++ lexer. Python 3.12 standard library only."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

CXX_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inc"}
KINDS = ("core", "qt", "ui", "feature", "exe")

# Kinds a module of each kind may depend on (architecture 2.2).
ALLOWED_DEP_KINDS = {
    "core": {"core"},
    "qt": {"core", "qt"},
    "ui": {"core", "qt", "ui", "feature"},
    "feature": {"core", "qt", "ui", "feature"},
    "exe": set(KINDS),
}


@dataclass
class Module:
    """One src/<dir> and what its fv_module call declares."""

    dir: str
    kind: str | None  # None: the directory does not call fv_module (legacy CMake)
    deps: list[str] = field(default_factory=list)  # target names, such as fv_base
    qt: list[str] = field(default_factory=list)

    @property
    def target(self) -> str:
        return f"fv_{self.dir}"


def repo_root(start: Path | None = None) -> Path:
    """The checkout holding this script (tools/lint/<script>)."""
    return (start or Path(__file__)).resolve().parents[2]


def strip_cmake_comments(text: str) -> str:
    return "\n".join(re.sub(r"(?<!\\)#.*", "", line) for line in text.splitlines())


def find_call(text: str, name: str) -> str | None:
    """The argument text of the first `name(...)` call, parentheses balanced."""
    match = re.search(rf"\b{re.escape(name)}\s*\(", text)
    if not match:
        return None
    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[match.end() : index]
    return None


KEYWORDS = {
    "KIND", "NAME", "GUI", "FEATURES", "DEPS", "THIRD_PARTY", "PRIVATE_THIRD_PARTY",
    "WIN32_LIBS", "QT",
}


def parse_fv_module(cmake_text: str) -> tuple[str | None, list[str], list[str]]:
    """(kind, deps, qt components) of the fv_module call in a CMakeLists.txt."""
    args_text = find_call(strip_cmake_comments(cmake_text), "fv_module")
    if args_text is None:
        return None, [], []
    tokens = args_text.split()
    values: dict[str, list[str]] = {}
    current = None
    for token in tokens:
        if token in KEYWORDS:
            current = token
            values.setdefault(current, [])
        elif current is not None and "${" not in token:
            values[current].append(token.strip('"'))
    kind = (values.get("KIND") or [None])[0]
    return kind, values.get("DEPS", []), values.get("QT", [])


def load_modules(src: Path) -> dict[str, Module]:
    """Every src/<dir> with a CMakeLists.txt, keyed by directory name."""
    modules: dict[str, Module] = {}
    for cmake in sorted(src.glob("*/CMakeLists.txt")):
        kind, deps, qt = parse_fv_module(cmake.read_text(encoding="utf-8"))
        modules[cmake.parent.name] = Module(cmake.parent.name, kind, deps, qt)
    return modules


def closure(modules: dict[str, Module], dir_name: str) -> set[str]:
    """Directory names the module may include from: itself and its DEPS, transitively."""
    seen = {dir_name}
    stack = [dir_name]
    while stack:
        module = modules.get(stack.pop())
        if module is None:
            continue
        for dep in module.deps:
            if dep.startswith("fv_") and dep[3:] not in seen:
                seen.add(dep[3:])
                stack.append(dep[3:])
    return seen


def source_files(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in CXX_SUFFIXES)


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.MULTILINE)


def includes(text: str) -> list[tuple[int, str, str]]:
    """(line number, delimiter, path) of every #include."""
    found = []
    for match in INCLUDE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        found.append((line, match.group(1), match.group(2)))
    return found


def cxx_tokens(text: str):
    """Yields (kind, value, line, start, end) for the string literals and comments of C++
    source: kind is "string" or "comment", [start, end) the whole token in `text`. Handles raw
    strings, prefixes (u8, u, U, L) and escapes."""
    index = 0
    line = 1
    length = len(text)
    while index < length:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
        elif text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end == -1 else end
            yield "comment", text[index:end], line, index, end
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end == -1 else end + 2
            yield "comment", text[index:end], line, index, end
            line += text.count("\n", index, end)
            index = end
        elif char == "'" and not (index > 0 and (text[index - 1].isalnum())):
            end = index + 1
            while end < length and text[end] != "'":
                end += 2 if text[end] == "\\" else 1
            index = end + 1
        elif char == '"':
            raw = re.search(r'(?:u8|u|U|L)?R$', text[max(0, index - 3) : index])
            if raw:
                delimiter_end = text.find("(", index)
                delimiter = text[index + 1 : delimiter_end]
                close = text.find(")" + delimiter + '"', delimiter_end)
                close = length if close == -1 else close
                value = text[delimiter_end + 1 : close]
                end = min(length, close + len(delimiter) + 2)
                yield "string", value, line, index, end
                line += value.count("\n")
                index = end
            else:
                end = index + 1
                while end < length and text[end] != '"' and text[end] != "\n":
                    end += 2 if text[end] == "\\" else 1
                yield "string", text[index + 1 : end], line, index, min(length, end + 1)
                index = end + 1
        else:
            index += 1


def report(name: str, problems: list[str], checked: str) -> int:
    if problems:
        print(f"{name}: {len(problems)} problem(s)")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(f"{name}: ok ({checked})")
    return 0
