"""generate_notices.py over a fake vcpkg_installed tree and a fake Qt kit."""

import contextlib
import io
import json
import re
import unittest
from pathlib import Path

import release_support as rs

import generate_notices as gn

TRIPLET = "x64-windows"


class FakeTrees(rs.TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.vcpkg = self.tmp / "vcpkg_installed"
        self.out = self.tmp / "stage"
        (self.vcpkg / "vcpkg" / "info").mkdir(parents=True)
        (self.vcpkg / TRIPLET / "share").mkdir(parents=True)

    def add_port(self, name, version, licence="Licence of {name}.\n", files=None, triplet=TRIPLET):
        """Install a fake port: its list file, and a copyright file unless ``licence`` is None."""
        files = files if files is not None else [f"{triplet}/include/{name}.h"]
        listing = [f"{triplet}/", f"{triplet}/share/", f"{triplet}/share/{name}/", *files]
        (self.vcpkg / "vcpkg" / "info" / f"{name}_{version}_{triplet}.list").write_text(
            "\n".join(listing) + "\n", encoding="utf-8")
        if licence is not None:
            folder = self.vcpkg / triplet / "share" / name
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "copyright").write_text(licence.format(name=name), encoding="utf-8")

    def add_qt(self, *names):
        kit = self.tmp / "Qt" / "6.10.3" / "msvc2022_64"
        kit.mkdir(parents=True, exist_ok=True)
        for name in names:
            folder = self.tmp / "Qt" / "Licenses"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / name).write_text(f"Text of {name}\n", encoding="utf-8")
        return kit

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gn.main(["--vcpkg-installed", str(self.vcpkg), "--triplet", TRIPLET,
                            "--out", str(self.out), *args])
        return code, out.getvalue(), err.getvalue()


