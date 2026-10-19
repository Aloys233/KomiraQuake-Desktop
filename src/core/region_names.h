#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace komira {

/// 「震源区」地名表：坐标 → 中文区域名（如「中国四川」「日本本州南岸近海」「韩国」）。
///
/// 数据来自参考项目 kanameishi 的 `src/utils/FERegions.js`（`getFEName`）：一张
/// **1° × 1° 的整格查找表**（180 × 360 格），每格给一个震源区编号，编号再查 758 条
/// 中文名。参考实现在 USGS / FSSN 这类报文里没有中文地名时正是用它补震中名
/// （`tempMsg.hypocenter = getFEName(lat, lng) || properties.place`）。
///
/// 本地震源推算只有坐标、没有报文 `placeName`，所以用同一张表、同一套名字 —— 卡片上
/// 的地名才与用户在参考产品里看到的一致，而不是自己另编一套行政区写法。
///
/// 数据由 `tools/gen_region_names.py` 生成（游程编码的 `assets/region_names.bin`），
/// 资产缺失时 `loaded() == false`，调用方退回坐标文本。刻意不依赖 Qt，以便进入
/// `run_core_tests.sh` 的纯 g++ 编译路径。
class RegionNameTable {
public:
    /// 可独立构造：单测要拿一份没被全局单例污染的表（也让「资产缺失」这条路径可测）。
    RegionNameTable() = default;

    static RegionNameTable& instance();

    bool loadFromFile(const std::string& path);
    bool loadFromMemory(const uint8_t* data, size_t size);
    bool loaded() const { return loaded_; }

    /// 坐标 → 区域名（带「附近」后缀，与参考实现同形，如「中国四川附近」）。
    /// 未加载或该格无名称时返回空串。
    ///
    /// 取整规则与参考实现逐字一致：`latI = clamp(floor(lat + 90), 0, 179)`、
    /// `lngI = clamp(floor(lng + 180), 0, 359)` —— 是 `floor`，不是四舍五入。
    std::string nameAt(double latitude, double longitude) const;

    /// 已加载的区域名条数（自检/日志用）。
    size_t nameCount() const { return names_.size(); }

private:
    std::vector<std::string> names_;
    /// 180 × 360 格，行优先；0 表示该格没有名称。
    std::vector<uint16_t> cells_;
    bool loaded_ = false;
};

} // namespace komira
