#include "core/event_gate.h"

#include <algorithm>
#include <vector>

namespace komira {

EventGate::EventGate(long long maxAgeMs, int maxEntries)
    : maxAgeMs_(maxAgeMs), maxEntries_(maxEntries) {}

std::string EventGate::keyOf(const EarthquakeEvent& e) { return e.identity(); }

bool EventGate::sameBody(const EarthquakeEvent& a, const EarthquakeEvent& b) {
    return a.magnitude == b.magnitude && a.latitude == b.latitude && a.longitude == b.longitude &&
           a.depth == b.depth && a.location == b.location && a.isFinal == b.isFinal &&
           a.isCanceled == b.isCanceled && a.timestamp == b.timestamp &&
           a.reportTime == b.reportTime && a.maxIntensityRaw == b.maxIntensityRaw &&
           a.maxIntensityText == b.maxIntensityText;
}

void EventGate::prune(long long nowMs) {
    if (entries_.empty()) return;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (nowMs - it->second.seenAt > maxAgeMs_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    if (static_cast<int>(entries_.size()) > maxEntries_) {
        std::vector<std::pair<long long, std::string>> ordered;
        ordered.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) ordered.emplace_back(entry.seenAt, key);
        std::sort(ordered.begin(), ordered.end());
        const int excess = static_cast<int>(entries_.size()) - maxEntries_;
        for (int i = 0; i < excess; ++i) entries_.erase(ordered[static_cast<size_t>(i)].second);
    }
}

EventGateDecision EventGate::admit(const EarthquakeEvent& event, long long nowMs) {
    prune(nowMs);
    const std::string key = keyOf(event);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        entries_[key] = Entry{event, nowMs};
        return EventGateDecision::Pass;
    }
    Entry& prev = it->second;
    prev.seenAt = nowMs;

    if (event.reportNum < prev.event.reportNum) return EventGateDecision::Stale;
    if (event.reportNum == prev.event.reportNum) {
        if (sameBody(event, prev.event)) return EventGateDecision::Duplicate;
        // 跨聚合商同报次：现任优先（先到者胜）。两路转发的同一报文若字段有细微差异，
        // 会随各自轮询反复互相覆盖而抖动；只有更高报次才接管。
        if (event.sourceProvider != prev.event.sourceProvider) return EventGateDecision::Duplicate;
        // 同一聚合商的同报次修正仍然生效。
        prev.event = event;
        return EventGateDecision::Correction;
    }
    prev.event = event;
    return EventGateDecision::Pass;
}

void EventGate::clear() { entries_.clear(); }

} // namespace komira
