#include "model/station_snapshot.h"

namespace komira {

void StationSnapshot::setEntries(std::vector<Entry> entries) {
    if (entries.size() == entries_.size()) {
        bool same = true;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& a = entries[i];
            const Entry& b = entries_[i];
            if (a.lat != b.lat || a.lon != b.lon || a.raw != b.raw || a.csis != b.csis ||
                a.kma != b.kma) {
                same = false;
                break;
            }
        }
        if (same) return;
    }
    entries_ = std::move(entries);
    emit changed();
}

} // namespace komira
