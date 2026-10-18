#pragma once

#include <QObject>
#include <QString>
#include <vector>

namespace komira {

/// 测站震度快照。AppController 每 100 ms 原地更新一次，地图项直接读 vector。
///
/// 之前用 `QVariantList<QVariantMap>`：C++ 侧要为上千站构造 map，QML 再转成 JS 数组，
/// 赋给 C++ 属性时又转回 QVariantList —— 每 100 ms 三轮全量转换。快照只复制 POD。
///
/// 悬停详情需要站名等元数据，但它们几乎不变、且远多于变化频率。若把 QString 直接塞进
/// Entry，每 100 ms 的整表拷贝就要复制 4 份字符串（含原子引用计数）。因此元数据放在
/// 与 entries 下标对齐的旁表 `metas_` 里：Entry 只多一个 int 下标，字符串只在首包/
/// 名录更新时参与一次隐式共享拷贝（QString 拷贝是引用计数，不分配内存）。
class StationSnapshot : public QObject {
    Q_OBJECT
public:
    struct Entry {
        double lat = 0.0;
        double lon = 0.0;
        /// 连续震度（NIED/S-Net）、中国地震烈度（Pancakes/Wolfx）或韩国 MMI（KMA）；
        /// 缺测均为负值。
        double raw = -3.0;
        /// true 表示该站用中国地震烈度（CSIS）方标，false 表示圆点。
        bool csis = false;
        /// true 表示该站为 KMA MMI：低烈度画 KMA 色阶圆点，MMI ≥ 2 画带数字方标。
        bool kma = false;
        /// 对应 metas() 的下标；-1 表示无元数据（测试/旧构造路径）。
        int meta = -1;
    };

    /// 单站展示元数据（仅悬停浮层用，不参与绘制）。KMA 上游没有名称，字段可为空。
    struct StationMeta {
        QString name;    ///< 观测点名称 / Wolfx 展示位置
        QString area;    ///< 都道府県 / 行政区
        QString code;    ///< 测站代码 / Wolfx UUID
        QString type;    ///< K-NET / KiK-net / S-net 区段
        QString source;  ///< 数据源显示名（NIED Kmoni / S-Net / KMA PEWS / …）
    };

    explicit StationSnapshot(QObject* parent = nullptr) : QObject(parent) {}

    const std::vector<Entry>& entries() const { return entries_; }
    /// 与 entries() 下标对齐的元数据表；长度可能与 entries() 不同（允许为 0）。
    const std::vector<StationMeta>& metas() const { return metas_; }
    int count() const { return static_cast<int>(entries_.size()); }

    /// 替换全部条目；内容与当前完全一致时不发信号（省掉一次重绘）。
    void setEntries(std::vector<Entry> entries);
    /// 同时替换条目与元数据（AppController 的常规路径）。
    void setEntries(std::vector<Entry> entries, std::vector<StationMeta> metas);

signals:
    void changed();

private:
    std::vector<Entry> entries_;
    std::vector<StationMeta> metas_;
};

} // namespace komira
