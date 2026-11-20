#pragma once

#include <optional>
#include <string_view>

namespace fv {

// How well query matches target (UTF-8), for dropdown filters and search inputs. The first of
// five strategies that matches decides the score: exact (10000), substring (3000 plus a
// start-of-word bonus), delimiter-normalised substring (2200), every query token on a distinct
// target token (1800), delimiters stripped (1500), subsequence (700, at least 100). Text is
// case-folded and indexed in code points. A blank query scores 0; no match is nullopt.
std::optional<int> fuzzy_score(std::string_view query, std::string_view target);

// fuzzy_score(query, target) has a value.
bool fuzzy_match(std::string_view query, std::string_view target);

} // namespace fv
