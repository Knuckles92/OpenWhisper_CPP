#pragma once

#include <span>
#include <string>
#include <string_view>

#include "process/launcher.h"

namespace fv::process_detail {

// bytes.decode("utf-8", errors="replace"): each maximal invalid subpart becomes one U+FFFD.
std::string decode_utf8_replace(std::string_view bytes);

// A ByteSink that hands `sink` one line at a time: split at "\n", a "\r" before it dropped,
// decoded as decode_utf8_replace does, and the unterminated rest at the end of the pipe.
ByteSink line_splitter(LineSink sink);

// The home folder as os.path.expanduser("~") gives it: USERPROFILE, else HOMEDRIVE and
// HOMEPATH on Windows; HOME elsewhere. Empty when there is none.
std::string home_folder();

} // namespace fv::process_detail
