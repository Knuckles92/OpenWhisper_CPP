"""Write golden vectors from OpenWhisper's pure functions, for C++ replay with fv_golden.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/golden_vectors.py --module services.format_utils
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/golden_vectors.py --all

Each module has a case file tools/parity/cases/<python.module>.py (one per module, so packages
never edit the same file) exporting

    CASES = {"function": [[arg, ...], ...]}           curated argument lists
    WRAPPERS = {"function": callable(module, *args)}  optional: methods needing a stub self
    TRACE = {"tests": ["tests/test_x.py"], "functions": ["function", ...]}   optional
    PINS = {...}                                      optional, added to the file's pins
    FLOAT_TOLERANCE = 1e-6                            optional, absolute, for the C++ replay

Curated inputs are ordinary values plus the edges where a literal C++ translation differs from
Python (half-to-even round, code points versus UTF-8 bytes, casefold, Unicode \\w). Traced inputs
are the calls OpenWhisper's own tests make (trace_plugin.py, run under pytest in a child
process); only their arguments are kept, and every result is computed here, in one isolated
process, so a test's monkeypatching never reaches a vector. Output: tests/golden/<module>.json,
in the format golden_format.py describes. Rerunning at the same commit gives no diff.
"""
from __future__ import annotations

import argparse
import importlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import golden_format  # noqa: E402
import ow_isolation  # noqa: E402

CASES_DIR = TOOLS / "cases"
GOLDEN_DIR = ow_isolation.REPO / "tests" / "golden"


def load_cases(module_name: str):
    path = CASES_DIR / f"{module_name}.py"
    if not path.is_file():
        raise SystemExit(f"No case file {path.relative_to(ow_isolation.REPO)}.")
    spec = importlib.util.spec_from_file_location(f"cases_{module_name.replace('.', '_')}", path)
    cases = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cases)
    return cases


def all_modules() -> list[str]:
    return sorted(p.stem for p in CASES_DIR.glob("*.py") if not p.name.startswith("_"))


def run_trace(module_name: str, trace: dict, functions: list[str], checkout: Path) -> dict:
    """Run the module's OpenWhisper test files under trace_plugin; returns {function: [case]}."""
    scratch = ow_isolation.SCRATCH
    out = scratch / "trace" / f"{module_name}.json"
    if out.exists():
        out.unlink()
    env = {k: v for k, v in os.environ.items()
           if k.lower() not in ow_isolation.PROXY_VARIABLES and not k.startswith("PYTHON")}
    env.update(ow_isolation.scratch_environment(scratch))
    env.update({
        "PYTHONPATH": str(TOOLS),
        "PYTHONPYCACHEPREFIX": str(scratch / "pycache"),
        "QT_QPA_PLATFORM": "offscreen",
    })
    tests = [str(checkout / t) for t in trace["tests"]]
    # -B, not -I: the plugin is found through PYTHONPATH, which -I ignores. The working directory
    # is the scratch folder, so nothing there shadows a module and pytest's rootdir stays the
    # checkout (no cache: -p no:cacheprovider; temp folders: --basetemp).
    command = [sys.executable, "-B", "-m", "pytest", "-q", "-p", "trace_plugin",
               "-p", "no:cacheprovider", "--basetemp", str(scratch / "pytest-tmp"),
               "--trace-golden", f"{module_name}:{','.join(functions)}",
               "--trace-out", str(out), *tests]
    result = subprocess.run(command, cwd=scratch, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if result.returncode != 0 or not out.is_file():
        sys.stderr.write(result.stdout[-4000:] + result.stderr[-4000:])
        raise SystemExit(f"Tracing {module_name} failed (pytest exit {result.returncode}).")
    traced = json.loads(out.read_text(encoding="utf-8"))["functions"]
    return {key.rsplit(".", 1)[1]: cases for key, cases in traced.items()}


def call(fn, args, kwargs):
    case = {"args": golden_format.encode(list(args))}
    if kwargs:
        case["kwargs"] = golden_format.encode(kwargs)
    try:
        case["result"] = golden_format.encode(fn(*args, **kwargs))
    except Exception as exc:  # the exception type is part of the contract
        case["raises"] = type(exc).__name__
    return case


def build(module_name: str, cases_module, traced: dict, commit: str) -> dict:
    module = importlib.import_module(module_name)
    wrappers = getattr(cases_module, "WRAPPERS", {})
    reference = ow_isolation.reference()
    pins = {"python": sys.version.split()[0], "platform": sys.platform,
            "local_timezone": time.tzname[0]}
    pins.update(getattr(cases_module, "PINS", {}))
    record = {"module": module_name, "source_commit": commit, "pins": pins}
    if hasattr(cases_module, "FLOAT_TOLERANCE"):
        record["float_tolerance"] = cases_module.FLOAT_TOLERANCE
    trace = getattr(cases_module, "TRACE", None)
    if trace:
        record["traced_from"] = list(trace["tests"])
    if pins["python"] != reference["python"]:
        raise SystemExit(f"Python {pins['python']} is not the pinned {reference['python']}.")

    functions = {}
    names = list(cases_module.CASES)
    names += [n for n in traced if n not in names]
    for name in names:
        if name in wrappers:
            wrapper = wrappers[name]
            fn = lambda *a, _w=wrapper, **k: _w(module, *a, **k)  # noqa: E731
        else:
            fn = getattr(module, name)
        cases, seen = [], []
        for args in cases_module.CASES.get(name, []):
            case = call(fn, args, {})
            cases.append(case)
            seen.append((case["args"], case.get("kwargs")))
        skipped = 0
        for traced_case in traced.get(name, []):
            key = (traced_case["args"], traced_case.get("kwargs"))
            if key in seen:
                continue
            try:
                args = golden_format.decode(traced_case["args"])
                kwargs = golden_format.decode(traced_case.get("kwargs", {}))
            except golden_format.Unencodable:
                skipped += 1
                continue
            case = call(fn, args, kwargs)
            case["traced"] = True
            cases.append(case)
            seen.append(key)
        if skipped:
            print(f"  {name}: {skipped} traced calls with non-JSON arguments skipped")
        functions[name] = cases
    record["functions"] = functions
    return record


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--module", action="append", default=[],
                        help="python module whose cases/<module>.py to run (repeatable)")
    parser.add_argument("--all", action="store_true", help="every case file")
    parser.add_argument("--no-trace", action="store_true", help="curated cases only")
    parser.add_argument("--out", type=Path, default=GOLDEN_DIR)
    parser.add_argument("--allow-other-commit", action="store_true",
                        help="write vectors although the checkout is not at the pinned commit")
    args = parser.parse_args(argv)
    modules = all_modules() if args.all else args.module
    if not modules:
        parser.error("name --module or --all")

    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout, args.allow_other_commit)
    data = ow_isolation.isolate()
    case_modules = {name: load_cases(name) for name in modules}
    traced = {}
    for name, cases_module in case_modules.items():
        trace = getattr(cases_module, "TRACE", None)
        if trace and not args.no_trace:
            functions = trace.get("functions") or list(cases_module.CASES)
            traced[name] = run_trace(name, trace, functions, checkout)
    ow_isolation.after_import_guards(data)

    for name, cases_module in case_modules.items():
        record = build(name, cases_module, traced.get(name, {}), commit)
        path = args.out / f"{name}.json"
        golden_format.write(path, record)
        counts = {f: len(c) for f, c in record["functions"].items()}
        print(f"{path.name}: {sum(counts.values())} cases "
              + ", ".join(f"{f} {n}" for f, n in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
