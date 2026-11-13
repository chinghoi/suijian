#pragma once

#include "VisualScreening.h"

namespace screening {

constexpr double audioWindow = 0.05;

struct AudioLevel { double time, db; };
struct Dialogue { Span span; double score; };
struct Candidate { Span span; bool dialogue = false; double score = 0; };

// Band-limited energy is a hint, not a speech classifier. Estimate the typical
// background and group activity into passages before applying boundary padding.
inline std::vector<Dialogue> possibleDialogue(const std::vector<AudioLevel> &levels, double duration,
                                               const std::vector<Span> &blank = {}) {
    if (levels.empty()) return {};
    std::vector<double> sorted;
    sorted.reserve(levels.size());
    for (auto level : levels) sorted.push_back(std::isfinite(level.db) ? level.db : -120.0);
    auto median = sorted.begin() + sorted.size() / 2;
    std::nth_element(sorted.begin(), median, sorted.end());
    const double threshold = std::max(-43.0, *median + 4.0);

    std::vector<Dialogue> spans;
    double first = -1.0, last = 0.0, power = 0.0;
    int active = 0;
    auto close = [&] {
        if (active * audioWindow >= 0.45 && last - first >= 0.45) {
            spans.push_back({{first, last}, std::sqrt(power / active)});
        }
        first = -1.0;
        power = 0.0;
        active = 0;
    };
    size_t barrier = 0;
    for (auto level : levels) {
        if (!std::isfinite(level.time) || level.time < 0 || level.time >= duration) continue;
        while (barrier < blank.size() && blank[barrier].second <= level.time) ++barrier;
        if (barrier < blank.size() && blank[barrier].first <= level.time) { close(); continue; }
        if (level.db < threshold || !std::isfinite(level.db)) continue;
        const double gap = level.time - last;
        // Keep short pauses, but allow long passages to split at a real pause.
        if (first >= 0 && (gap > 2.0 || (last - first >= 12.0 && gap >= 0.65))) close();
        if (first < 0) first = level.time;
        last = std::min(duration, level.time + audioWindow);
        power += std::pow(10.0, level.db / 10.0);
        ++active;
    }
    close();
    std::vector<Dialogue> padded;
    barrier = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        auto [start, end] = spans[i].span;
        const double left = i ? (spans[i - 1].span.second + start) / 2 : 0.0;
        const double right = i + 1 < spans.size() ? (end + spans[i + 1].span.first) / 2 : duration;
        Span span{std::max(left, start - 0.8), std::min(right, end + 1.0)};
        while (barrier < blank.size() && blank[barrier].second <= start) {
            span.first = std::max(span.first, blank[barrier].second);
            ++barrier;
        }
        if (barrier < blank.size()) span.second = std::min(span.second, blank[barrier].first);
        const double length = span.second - span.first;
        if (length >= 2.0) padded.push_back({span, length * spans[i].score});
    }
    return padded;
}

inline std::vector<Candidate> combineCandidates(const std::vector<Span> &visual,
                                                 const std::vector<Dialogue> &dialogue) {
    std::vector<Candidate> result;
    std::vector<Span> protectedSpans;
    for (const auto &speech : dialogue) {
        protectedSpans.push_back(speech.span);
        result.push_back({speech.span, true, speech.score});
    }

    // Keep speech boundaries distinct even when a long good picture overlaps
    // them. The duration allocator can then choose the dialogue as a unit.
    size_t first = 0;
    for (auto picture : visual) {
        while (first < protectedSpans.size() && protectedSpans[first].second <= picture.first) ++first;
        double start = picture.first;
        for (size_t i = first; i < protectedSpans.size() && protectedSpans[i].first < picture.second; ++i) {
            if (protectedSpans[i].first > start) result.push_back({{start, protectedSpans[i].first}});
            start = std::max(start, protectedSpans[i].second);
        }
        if (start < picture.second) result.push_back({{start, picture.second}});
    }
    std::sort(result.begin(), result.end(), [](const Candidate &a, const Candidate &b) {
        return a.span < b.span;
    });
    return result;
}

} // namespace screening
