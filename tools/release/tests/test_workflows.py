"""The GitHub workflows (.github/workflows): they parse, start only by hand, qualify a release
candidate through CI and its checksums, and publish nothing. The one write they hold is
qualify-release.yml's ``upload`` job: ``gh release upload`` to an existing draft, behind
``if: vars.FIORIVOCE_RELEASE_REPO != ''`` (M10.17, lead decision 8); written, never run.

Python's standard library has no YAML reader, so ``load_yaml`` reads the block subset the
workflows are written in (mappings, sequences, plain and quoted scalars, flow sequences of
scalars, ``|`` and ``>`` block scalars). Every scalar stays a string, like PyYAML's BaseLoader,
which OpenWhisper's test used. ``actionlint`` also checks the files when it is installed.
"""

import re
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO / ".github" / "workflows"


class YamlError(ValueError):
    pass


_KEY = re.compile(r"""^(?P<key>"[^"]*"|'[^']*'|[^\s"'#\-\[{][^:#]*?|-[^\s:#][^:#]*?)\s*:(?:\s+|$)""")


def _strip_comment(text: str) -> str:
    """``text`` without a trailing `` # comment`` outside quotes."""
    quote = None
    for index, char in enumerate(text):
        if quote:
            if char == quote:
                quote = None
        elif char in "'\"":
            quote = char
        elif char == "#" and (index == 0 or text[index - 1] in " \t"):
            return text[:index].rstrip()
    return text.rstrip()


def _scalar(text: str):
    text = _strip_comment(text).strip()
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return bytes(text[1:-1], "utf-8").decode("unicode_escape")
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        return [_scalar(part) for part in inner.split(",")] if inner else []
    if text.startswith(("{", "&", "*", "!")) or text in ("|", ">"):
        raise YamlError(f"unsupported YAML: {text}")
    return text


def load_yaml(source: str):
    lines = source.splitlines()

    def content(index: int) -> int:
        """The index of the next line that is not blank or a comment."""
        while index < len(lines):
            stripped = lines[index].strip()
            if stripped and not stripped.startswith("#"):
                return index
            index += 1
        return index

    def indent_of(index: int) -> int:
        line = lines[index]
        if "\t" in line[: len(line) - len(line.lstrip())]:
            raise YamlError(f"line {index + 1}: tab indentation")
        return len(line) - len(line.lstrip(" "))

    def block_scalar(index: int, parent_indent: int, style: str):
        """A ``|`` or ``>`` scalar starting after line ``index - 1``."""
        body, indent = [], None
        while index < len(lines):
            line = lines[index]
            if line.strip():
                width = len(line) - len(line.lstrip(" "))
                if width <= parent_indent:
                    break
                indent = width if indent is None else indent
                body.append(line[indent:])
            else:
                body.append("")
            index += 1
        while body and not body[-1]:
            body.pop()
        if style.startswith(">"):
            text = ""
            for line in body:
                text += ("\n" if not line else (" " if text and not text.endswith("\n") else "")
                         + line)
        else:
            text = "\n".join(body)
        if not style.endswith("-"):
            text += "\n"
        return text, index

    def value_after(rest: str, index: int, indent: int):
        """The value of ``key: rest`` on line ``index - 1``."""
        rest = _strip_comment(rest).strip()
        if rest in ("|", "|-", ">", ">-"):
            return block_scalar(index, indent, rest)
        if rest:
            return _scalar(rest), index
        nxt = content(index)
        if nxt < len(lines):
            child = indent_of(nxt)
            if child > indent or (child == indent and lines[nxt].lstrip().startswith("- ")):
                return node(nxt, child)
        return "", index

    def node(index: int, indent: int, first=None):
        """The mapping or sequence at ``indent`` from line ``index``. ``first`` is the text of
        that line after a sequence dash, standing in for the line itself."""
        text = first if first is not None else lines[index].strip()
        if text == "-" or text.startswith("- "):
            return sequence(index, indent, first)
        return mapping(index, indent, first)

    def sequence(index: int, indent: int, first=None):
        items = []
        while True:
            text = first if first is not None else lines[index].strip()
            first = None
            if not (text == "-" or text.startswith("- ")):
                raise YamlError(f"line {index + 1}: expected a sequence item")
            item = text[1:].lstrip()
            # The item's own indentation: its text starts after the dash and its spaces.
            item_indent = indent + (len(text) - len(item))
            if not item:
                nxt = content(index + 1)
                value, index = node(nxt, indent_of(nxt))
            elif _KEY.match(item) or item == "-" or item.startswith("- "):
                value, index = node(index, item_indent, item)
            else:
                value, index = _scalar(item), index + 1
            items.append(value)
            index = content(index)
            if index >= len(lines) or indent_of(index) != indent or \
                    not lines[index].lstrip().startswith("-"):
                return items, index

    def mapping(index: int, indent: int, first=None):
        result = {}
        while True:
            text = first if first is not None else lines[index].strip()
            first = None
            match = _KEY.match(text)
            if not match:
                raise YamlError(f"line {index + 1}: expected 'key:' in {text!r}")
            key = match.group("key").strip()
            if key[0] in "'\"":
                key = key[1:-1]
            if key in result:
                raise YamlError(f"line {index + 1}: duplicate key {key}")
            result[key], index = value_after(text[match.end():], index + 1, indent)
            index = content(index)
            if index >= len(lines):
                return result, index
            width = indent_of(index)
            if width < indent:
                return result, index
            if width > indent:
                raise YamlError(f"line {index + 1}: unexpected indentation")
            if lines[index].lstrip().startswith("- "):
                return result, index

    start = content(0)
    if start >= len(lines):
        return None
    document, end = node(start, indent_of(start))
    end = content(end)
    if end < len(lines):
        raise YamlError(f"line {end + 1}: text after the document")
    return document