class GenerateNotices(FakeTrees):
    def test_every_port_is_listed_with_its_licence_text(self):
        self.add_port("zlib", "1.4", "zlib licence text for {name}\n")
        self.add_port("boost-asio", "1.92.0")
        self.add_port("pugixml", "1.16")
        self.add_port("ffmpeg", "9.0.2", "LGPL text, ffmpeg.\n")
        kit = self.add_qt("LICENSE.LGPL3", "LICENSE.GPL3", "notes.txt")
        code, out, err = self.run_tool("--qt", str(kit))
        self.assertEqual((code, err), (0, ""))
        self.assertIn("4 vcpkg licence texts, 2 Qt licence files", out)
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for name, version in (("boost-asio", "1.92.0"), ("ffmpeg", "9.0.2"), ("pugixml", "1.16"),
                              ("zlib", "1.4")):
            self.assertIn(f"| {name} | {version} | [copyright](licenses/vcpkg/{name}/copyright) |",
                          notices)
            copied = self.out / "licenses" / "vcpkg" / name / "copyright"
            self.assertEqual(copied.read_text(encoding="utf-8"),
                             (self.vcpkg / TRIPLET / "share" / name / "copyright").read_text(
                                 encoding="utf-8"))
        self.assertIn("licenses/qt/LICENSE.LGPL3", notices)
        self.assertEqual(sorted(p.name for p in (self.out / "licenses" / "qt").iterdir()),
                         ["LICENSE.GPL3", "LICENSE.LGPL3"])

    def test_the_notices_carry_the_hand_written_fragments(self):
        self.add_port("zlib", "1.4")
        self.run_tool()
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for heading in ("## Qt 6", "## Tabler Icons", "## Speech runtime and models",
                        "LGPL-3.0", "MIT License", "CC-BY-4.0", "NeMo-Speech.cpp"):
            self.assertIn(heading, notices)
        self.assertTrue(notices.startswith("# Third-party notices\n"))
        self.assertNotIn("\r", notices)
        # Fragments keep their file-name order, so the output is reproducible.
        self.assertLess(notices.index("## Qt 6"), notices.index("## Tabler Icons"))
        self.assertLess(notices.index("## Tabler Icons"), notices.index("## Speech runtime"))

    def test_a_port_without_a_copyright_file_fails(self):
        self.add_port("zlib", "1.4")
        self.add_port("nolicence", "2.0", licence=None)
        code, out, err = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("nolicence 2.0 has no copyright file", err)
        self.assertFalse((self.out / "THIRD_PARTY_NOTICES.md").exists())

    def test_a_port_that_installs_no_code_may_lack_one(self):
        self.add_port("zlib", "1.4")
        self.add_port("boost-uninstall", "1.92.0", licence=None, files=[
            f"{TRIPLET}/share/boost-uninstall/vcpkg.spdx.json",
            f"{TRIPLET}/share/boost/vcpkg-cmake-wrapper.cmake"])
        code, _, _ = self.run_tool()
        self.assertEqual(code, 0)
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn("| boost-uninstall | 1.92.0 | none (installs no code) |", notices)
        self.assertFalse((self.out / "licenses" / "vcpkg" / "boost-uninstall").exists())

    def test_only_the_requested_triplet_is_listed(self):
        self.add_port("zlib", "1.4")
        self.add_port("zlib", "1.4", triplet="x64-linux")
        self.add_port("linux-only", "1", triplet="x64-linux")
        code, _, _ = self.run_tool()
        self.assertEqual(code, 0)
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn("| zlib | 1.4 |", notices)
        self.assertNotIn("linux-only", notices)

    def test_minimal_kit_uses_only_the_source_licenses_for_its_exact_version(self):
        self.add_port("zlib", "1.4")
        kit = self.add_qt()
        version = kit / "lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
        version.parent.mkdir(parents=True)
        version.write_text('set(PACKAGE_VERSION "6.10.3")\n', encoding="utf-8")
        code, _, err = self.run_tool("--qt", str(kit), "--require-qt-licenses")
        self.assertEqual((code, err), (0, ""))
        self.assertTrue((self.out / "licenses/qt/LGPL-3.0-only.txt").is_file())
        version.write_text('set(PACKAGE_VERSION "0.0.0")\n', encoding="utf-8")
        self.assertNotEqual(self.run_tool("--qt", str(kit), "--require-qt-licenses")[0], 0)

    def test_a_missing_qt_licence_is_a_warning_unless_required(self):
        self.add_port("zlib", "1.4")
        kit = self.add_qt()
        code, _, err = self.run_tool("--qt", str(kit))
        self.assertEqual(code, 0)
        self.assertIn("no Qt licence files found", err)
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn("did not include licence files", notices)
        code, _, err = self.run_tool("--qt", str(kit), "--require-qt-licenses")
        self.assertEqual(code, 1)
        self.assertIn("No Qt licence files", err)

    def test_stale_licences_from_an_earlier_run_are_removed(self):
        self.add_port("zlib", "1.4")
        self.add_port("old", "1")
        self.run_tool()
        (self.vcpkg / "vcpkg" / "info" / "old_1_x64-windows.list").unlink()
        self.run_tool()
        self.assertFalse((self.out / "licenses" / "vcpkg" / "old").exists())
        self.assertTrue((self.out / "licenses" / "vcpkg" / "zlib" / "copyright").is_file())

    def test_a_folder_that_is_not_vcpkg_installed_fails(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gn.main(["--vcpkg-installed", str(self.tmp / "nowhere"), "--out", str(self.out)])
        self.assertEqual(code, 1)
        self.assertIn("is this a vcpkg_installed folder", err.getvalue())

    def test_the_committed_fragments_are_all_present(self):
        names = [path.name for path in sorted(gn.NOTICES_DIR.glob("*.md"))]
        self.assertEqual(names, ["01-qt.md", "02-tabler.md", "03-speech.md", "04-dashboard.md",
                                 "05-python-docx.md"])
        for path in gn.NOTICES_DIR.glob("*.md"):
            self.assertTrue(path.read_text(encoding="utf-8").startswith("## "), path.name)

    def test_the_word_template_parts_carry_python_docx_licence(self):
        # src/documents/data embeds four parts of python-docx's default template in every Word
        # file written; its MIT terms ask for the copyright line and the permission notice.
        notice = (gn.NOTICES_DIR / "05-python-docx.md").read_text(encoding="utf-8")
        data = Path(gn.__file__).resolve().parents[2] / "src" / "documents" / "data"
        self.assertEqual(len(list(data.glob("word_*"))), 4)
        self.assertIn("python-docx 1.2.0", notice)
        self.assertIn("Copyright (c) 2013 Steve Canny, https://github.com/scanny", notice)
        self.assertIn("The above copyright notice and this permission notice shall be included",
                      notice)

    def test_the_notices_cover_every_runtime_package_of_the_dashboard_bundle(self):
        # webui\dist ships in {app}; its minified JavaScript carries no licence comment, so the
        # notices are the only place the MIT and OFL terms of what it bundles are shown.
        self.add_port("zlib", "1.4")
        self.assertEqual(self.run_tool()[0], 0)
        notices = (self.out / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        webui = Path(gn.__file__).resolve().parents[2] / "webui"
        lock = json.loads((webui / "package-lock.json").read_text(encoding="utf-8"))["packages"]
        runtime = {key[len("node_modules/"):]: value for key, value in lock.items()
                   if key.startswith("node_modules/") and not value.get("dev")}
        # loose-envify and js-tokens are browserify transforms that a Vite build never bundles.
        build_only = {"loose-envify", "js-tokens"}
        fonts = {"@fontsource-variable/dm-sans": "DM Sans",
                 "@fontsource/instrument-serif": "Instrument Serif"}
        self.assertEqual(set(runtime) - build_only,
                         {"react", "react-dom", "scheduler", *fonts})
        flat = " ".join(notices.split())
        for name in ("react", "react-dom", "scheduler"):
            self.assertEqual(runtime[name]["license"], "MIT")
            version = re.escape(runtime[name]["version"])
            self.assertRegex(notices,
                             rf"\(`{re.escape(name)}`[^|\n]*\| {version} \| MIT License \|")
        for package, font in fonts.items():
            self.assertEqual(runtime[package]["license"], "OFL-1.1")
            self.assertIn(f"`{package}` {runtime[package]['version']}", flat)
            self.assertIn(f"**{font}**", notices)
        self.assertIn("SIL Open Font License, Version 1.1", flat)
        self.assertIn("Permission is hereby granted, free of charge", notices)
        # Each font's copyright line is quoted from the licence file that ships beside it.
        for font, licence in (("DM Sans", "DM-Sans-OFL.txt"),
                              ("Instrument Serif", "Instrument-Serif-OFL.txt")):
            path = webui / "dist" / "licenses" / licence
            self.assertTrue(path.is_file(), licence)
            holder = re.match(r"(Copyright \d{4} The [A-Za-z ]+ Project Authors)",
                              path.read_text(encoding="utf-8"))[1]
            self.assertIn(holder, flat)
            self.assertIn(f"[webui/dist/licenses/{licence}](webui/dist/licenses/{licence})",
                          notices)
        self.assertLess(notices.index("## Speech runtime"), notices.index("## Meeting dashboard"))
        self.assertLess(notices.index("## Meeting dashboard"), notices.index("## Licence texts"))


if __name__ == "__main__":
    unittest.main()
