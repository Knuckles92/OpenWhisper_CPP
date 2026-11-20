#include "process/detached.h"

namespace fv {

std::string list2cmdline(std::span<const std::string> args) {
    std::string result;
    for (const auto& arg : args) {
        std::string pending_backslashes;
        if (!result.empty()) {
            result += ' ';
        }
        const bool quote = arg.find(' ') != std::string::npos ||
                           arg.find('\t') != std::string::npos || arg.empty();
        if (quote) {
            result += '"';
        }
        for (const char c : arg) {
            if (c == '\\') {
                pending_backslashes += c;
            } else if (c == '"') {
                // Backslashes before a quote are doubled, then the quote is escaped.
                result.append(pending_backslashes.size() * 2, '\\');
                pending_backslashes.clear();
                result += "\\\"";
            } else {
                result += pending_backslashes;
                pending_backslashes.clear();
                result += c;
            }
        }
        result += pending_backslashes;
        if (quote) {
            // Backslashes before the closing quote are doubled.
            result += pending_backslashes;
            result += '"';
        }
    }
    return result;
}

} // namespace fv
