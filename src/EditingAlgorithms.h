#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <map>
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

struct BudgetClip {
    double start, end, fps;
    bool dialogue = false;
    double score = 0;
    size_t source = 0;
    double sourceDuration = 0;
};

// Allocate the duration across the whole sequence, instead of exhausting it
// on the first few shots. Speech units remain intact.
inline std::vector<std::pair<double, double>> spreadBudget(
        const std::vector<BudgetClip> &clips, double budget, bool speech) {
    std::vector<std::pair<double, double>> result(clips.size());
    if (!std::isfinite(budget) || budget <= 0) return result;
    double total = 0;
    for (const auto &clip : clips)
        if (std::isfinite(clip.start) && std::isfinite(clip.end) && clip.start >= 0 && clip.end > clip.start
            && std::isfinite(clip.fps) && clip.fps > 0)
            total += clip.end - clip.start;
    if (total <= 0) return result;
    const double ratio = std::min(1.0, budget / total);
    double seen = 0, used = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto &clip = clips[i];
        const double duration = clip.end - clip.start;
        if (!std::isfinite(clip.start) || clip.start < 0 || !std::isfinite(duration) || duration <= 0
            || !std::isfinite(clip.fps) || clip.fps <= 0) continue;
        seen += duration;
        if (speech) {
            const auto whole = budgetSpan(clip.start, clip.end, budget - used, clip.fps, true);
            const double length = whole.second - whole.first;
            if (length > 0 && (seen * ratio - used >= length * 0.5 || ratio >= 1)) result[i] = whole;
        } else {
            // Carry unused fractions forward so many short shots don't all
            // fall below the minimum clip length and disappear.
            const double share = std::min({duration, std::max(duration * ratio, seen * ratio - used), budget - used});
            const double offset = std::max(0.0, (duration - share) / 2);
            result[i] = budgetSpan(clip.start + offset, clip.end, share, clip.fps, false);
        }
        used += result[i].second - result[i].first;
    }
    return result;
}

// Allocate by original footage length, then give unused time to other sources.
// Speech is picked as complete passages by score. Pictures have a minimum hold
// and a share of the quota when there is enough time for both.
inline std::vector<std::pair<double, double>> dialogueFirstBudget(
        const std::vector<BudgetClip> &clips, double budget) {
    std::vector<std::pair<double, double>> result(clips.size());
    if (!std::isfinite(budget) || budget <= 0) return result;
    struct Source { double duration = 0; std::vector<size_t> speech, pictures; };
    std::map<size_t, Source> sources;
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto &c = clips[i];
        if (!std::isfinite(c.start) || !std::isfinite(c.end) || !std::isfinite(c.fps)
            || c.start < 0 || c.end <= c.start || c.fps <= 0) continue;
        auto &source = sources[c.source];
        source.duration = std::max({source.duration, c.end, c.sourceDuration});
        (c.dialogue ? source.speech : source.pictures).push_back(i);
    }
    auto rank = [&](std::vector<size_t> &indices, bool speech) {
        std::stable_sort(indices.begin(), indices.end(), [&](size_t i, size_t j) {
            const auto &a = clips[i], &b = clips[j];
            return speech ? a.score > b.score : a.end - a.start > b.end - b.start;
        });
    };
    auto keepSpeech = [&](const std::vector<size_t> &indices, double &left) {
        for (auto i : indices) {
            const auto &c = clips[i];
            const double len = c.end - c.start;
            if (result[i].second || len > left) continue;
            result[i] = {c.start, c.end};
            left -= len;
        }
    };
    auto keepPictures = [&](const std::vector<size_t> &indices, double &left, bool extend) {
        for (auto i : indices) {
            const auto &c = clips[i];
            const double len = c.end - c.start;
            const double used = result[i].second - result[i].first;
            if (len < 3.0 || left + used < 3.0 || used >= len || (used > 0 && !extend)) continue;
            const double hold = std::min(left + used, len);
            const auto span = hold >= len ? std::make_pair(c.start, c.end)
                : budgetSpan(c.start + (len - hold) / 2, c.end, hold, c.fps, false);
            const double duration = span.second - span.first;
            if (duration < 3.0 - 1e-7 || duration <= used) continue;
            result[i] = span;
            left -= duration - used;
        }
    };
    double total = 0, unused = budget;
    for (const auto &[id, s] : sources) total += s.duration;
    std::vector<size_t> allSpeech, allPictures;
    for (auto &[id, s] : sources) {
        rank(s.speech, true);
        rank(s.pictures, false);
        const double quota = budget * (s.duration / total);
        const bool hasPicture = !s.pictures.empty() && clips[s.pictures.front()].end - clips[s.pictures.front()].start >= 3.0;
        double left = quota;
        if (hasPicture && quota >= 12.0) {
            // Leave room for a visual passage if activity would fill the quota.
            const double reserve = std::max(3.1, std::min(quota * 0.25,
                clips[s.pictures.front()].end - clips[s.pictures.front()].start));
            double speechLeft = left - reserve;
            keepSpeech(s.speech, speechLeft);
            left = reserve + speechLeft;
        } else {
            keepSpeech(s.speech, left);
        }
        keepPictures(s.pictures, left, false);
        unused -= quota - left;
        allSpeech.insert(allSpeech.end(), s.speech.begin(), s.speech.end());
        allPictures.insert(allPictures.end(), s.pictures.begin(), s.pictures.end());
    }
    rank(allSpeech, true);
    rank(allPictures, false);
    keepSpeech(allSpeech, unused);
    keepPictures(allPictures, unused, false);
    keepPictures(allPictures, unused, true);
    return result;
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
