#pragma once

#include <QObject>
#include <vector>

namespace komira {

/// 测站震度快照。AppController 每 100 ms 原地更新一次，地图项直接读 vector。
///
/// 之前用 `QVariantList<QVariantMap>`：C++ 侧要为上千站构造 map，QML 再转成 JS 数组，
/// 赋给 C++ 属性时又转回 QVariantList —— 每 100 ms 三轮全量转换。快照只复制 POD。
class StationSnapshot : public QObject {
    Q_OBJECT
public:
    struct Entry {
        double lat = 0.0;
        double lon = 0.0;
        /// 连续震度（NIED）或 CSIS 烈度（Pancakes/Wolfx）；缺测为负值。
        double raw = -3.0;
        /// true 表示该站用中国地震烈度（CSIS）方标，false 表示 NIED 圆点。
        bool csis = false;
    };

    explicit StationSnapshot(QObject* parent = nullptr) : QObject(parent) {}

    const std::vector<Entry>& entries() const { return entries_; }
    int count() const { return static_cast<int>(entries_.size()); }

    /// 替换全部条目；内容与当前完全一致时不发信号（省掉一次重绘）。
    void setEntries(std::vector<Entry> entries);

signals:
    void changed();

private:
    std::vector<Entry> entries_;
};

} // namespace komira
