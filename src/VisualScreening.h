#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace screening {
using Span = std::pair<double, double>;

// A conservative technical-quality heuristic, not an aesthetic or semantic model.
// Frames are display-oriented, grayscale, sampled at 2 fps by the caller.
inline bool usableFrame(const std::uint8_t *frame, const std::uint8_t *previous,
                        int width, int height, bool *hasPicture = nullptr) {
    if (!frame || width < 2 || height < 2) return false;
    const int count = width * height;
    double sum = 0, squares = 0, gradient = 0, motion = 0;
    int dark = 0, bright = 0;
    for (int i = 0; i < count; ++i) {
        const double value = frame[i];
        sum += value;
        squares += value * value;
        dark += value < 12;
        bright += value > 244;
        if (i % width) gradient += std::abs(value - frame[i - 1]);
        if (i >= width) gradient += std::abs(value - frame[i - width]);
        if (previous) motion += std::abs(value - previous[i]);
    }
    const double variance = squares / count - std::pow(sum / count, 2);
    const bool picture = dark < count * 0.92 && bright < count * 0.92 && variance >= 9.0;
    if (hasPicture) *hasPicture = picture;
    return picture
        && gradient / (2 * count - width - height) >= 1.8
        && (!previous || motion / (count * 255.0) < 0.16);
}

inline std::vector<Span> mergeSpans(std::vector<Span> spans, double duration) {
    std::vector<Span> result;
    std::sort(spans.begin(), spans.end());
    for (auto span : spans) {
        if (!std::isfinite(span.first) || !std::isfinite(span.second)) continue;
        span.first = std::max(0.0, span.first);
        span.second = std::min(duration, span.second);
        if (span.second <= span.first) continue;
        if (!result.empty() && span.first <= result.back().second)
            result.back().second = std::max(result.back().second, span.second);
        else result.push_back(span);
    }
    return result;
}
}
