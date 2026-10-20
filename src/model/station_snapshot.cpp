#include "model/station_snapshot.h"

#include <utility>

namespace komira {
namespace {

/// Entry 的**绘制相关**字段完全一致即视为同一帧：metas 只在条目/元数据一起替换时
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

const std::vector<StationSnapshot::Entry>& emptyEntries() {
    static const std::vector<StationSnapshot::Entry> kEmpty;
    return kEmpty;
}

const std::vector<StationSnapshot::StationMeta>& emptyMetas() {
    static const std::vector<StationSnapshot::StationMeta> kEmpty;
    return kEmpty;
}

} // namespace

const std::vector<StationSnapshot::Entry>& StationSnapshot::entries() const {
    return entries_ ? *entries_ : emptyEntries();
}

const std::vector<StationSnapshot::StationMeta>& StationSnapshot::metas() const {
    return metas_ ? *metas_ : emptyMetas();
}

bool StationSnapshot::setEntries(std::vector<Entry> entries) {
    // 只替换震度：元数据表保持不变（AppController 拆开两条路径，避免每 100 ms
    // 复制一遍字符串引用计数）。
    if (entries_ && sameFrame(entries, *entries_)) return false;
    entries_ = std::make_shared<const std::vector<Entry>>(std::move(entries));
    emit changed();
    return true;
}

bool StationSnapshot::setEntries(std::vector<Entry> entries, std::vector<StationMeta> metas) {
    const bool sameEntries = entries_ && sameFrame(entries, *entries_);
    const bool sameMetas = metas_ && sameMeta(metas, *metas_);
    if (sameEntries && sameMetas) return false;
    entries_ = std::make_shared<const std::vector<Entry>>(std::move(entries));
    metas_ = std::make_shared<const std::vector<StationMeta>>(std::move(metas));
    emit changed();
    return true;
}

bool StationSnapshot::setSharedEntries(EntryList entries) {
    if (!entries) entries = std::make_shared<const std::vector<Entry>>();
    // 同一份帧被重复投喂（例如元数据路径刚设过）：指针相同即内容必然相同。
    if (entries_ == entries) return false;
    if (entries_ && sameFrame(*entries, *entries_)) {
        // 内容等价但指针不同：只更新持有者，避免下游拿到旧帧的强引用。
        entries_ = std::move(entries);
        return false;
    }
    entries_ = std::move(entries);
    emit changed();
    return true;
}

bool StationSnapshot::setSharedEntries(EntryList entries, MetaList metas) {
    if (!entries) entries = std::make_shared<const std::vector<Entry>>();
    if (!metas) metas = std::make_shared<const std::vector<StationMeta>>();
    const bool sameEntries = entries_ == entries || (entries_ && sameFrame(*entries, *entries_));
    const bool sameMetas = metas_ == metas || (metas_ && sameMeta(*metas, *metas_));
    if (sameEntries && sameMetas) return false;
    entries_ = std::move(entries);
    metas_ = std::move(metas);
    emit changed();
    return true;
}

} // namespace komira