def read(name: str) -> dict:
    return load_yaml((WORKFLOWS / name).read_text(encoding="utf-8"))


def steps_of(workflow: dict):
    for job_id, job in workflow["jobs"].items():
        for step in job.get("steps", []):
            yield job_id, step


class YamlReader(unittest.TestCase):
    def test_reads_the_constructs_the_workflows_use(self):
        document = load_yaml(
            "name: CI  # a comment\n"
            "on:\n"
            "  workflow_dispatch:\n"
            "    inputs:\n"
            "      ref:\n"
            "        required: false\n"
            "jobs:\n"
            "  build:\n"
            "    needs: [source, qualification]\n"
            "    steps:\n"
            "      - uses: actions/checkout@abc # v4\n"
            "        with:\n"
            "          ref: ${{ needs.source.outputs.commit }}\n"
            "      - name: 'It''s quoted'\n"
            "        run: |\n"
            "          echo one\n"
            "\n"
            "          echo '# not a comment'\n"
            "      - run: >-\n"
            "          folded\n"
            "          line\n"
            "      - plain item\n"
            "    list:\n"
            "    - a\n"
            "    - \"b\"\n")
        self.assertEqual(document["name"], "CI")
        self.assertEqual(document["on"]["workflow_dispatch"]["inputs"]["ref"]["required"],
                         "false")
        job = document["jobs"]["build"]
        self.assertEqual(job["needs"], ["source", "qualification"])
        self.assertEqual(job["steps"][0], {"uses": "actions/checkout@abc",
                                           "with": {"ref": "${{ needs.source.outputs.commit }}"}})
        self.assertEqual(job["steps"][1]["name"], "It's quoted")
        self.assertEqual(job["steps"][1]["run"], "echo one\n\necho '# not a comment'\n")
        self.assertEqual(job["steps"][2]["run"], "folded line")
        self.assertEqual(job["steps"][3], "plain item")
        self.assertEqual(job["list"], ["a", "b"])

    def test_refuses_what_it_cannot_read(self):
        for text in ("a: 1\n  b: 2\n", "a: 1\na: 2\n", "a: &anchor 1\n", "a:\n\t- b\n"):
            with self.assertRaises(YamlError, msg=text):
                load_yaml(text)


RELEASE_GUARD = "vars.FIORIVOCE_RELEASE_REPO != ''"
# The only place a workflow may write to a release: this job of this workflow.
UPLOAD_WORKFLOW, UPLOAD_JOB = "qualify-release.yml", "upload"
_PINNED = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")
_USES_LINE = re.compile(r"^\s*(?:-\s+)?uses:\s*(\S+)(.*)$")
# By tag, as M3.11 wrote them (open-items "CI workflows" records the risk); no other exception.
TAG_PINNED = {("ci.yml", "actions/cache@v4"), ("release-build.yml", "actions/cache@v4")}


