#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pyport/errors.h"

namespace fv::py {

// re.error: a pattern or replacement template Python would refuse.
class RegexError : public PyError {
public:
    explicit RegexError(const std::string& message) : PyError("error", message) {}
};

// Flag values are Python's (re.IGNORECASE == 2, ...).
enum RegexFlag : unsigned {
    IGNORECASE = 2,
    MULTILINE = 8,
    DOTALL = 16,
    VERBOSE = 64,
};

class Regex;

// re.Match. Positions are code point indices into the searched text, as in Python; the byte_
// accessors give the same span in UTF-8 bytes. A Match keeps its own copy of the text and may
// outlive its Regex.
class Match {
public:
    std::string group(std::size_t index = 0) const; // "" when the group did not take part
    std::string group(std::string_view name) const;
    std::optional<std::string> group_if(std::size_t index) const; // nullopt when not taking part
    bool matched(std::size_t index) const;
    std::vector<std::optional<std::string>> groups() const; // groups 1..n

    std::size_t start(std::size_t index = 0) const;
    std::size_t end(std::size_t index = 0) const;
    std::size_t byte_start(std::size_t index = 0) const;
    std::size_t byte_end(std::size_t index = 0) const;

    // Match.expand(template): \1, \g<1>, \g<name>, \n and the other escapes of re.sub.
    std::string expand(std::string_view repl) const;

private:
    friend class Regex;
    static constexpr std::size_t kUnset = static_cast<std::size_t>(-1);

    std::shared_ptr<const std::string> text_;
    std::vector<std::size_t> offsets_; // byte start and end of each group, kUnset when unset
    std::shared_ptr<const std::map<std::string, std::size_t, std::less<>>> names_;
};

// A compiled Python re pattern over UTF-8 text, run by PCRE2. Python's syntax is translated
// first: \w, \d, \s, \b, \B and their negations use Python 3.12's Unicode 15.0 tables (PCRE2's
// own are newer and count combining marks as \w), \Z is the end of the text, \uXXXX and
// \UXXXXXXXX are code points, and \v is the vertical tab. IGNORECASE uses PCRE2's Unicode case
// folding, which can differ from Python's for characters added after Unicode 15.0.
// Throws RegexError for a pattern Python or PCRE2 refuses.
class Regex {
public:
    explicit Regex(std::string_view pattern, unsigned flags = 0);
    ~Regex();
    Regex(Regex&&) noexcept;
    Regex& operator=(Regex&&) noexcept;

    std::size_t group_count() const;
    const std::map<std::string, std::size_t, std::less<>>& group_index() const;

    // pos is a code point index, as Python's.
    std::optional<Match> search(std::string_view text, std::size_t pos = 0) const;
    std::optional<Match> match(std::string_view text, std::size_t pos = 0) const;
    std::optional<Match> fullmatch(std::string_view text, std::size_t pos = 0) const;
    std::vector<Match> finditer(std::string_view text) const;

    // re.sub: the template is checked before any matching, as Python does. count 0 = all.
    std::string sub(std::string_view repl, std::string_view text, std::size_t count = 0) const;
    std::string sub(const std::function<std::string(const Match&)>& repl, std::string_view text,
                    std::size_t count = 0) const;
    // re.subn: the result and the number of replacements.
    std::pair<std::string, std::size_t> subn(std::string_view repl, std::string_view text,
                                             std::size_t count = 0) const;

    // re.split: captured groups are included, nullopt for a group that did not take part.
    std::vector<std::optional<std::string>> split(std::string_view text,
                                                  std::size_t maxsplit = 0) const;

    // re.findall with at most one group: the whole match or that group's text. Throws
    // RegexError for a pattern with more groups; use findall_groups.
    std::vector<std::string> findall(std::string_view text) const;
    // re.findall with several groups: one tuple of group texts ("" when unset) per match.
    std::vector<std::vector<std::string>> findall_groups(std::string_view text) const;

private:
    struct Impl;
    friend class Match;

    std::optional<Match> run(std::shared_ptr<const std::string> text, std::size_t byte_pos,
                             unsigned options) const;
    template <class Visit>
    void each_match(std::string_view text, std::size_t limit, Visit visit) const;

    std::unique_ptr<Impl> impl_;
};

// re.escape: backslash before each character of "()[]{}?*+-|^$\.&~# \t\n\r\v\f".
std::string regex_escape(std::string_view text);

} // namespace fv::py
