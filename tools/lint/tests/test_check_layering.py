"""check_layering.py against small trees with deliberate violations."""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_layering  # noqa: E402


def write(root: Path, files: dict[str, str]) -> None:
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")


CLEAN = {
    "src/base/CMakeLists.txt": "fv_module(KIND core THIRD_PARTY nlohmann_json::nlohmann_json)\n",
    "src/base/clock.h": "#pragma once\n#include <chrono>\n",
    "src/base/detail/impl.h": "#pragma once\n",
    "src/base/clock.cpp": '#include "base/clock.h"\n#include "base/detail/impl.h"\n',
    "src/settings/CMakeLists.txt": "fv_module(KIND core\n          DEPS fv_base)\n",
    "src/settings/store.h": '#pragma once\n#include "base/clock.h"\n',
    "src/history/CMakeLists.txt": "fv_module(KIND core DEPS fv_settings)  # base via settings\n",
    "src/history/manager.cpp": '#include "base/clock.h"\n#include "settings/store.h"\n',
    "src/ui_widgets/CMakeLists.txt": "fv_module(KIND ui DEPS fv_settings)\n",
    "src/ui_widgets/card.h": '#pragma once\n#include <QFrame>\n#include "settings/store.h"\n',
    "src/app/CMakeLists.txt": "qt_add_executable(openwhisper-cpp main.cpp)\n",
    "src/app/main.cpp": '#include "ui_widgets/card.h"\n',
}


class CheckLayering(unittest.TestCase):
    def check(self, extra: dict[str, str]) -> list[str]:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write(root, {**CLEAN, **extra})
            return check_layering.check(root)

    def test_a_tree_that_keeps_to_its_layers_passes(self):
        self.assertEqual(self.check({}), [])

    def test_a_core_module_that_includes_a_qt_header_fails(self):
        problems = self.check({"src/settings/qt_leak.cpp": "#include <QString>\n"})
        self.assertEqual(len(problems), 1)
        self.assertIn("src/settings/qt_leak.cpp:1", problems[0])
        self.assertIn("includes Qt header <QString>", problems[0])

    def test_a_core_module_that_includes_a_qt_module_path_fails(self):
        problems = self.check({"src/base/x.h": "#pragma once\n#include <QtCore/qobject.h>\n"})
        self.assertEqual(len(problems), 1)
        self.assertIn("<QtCore/qobject.h>", problems[0])

    def test_including_a_module_outside_the_deps_closure_fails(self):
        problems = self.check({"src/base/bad.cpp": '\n#include "settings/store.h"\n'})
        self.assertEqual(problems,
                         ["src/base/bad.cpp:2: includes settings/store.h but fv_base does not "
                          "depend on fv_settings"])

    def test_a_core_module_that_includes_a_widget_module_fails(self):
        problems = self.check({"src/history/view.cpp": '#include "ui_widgets/card.h"\n'})
        self.assertEqual(len(problems), 1)
        self.assertIn("fv_history does not depend on fv_ui_widgets", problems[0])

    def test_a_core_module_that_depends_on_a_widget_module_fails(self):
        problems = self.check({
            "src/history/CMakeLists.txt": "fv_module(KIND core DEPS fv_settings fv_ui_widgets)\n",
        })
        self.assertIn("src/history (core) may not depend on fv_ui_widgets (ui)", problems)

    def test_including_another_modules_detail_header_fails(self):
        problems = self.check({"src/settings/peek.cpp": '#include "base/detail/impl.h"\n'})
        self.assertEqual(len(problems), 1)
        self.assertIn("a detail header of fv_base", problems[0])

    def test_deps_naming_something_that_is_not_a_module_fails(self):
        problems = self.check({"src/history/CMakeLists.txt": "fv_module(KIND core DEPS fv_nope)\n"})
        self.assertIn("src/history: DEPS names fv_nope, which is not an fv_module", problems)

    def test_the_real_tree_passes(self):
        root = Path(__file__).resolve().parents[3]
        self.assertEqual(check_layering.check(root), [])


if __name__ == "__main__":
    unittest.main()
