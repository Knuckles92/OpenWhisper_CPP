#pragma once

#include "pyport/regex.h"

namespace fv::walkthrough::detail {

// write_up.py's _IMAGE: ![alt](target) or ![alt](<target> "title"); group 1 the alt text,
// group 2 the target as written (with its angle brackets).
const py::Regex& image_pattern();

// library.py's _SHOT: a screenshot's file name, shot-<n>.png from 1.
const py::Regex& shot_pattern();

} // namespace fv::walkthrough::detail
