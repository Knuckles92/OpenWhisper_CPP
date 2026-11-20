"""check_brand, check_style_props, check_qss_names and check_ownership against small trees."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_brand  # noqa: E402
import check_ownership  # noqa: E402
import check_qss_names  # noqa: E402
import check_style_props  # noqa: E402
import fvlint  # noqa: E402


def write(root: Path, files: dict[str, str]) -> None:
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")


class Tree(unittest.TestCase):
    def setUp(self):
        self._folder = tempfile.TemporaryDirectory()
        self.root = Path(self._folder.name)

    def tearDown(self):
        self._folder.cleanup()


class Lexer(unittest.TestCase):
    def test_strings_and_comments_are_told_apart(self):
        text = ('// "not a string"\nauto a = "one"; /* two */ auto b = R"x(th"ree)x";\n'
                "auto c = 1'000; auto d = '\"'; auto e = u8\"four\\\"five\";\n")
        tokens = [(kind, value) for kind, value, *_ in fvlint.cxx_tokens(text)]
        self.assertEqual(tokens, [("comment", '// "not a string"'), ("string", "one"),
                                  ("comment", "/* two */"), ("string", 'th"ree'),
                                  ("string", 'four\\"five')])


class Brand(Tree):
    def test_a_literal_naming_openwhisper_fails_and_a_comment_does_not(self):
        write(self.root, {
            "src/history/a.cpp": '// Ported from OpenWhisper.\nauto f = "openwhisper.db";\n',
            "src/base/identity.cpp": 'auto p = "OpenWhisper";\n',
            "src/remote/wire.h": 'auto path = "/openwhisper/v1/engine";\n',
            "src/backup/import_settings.cpp": 'auto e = "OPENWHISPER_DATA_DIR";\n',
        })
        problems, count = check_brand.check(self.root)
        self.assertEqual(count, 4)
        self.assertEqual(len(problems), 1)
        self.assertIn("src/history/a.cpp:2", problems[0])

    def test_an_include_of_a_header_named_after_it_passes(self):
        write(self.root, {
            "src/ui_setup/a.h": '#include "models/openwhisper_discovery.h"\n'
                                '  #  include "models/testing/fake_openwhisper.h"\n'
                                'auto name = "openwhisper";\n',
        })
        problems, _ = check_brand.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("src/ui_setup/a.h:3", problems[0])


class StyleProps(Tree):
    def test_raw_property_calls_and_undeclared_props_fail_in_widget_code(self):
        write(self.root, {
            "src/ui_theme/style_props.h": "namespace fv::ui::prop {\n"
                                          'inline constexpr const char* tone = "tone";\n}\n',
            "src/ui_theme/restyle.cpp": "void f(QWidget* w) { w->setProperty(name, v); }\n",
            "src/ui_record/tab.cpp": textwrap.dedent("""\
                void g(QWidget* w) {
                    setStyleProperty(w, prop::tone, "ok");
                    setStyleProperty(w, prop::colour, "red");
                    w->setStyleSheet("color: red");  // setProperty( in a comment is fine
                    w->setProperty("tone", "x");
                    setLocalStyleSheet(w, "@text");
                }
                """),
        })
        problems, count = check_style_props.check(self.root)
        self.assertEqual(count, 1)
        self.assertEqual(problems, [
            "src/ui_record/tab.cpp:3: prop::colour is not declared in ui_theme/style_props.h",
            "src/ui_record/tab.cpp:4: raw setStyleSheet; use fv::ui::setStyleProperty or "
            "fv::ui::setLocalStyleSheet",
            "src/ui_record/tab.cpp:5: raw setProperty; use fv::ui::setStyleProperty or "
            "fv::ui::setLocalStyleSheet",
        ])


class QssNames(Tree):
    def test_unused_names_fail_unless_allowlisted(self):
        write(self.root, {
            "src/ui_theme/resources/theme.qss": textwrap.dedent("""\
                /* #commented { } */
                QFrame#customTitleBar { background: #1f2937; }
                QPushButton[tone="danger"], #meetingPanel QLabel { color: #fff; }
                QLabel[starred] { }
                #unportedThing { }
                """),
            "src/ui_theme/style_props.h": 'inline constexpr const char* tone = "tone";\n',
            "src/ui_shell/title_bar.cpp": 'setObjectName(QStringLiteral(u"customTitleBar"));\n',
            "tools/lint/qss_unused.txt": "// comment\n#meetingPanel\n",
        })
        problems, count = check_qss_names.check(self.root)
        self.assertEqual(count, 1)
        self.assertEqual(len(problems), 2)
        self.assertIn("#unportedThing", problems[0])
        self.assertIn("[starred]", problems[1])

    def test_no_style_sheet_yet_passes(self):
        self.assertEqual(check_qss_names.check(self.root), ([], 0))


@unittest.skipUnless(shutil.which("git"), "git is not on PATH")
class Ownership(Tree):
    def git(self, *args: str) -> None:
        subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)

    def test_changes_outside_owned_paths_and_to_frozen_files_fail(self):
        write(self.root, {"src/settings/store.h": "", "CMakeLists.txt": "", "README.md": ""})
        self.git("init", "-q")
        self.git("-c", "user.email=t@example.invalid", "-c", "user.name=t", "add", "-A")
        self.git("-c", "user.email=t@example.invalid", "-c", "user.name=t", "commit", "-qm", "0")
        write(self.root, {
            "src/settings/store.cpp": "x",
            "tests/settings/test_store.cpp": "x",
            "src/base/clock.h": "x",
            "CMakeLists.txt": "x",
        })
        problems = check_ownership.check(self.root, "HEAD", ["src/settings/"])
        self.assertEqual(problems, [
            "CMakeLists.txt: frozen file changed (only a package naming it in its owned paths "
            "may change it)",
            "src/base/clock.h: changed outside the package's owned paths",
        ])
        self.assertEqual(
            check_ownership.check(self.root, "HEAD",
                                  ["src/settings/", "src/base/clock.h", "CMakeLists.txt"]), [])

    def test_a_package_owning_a_frozen_folder_may_change_its_files(self):
        write(self.root, {"cmake/OpenWhisper_CPPModules.cmake": "", "src/settings/keys.inc": ""})
        self.git("init", "-q")
        self.git("-c", "user.email=t@example.invalid", "-c", "user.name=t", "add", "-A")
        self.git("-c", "user.email=t@example.invalid", "-c", "user.name=t", "commit", "-qm", "0")
        write(self.root, {
            "cmake/OpenWhisper_CPPModules.cmake": "x",
            "cmake/pch/core.h": "x",
            "src/settings/keys.inc": "x",
        })
        # M0.1 owns cmake/ as a folder; owning src/settings/ does not reach the frozen keys.inc.
        self.assertEqual(check_ownership.check(self.root, "HEAD", ["cmake/", "src/settings/"]), [
            "src/settings/keys.inc: frozen file changed (only a package naming it in its owned "
            "paths may change it)",
        ])
        self.assertEqual(check_ownership.check(self.root, "HEAD", ["cmake/*"]), [
            "src/settings/keys.inc: frozen file changed (only a package naming it in its owned "
            "paths may change it)",
        ])
        self.assertEqual(
            check_ownership.check(self.root, "HEAD", ["cmake/", "src/settings/keys.inc"]), [])


if __name__ == "__main__":
    unittest.main()
