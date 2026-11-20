#include "diarize/detail/average_linkage.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fv::diarize::detail {

namespace {

// numpy.argmin over the flat matrix: the first minimum, or the first NaN when there is one.
std::size_t argmin(const double* values, std::size_t count) {
    std::size_t best = 0;
    double best_value = values[0];
    if (std::isnan(best_value)) {
        return 0;
    }
    for (std::size_t i = 1; i < count; ++i) {
        const double value = values[i];
        if (std::isnan(value)) {
            return i;
        }
        if (value < best_value) {
            best_value = value;
            best = i;
        }
    }
    return best;
}

} // namespace

std::vector<std::vector<std::size_t>> average_linkage(std::span<const double> dist, std::size_t n,
                                                      double threshold) {
    if (dist.size() < n * n) {
        throw std::invalid_argument("The distance matrix is smaller than n x n");
    }
    if (n == 0) {
        return {};
    }
    if (n == 1) {
        return {{0}};
    }
    constexpr double inf = std::numeric_limits<double>::infinity();
    std::vector<double> matrix(dist.begin(), dist.begin() + static_cast<std::ptrdiff_t>(n * n));
    double* d = matrix.data();
    for (std::size_t i = 0; i < n; ++i) {
        d[i * n + i] = inf;
    }
    std::vector<std::vector<std::size_t>> members(n);
    for (std::size_t i = 0; i < n; ++i) {
        members[i] = {i};
    }
    std::vector<double> sizes(n, 1.0);
    std::vector<char> alive(n, 1);
    std::size_t alive_count = n;
    std::vector<double> merged(n);

    while (alive_count > 1) {
        const std::size_t flat = argmin(d, n * n);
        const std::size_t a = flat / n;
        const std::size_t b = flat % n;
        if (d[flat] > threshold) {
            break;
        }
        // d(A u B, C), computed from the rows as they were before this merge.
        const double size_a = sizes[a];
        const double size_b = sizes[b];
        const double* row_a = d + a * n;
        const double* row_b = d + b * n;
        for (std::size_t c = 0; c < n; ++c) {
            merged[c] = (size_a * row_a[c] + size_b * row_b[c]) / (size_a + size_b);
        }
        for (std::size_t c = 0; c < n; ++c) {
            d[b * n + c] = inf;
            d[c * n + b] = inf;
        }
        for (std::size_t c = 0; c < n; ++c) {
            d[a * n + c] = merged[c];
            d[c * n + a] = merged[c];
        }
        d[a * n + a] = inf;
        d[a * n + b] = inf;
        d[b * n + a] = inf;

        sizes[a] += sizes[b];
        // A copy: a pick on the diagonal (only a NaN or an infinite threshold allows one) makes
        // a == b, where Python's list.extend appends the list to itself.
        const std::vector<std::size_t> absorbed = members[b];
        members[a].insert(members[a].end(), absorbed.begin(), absorbed.end());
        alive[b] = 0;
        --alive_count;
    }

    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < n; ++i) {
        if (alive[i] != 0) {
            groups.push_back(std::move(members[i]));
        }
    }
    return groups;
}

} // namespace fv::diarize::detail
