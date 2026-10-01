#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "core/event_gate.h"

namespace komira {

// Pure event reducer; caller supplies the calibrated clock and executes effects.
class WarningSession {
public:
    struct Active {
        EarthquakeEvent event;
        long long firstOrigin = 0;
        bool hidden = false;
        bool muted = false;
        bool arrived = false;
    };
    enum class Change { Ignored, Added, Updated, Corrected, Ended };
    Change accept(const EarthquakeEvent& event, long long now) {
        prune(now);
        const auto key = event.identity();
        if (tombstones_.count(key)) return Change::Ignored;
        const auto decision = gate_.admit(event, now);
        if (decision == EventGateDecision::Stale || decision == EventGateDecision::Duplicate)
            return Change::Ignored;
        if (event.isCanceled || event.expired(now)) {
            finish(key, now);
            return Change::Ended;
        }
        auto it = active.find(key);
        if (it == active.end()) {
            active.emplace(key, Active{event, event.timestamp});
            return Change::Added;
        }
        it->second.event = event;
        return decision == EventGateDecision::Correction ? Change::Corrected : Change::Updated;
    }
    std::vector<std::string> tick(long long now) {
        std::vector<std::string> ended;
        for (const auto& [key, state] : active)
            if (state.event.expired(now) || now >= state.firstOrigin + 30LL * 60 * 1000)
                ended.push_back(key);
        for (const auto& key : ended) finish(key, now);
        prune(now);
        return ended;
    }
    void finish(const std::string& key, long long now) {
        active.erase(key);
        tombstones_[key] = now;
    }
    bool finished(const std::string& key) const { return tombstones_.count(key) != 0; }
    std::map<std::string, Active> active;
private:
    void prune(long long now) {
        for (auto it = tombstones_.begin(); it != tombstones_.end();)
            if (now - it->second > 24LL * 60 * 60 * 1000) it = tombstones_.erase(it);
            else ++it;
        // At this age a replay also fails the event's hard age limit.
        while (tombstones_.size() > 4096) {
            const auto oldest = std::min_element(tombstones_.begin(), tombstones_.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            tombstones_.erase(oldest);
        }
    }
    EventGate gate_;
    std::map<std::string, long long> tombstones_;
};

} // namespace komira
