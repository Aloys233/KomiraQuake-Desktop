#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "model/earthquake_event.h"

namespace komira {

enum class EventGateDecision { Pass, Correction, Duplicate, Stale };

/// 事件去重与报数管理。《NATIVE_PORT_SPEC》 §5。
class EventGate {
public:
    explicit EventGate(long long maxAgeMs = 60LL * 60LL * 1000LL, int maxEntries = 1024);

    EventGateDecision admit(const EarthquakeEvent& event, long long nowMs);
    void clear();

private:
    struct Entry {
        EarthquakeEvent event;
        long long seenAt = 0;
    };

    static bool sameBody(const EarthquakeEvent& a, const EarthquakeEvent& b);
    static std::string keyOf(const EarthquakeEvent& e);
    void prune(long long nowMs);

    long long maxAgeMs_;
    int maxEntries_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace komira
