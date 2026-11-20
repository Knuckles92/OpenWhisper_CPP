#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "pyport/errors.h"

namespace fv::py {

using Json = fv::Json; // nlohmann::ordered_json, as settings/setting.h

// Python's json.JSONDecodeError: the message is "<msg>: line L column C (char P)", with the
// position in code points.
class JsonError : public ValueError {
public:
    JsonError(std::string msg, std::size_t pos, std::size_t lineno, std::size_t colno);
    const std::string& msg() const noexcept { return msg_; }
    std::size_t pos() const noexcept { return pos_; }
    std::size_t lineno() const noexcept { return lineno_; }
    std::size_t colno() const noexcept { return colno_; }

private:
    std::string msg_;
    std::size_t pos_;
    std::size_t lineno_;
    std::size_t colno_;
};

// Containers json.loads of Python 3.12 nests before RecursionError, measured from a script's
// top level (the interpreter's C recursion budget).
inline constexpr std::size_t kJsonMaxDepth = 2997;

struct DumpOptions {
    std::optional<int> indent; // spaces per level; 0 or less still breaks lines
    bool ensure_ascii = true;  // non-ASCII as \uXXXX, astral as a surrogate pair
    bool sort_keys = false;
    std::string item_sep, key_sep; // empty -> Python's defaults: ", " (or "," with indent), ": "
};

// json.dumps: floats as repr(), NaN, Infinity and -Infinity as Python writes them. A string
// holding a three-byte-encoded lone surrogate (from loads) is written back as \udXXX.
std::string dumps(const Json& value, const DumpOptions& options = {});

// json.loads of a str: NaN, Infinity and -Infinity accepted, a duplicate key keeps the last value
// in the first key's position, a leading U+FEFF refused, more than kJsonMaxDepth nested
// containers is RecursionError. Throws JsonError with Python's messages. An integer beyond
// uint64 becomes a float (Python keeps an int); a lone surrogate escape becomes its
// three-byte encoding.
Json loads(std::string_view text);

// Python's == between JSON values: True == 1 == 1.0, exact across int and float, NaN unequal to
// everything, dicts regardless of key order.
bool py_equal(const Json& a, const Json& b);

} // namespace fv::py
