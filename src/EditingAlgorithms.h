#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace editing {
// Keep complete speech units; visual shots may consume the remaining budget.
inline std::pair<double, double> budgetSpan(double start, double end, double budget,
                                             double fps, bool speech) {
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(budget)
        || !std::isfinite(fps) || fps <= 0 || start < 0 || end <= start || budget < 0.5) return {};
    const double first = std::ceil(start * fps - 1e-7) / fps;
    const double last = std::floor(end * fps + 1e-7) / fps;
    if (speech && last - first > budget + 1e-7) return {};
    const double chosenEnd = std::min(last, first + std::floor(budget * fps + 1e-7) / fps);
    return chosenEnd - first >= 0.5 ? std::make_pair(first, chosenEnd) : std::pair<double, double>{};
}

inline bool sameRetake(const std::u16string &a, const std::u16string &b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    if (std::min(a.size(), b.size()) < 8) return false;
    const size_t limit = std::max(a.size(), b.size()) / 10;
    if (std::max(a.size(), b.size()) - std::min(a.size(), b.size()) > limit) return false;
    std::vector<size_t> previous(b.size() + 1), current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) previous[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                  previous[j - 1] + (a[i - 1] != b[j - 1])});
        previous.swap(current);
    }
    return previous.back() <= limit;
}
}
