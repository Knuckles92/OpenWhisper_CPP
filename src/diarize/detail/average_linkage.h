#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace fv::diarize::detail {

// Average-linkage agglomerative clustering over the symmetric n x n distance matrix `dist`
// (row-major): merges the closest pair of clusters (the Lance-Williams average update) until the
// closest pair is farther apart than `threshold`. Each step takes the first minimum in row-major
// order over the whole matrix, dead rows and columns masked to +inf, as numpy's argmin does (a
// NaN counts as the minimum). Returns the clusters as row indices, in the order of each
// cluster's surviving row, each listing the row that absorbed the others first and then the
// absorbed rows in merge order. O(n^3) time and O(n^2) memory: callers keep n bounded. Throws
// std::invalid_argument when `dist` holds fewer than n * n values.
std::vector<std::vector<std::size_t>> average_linkage(std::span<const double> dist, std::size_t n,
                                                      double threshold);

} // namespace fv::diarize::detail