def uses_lines(name: str):
    """(action, rest of the line) of every ``uses:`` line of a workflow file."""
    for line in (WORKFLOWS / name).read_text(encoding="utf-8").splitlines():
        match = _USES_LINE.match(line)
        if match:
            yield match.group(1), match.group(2)


class Workflows(unittest.TestCase):
    def setUp(self):
        self.ci = read("ci.yml")
        self.release = read("release-build.yml")
        self.qualify = read("qualify-release.yml")

    def test_only_the_three_workflows_exist(self):
        self.assertEqual(sorted(p.name for p in WORKFLOWS.iterdir()),
                         ["ci.yml", "qualify-release.yml", "release-build.yml"])

    def test_workflows_start_only_by_hand(self):
        # Pushes, pull requests and schedules spend the owner's runner minutes: enabling them is
        # the owner's decision. ci.yml is also callable by release-build.yml.
        self.assertEqual(set(self.ci["on"]), {"workflow_dispatch", "workflow_call"})
        self.assertEqual(set(self.release["on"]), {"workflow_dispatch"})
        self.assertEqual(set(self.qualify["on"]), {"workflow_dispatch"})
        for workflow in (self.ci, self.release, self.qualify):
            self.assertEqual(workflow["permissions"], {"contents": "read"})
        # Whatever workflow is added later: no push, pull request, schedule or other trigger.
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            triggers = load_yaml(path.read_text(encoding="utf-8"))["on"]
            self.assertLessEqual(set(triggers), {"workflow_dispatch", "workflow_call"}, path.name)

    def test_actions_are_pinned_to_a_commit_as_the_existing_workflows_pin_them(self):
        """owner/repo@<40-hex commit> # vN, one commit per action across the workflows; local
        reusable workflows only by path. The tag-pinned exceptions are listed, not grown."""
        commits = {}
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            for action, rest in uses_lines(path.name):
                if action.startswith("./.github/workflows/"):
                    continue
                if (path.name, action) in TAG_PINNED:
                    continue
                self.assertRegex(action, _PINNED, f"{path.name}: {action}")
                self.assertRegex(rest, r"^\s+#\s+v\d+", f"{path.name}: {action} has no version")
                name, commit = action.split("@")
                self.assertEqual(commits.setdefault(name, commit), commit,
                                 f"{path.name}: {name} pinned to two commits")
        self.assertFalse(any(a.endswith("@v4") for a, _ in uses_lines(UPLOAD_WORKFLOW)))

    def test_release_workflows_require_ci_and_checksums_without_manual_report(self):
        build = self.release["jobs"]
        self.assertEqual(build["qualification"]["uses"], "./.github/workflows/ci.yml")
        self.assertEqual(build["qualification"]["with"]["source_ref"],
                         "${{ needs.source.outputs.commit }}")
        self.assertIn("qualification", build["windows"]["needs"])
        checkout = next(s for s in build["windows"]["steps"]
                        if s.get("uses", "").startswith("actions/checkout@"))
        self.assertEqual(checkout["with"]["ref"], "${{ needs.source.outputs.commit }}")
        self.assertNotIn("upload-draft", build)
        steps = build["windows"]["steps"]
        commands = [s.get("run", "") for s in steps]
        candidate = next(i for i, c in enumerate(commands)
                         if "build-installer.ps1" in c and "-SourceQualification passed" in c)
        validate = next(i for i, c in enumerate(commands)
                        if "check_release_qualification.py" in c)
        upload = next(i for i, s in enumerate(steps)
                      if s.get("uses", "").startswith("actions/upload-artifact@")
                      and s["with"]["path"].startswith("out/installer"))
        self.assertIn("-RequireQtLicenses", commands[candidate])
        self.assertLess(candidate, validate)
        self.assertLess(validate, upload)
        for flag in ("--require-source-qualification", "--expected-commit", "--expected-run"):
            self.assertIn(flag, commands[validate])
        self.assertIn("${{ needs.source.outputs.commit }}", steps[validate]["env"].values())
        self.assertEqual(steps[upload]["with"]["if-no-files-found"], "error")
        # The upgrade smoke runs on the candidate before it is kept.
        smoke = next(i for i, c in enumerate(commands) if "smoke-setup.ps1" in c)
        self.assertLess(candidate, smoke)
        self.assertLess(smoke, upload)
        for workflow in (self.ci, self.release):
            for job_id, step in steps_of(workflow):
                self.assertNotIn("continue-on-error", step, job_id)

    def test_release_upload_is_only_in_the_guarded_manual_job(self):
        """gh release upload appears once in all the workflows: in qualify-release.yml's upload
        job, which runs only by hand, only when FIORIVOCE_RELEASE_REPO is set, after the
        qualify job, and between the draft's two validations."""
        found = []
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            workflow = load_yaml(path.read_text(encoding="utf-8"))
            for job_id, job in workflow["jobs"].items():
                for step in job.get("steps", []):
                    if re.search(r"\bgh\s+release\s+upload\b", step.get("run", "")):
                        found.append((path.name, job_id))
        self.assertEqual(found, [(UPLOAD_WORKFLOW, UPLOAD_JOB)])
        self.assertEqual(set(self.qualify["on"]), {"workflow_dispatch"})
        jobs = self.qualify["jobs"]
        self.assertEqual(set(jobs), {"qualify", UPLOAD_JOB})
        for job_id, job in jobs.items():
            self.assertEqual(job["if"], RELEASE_GUARD, job_id)
            self.assertNotIn("continue-on-error", job, job_id)
        self.assertEqual(jobs[UPLOAD_JOB]["needs"], "qualify")
        self.assertEqual(jobs["qualify"]["permissions"], {"actions": "read", "contents": "read"})
        self.assertEqual(jobs[UPLOAD_JOB]["permissions"], {"actions": "read", "contents": "write"})
        run = "\n".join(s["run"] for s in jobs[UPLOAD_JOB]["steps"] if "run" in s)
        upload = run.index("gh release upload")
        self.assertEqual(run.count("gh release upload"), 1)
        self.assertIn("--clobber", run[upload:].splitlines()[0])
        self.assertLess(run.index("--json isDraft --jq .isDraft)\" = true"), upload)
        self.assertLess(run.index("test \"$RELEASE_TAG\" = \"v$version\""), upload)
        self.assertLess(run.index("test \"$(git rev-parse 'FETCH_HEAD^{commit}')\""), upload)
        self.assertLess(run.index("--draft-assets draft-assets.json"), upload)
        self.assertGreater(run.index("--draft-assets uploaded-release.json --uploaded"), upload)
        self.assertTrue(run.startswith("set -euo pipefail"))
        # Both jobs check the candidate against the tag's commit and the run it names.
        for job_id in jobs:
            text = "\n".join(s.get("run", "") for s in jobs[job_id]["steps"])
            for flag in ("check_release_qualification.py --dir release",
                         "--require-source-qualification",
                         '--expected-commit "$(git rev-parse HEAD)"',
                         '--expected-run "$CANDIDATE_RUN"'):
                self.assertIn(flag, text, job_id)
        provenance = "\n".join(s.get("run", "") for s in jobs["qualify"]["steps"])
        self.assertIn('run["path"] == ".github/workflows/release-build.yml"', provenance)
        self.assertIn('test "$RELEASE_REPO" = "$REPOSITORY"', provenance)
        inputs = self.qualify["on"]["workflow_dispatch"]["inputs"]
        self.assertEqual(set(inputs), {"candidate_run_id", "release_tag", "require_windows_signed"})

    def test_qualify_release_writes_nothing_but_the_draft_upload(self):
        allowed = ("actions/checkout@", "actions/setup-python@", "actions/download-artifact@")
        for job_id, job in self.qualify["jobs"].items():
            for step in job.get("steps", []):
                if "uses" in step:
                    self.assertTrue(step["uses"].startswith(allowed), step["uses"])
                command = step.get("run", "")
                for forbidden in (r"\bgit\s+push\b", r"\bgit\s+tag\b", r"--upload-file",
                                  r"\bcurl\b", r"\bwget\b", r"Invoke-RestMethod",
                                  r"Invoke-WebRequest", r"\bnpm\s+publish\b",
                                  r"\bactions/upload-artifact\b"):
                    self.assertIsNone(re.search(forbidden, command),
                                      f"{job_id}: {forbidden} in {command!r}")
                for line in command.splitlines():
                    for call in re.finditer(r"\bgh\s+(\S+)(?:\s+(\S+))?(.*)", line):
                        verb = (call.group(1), call.group(2))
                        if verb == ("release", "upload"):
                            self.assertEqual(job_id, UPLOAD_JOB)
                        elif verb[0] == "release":
                            self.assertEqual(verb[1], "view", line)
                        else:
                            # gh api reads only: no method, field or input flags.
                            self.assertEqual(verb[0], "api", line)
                            arguments = " " + (call.group(2) or "") + call.group(3)
                            self.assertIsNone(re.search(r"\s(-X|--method|-f|-F|--field|"
                                                        r"--raw-field|--input)\b",
                                                        arguments), line)

    def test_no_step_publishes_pushes_or_uploads_outside_the_run(self):
        allowed = ("actions/checkout@", "actions/setup-python@", "actions/cache@",
                   "actions/upload-artifact@")
        for workflow in (self.ci, self.release):
            for job_id, job in workflow["jobs"].items():
                if "uses" in job:
                    self.assertEqual(job["uses"], "./.github/workflows/ci.yml")
                for step in job.get("steps", []):
                    if "uses" in step:
                        self.assertTrue(step["uses"].startswith(allowed), step["uses"])
                    command = step.get("run", "")
                    for forbidden in (r"\bgh\s", r"\bgit\s+push\b", r"\bgit\s+tag\b",
                                      r"--upload-file", r"\bcurl\b.*\s-T\s", r"Invoke-RestMethod",
                                      r"Invoke-WebRequest", r"\bnpm\s+publish\b"):
                        self.assertIsNone(re.search(forbidden, command),
                                          f"{job_id}: {forbidden} in {command!r}")
                    self.assertNotIn("GH_TOKEN", step.get("env", {}))
                    self.assertNotIn("github.token", str(step))

    def test_ci_runs_the_merge_gate_presets(self):
        jobs = self.ci["jobs"]

        def commands(job):
            return "\n".join(s.get("run", "") for s in jobs[job]["steps"])

        self.assertIn("cmake --workflow --preset windows-ci", commands("windows"))
        self.assertEqual(jobs["windows"]["runs-on"], "windows-2022")
        self.assertIn("cmake --workflow --preset linux-core", commands("linux-core"))
        lint = commands("lint")
        self.assertIn("clang-format-18 --dry-run -Werror", lint)
        self.assertIn("parity_report.py --check", lint)
        self.assertIn("unittest discover -s tools/release/tests", lint)
        for check in ("layering", "ownership", "qss_names", "brand", "style_props"):
            self.assertIn(check, lint)
        # A pinned Qt kit and vcpkg's files binary cache at the manifest's baseline.
        self.assertEqual(self.ci["env"]["QT_VERSION"], "6.10.3")
        baseline = re.search(r'"builtin-baseline":\s*"([0-9a-f]{40})"',
                             (REPO / "vcpkg.json").read_text(encoding="utf-8")).group(1)
        self.assertEqual(self.ci["env"]["VCPKG_COMMIT"], baseline)
        self.assertEqual(self.release["env"]["VCPKG_COMMIT"], baseline)
        self.assertTrue(jobs["windows"]["env"]["VCPKG_BINARY_SOURCES"].startswith("clear;files,"))
        # Each job checks out the commit it was asked to test.
        for job in jobs.values():
            checkout = next(s for s in job["steps"]
                            if s.get("uses", "").startswith("actions/checkout@"))
            self.assertEqual(checkout["with"]["ref"], "${{ inputs.source_ref || github.sha }}")

    @unittest.skipUnless(shutil.which("actionlint"), "actionlint is not installed")
    def test_actionlint_accepts_the_workflows(self):
        completed = subprocess.run(
            ["actionlint", "-no-color", *(str(p) for p in sorted(WORKFLOWS.glob("*.yml")))],
            capture_output=True, text=True, check=False, cwd=REPO)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)


if __name__ == "__main__":
    unittest.main()
