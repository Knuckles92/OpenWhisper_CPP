#pragma once

#include <string>
#include <vector>

namespace fv {

// --self-test-gui: the checks of main.py's _run_package_self_test that need Qt, rewritten for a
// native build (A01-79): the offscreen platform plugin starts, the SVG image format and icon
// engine plugins load the bundled check.svg, :/ui_theme/theme.qss is compiled in, the windows11
// style exists, and the app icon draws. Prints "<product> package self-test passed" and returns
// 0, or prints each failure on stderr and returns 1. Always offscreen: it shows nothing.
// Creates the QApplication itself when none exists.
int runSelfTestGui(int argc, char** argv);

// The failed checks, one sentence each; empty when every check passes. Needs a QApplication.
std::vector<std::string> selfTestGuiProblems();

} // namespace fv
