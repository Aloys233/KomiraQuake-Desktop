#include "model/station_snapshot.h"

#include <utility>

namespace komira {
namespace {

/// Entry 的**绘制相关**字段完全一致即视为同一帧：metas_ 只在条目/元数据一起替换时
/// 才会变化（AppController 只在首包或名录更新时才传新表），因此比较 meta 下标即可。
bool sameFrame(const std::vector<StationSnapshot::Entry>& a,
               const std::vector<StationSnapshot::Entry>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].lat != b[i].lat || a[i].lon != b[i].lon || a[i].raw != b[i].raw ||
            a[i].csis != b[i].csis || a[i].kma != b[i].kma || a[i].meta != b[i].meta) {
            return false;
        }
    }
    return true;
}

bool sameMeta(const std::vector<StationSnapshot::StationMeta>& a,
              const std::vector<StationSnapshot::StationMeta>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].area != b[i].area || a[i].code != b[i].code ||
            a[i].type != b[i].type || a[i].source != b[i].source) {
            return false;
        }
    }
    return true;
}

} // namespace

void StationSnapshot::setEntries(std::vector<Entry> entries) {
    // 只替换震度：元数据表保持不变（AppController 拆开两条路径，避免每 100 ms
    // 复制一遍字符串引用计数）。
    if (sameFrame(entries, entries_)) return;
    entries_ = std::move(entries);
    emit changed();
}

void StationSnapshot::setEntries(std::vector<Entry> entries, std::vector<StationMeta> metas) {
    const bool sameEntries = sameFrame(entries, entries_);
    if (sameEntries && sameMeta(metas, metas_)) return;
    entries_ = std::move(entries);
    metas_ = std::move(metas);
    emit changed();
}

} // namespace komira
