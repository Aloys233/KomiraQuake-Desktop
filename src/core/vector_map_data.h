#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/geo_geometry.h"

namespace komira {

/// 一个地图要素（省 / 区县 / 洲）：名称 + 全部环（首环为外环，其余为洞）。
struct VectorFeature {
    std::string name;
    GeoPolygon polygon;
};

/// 一层矢量地图数据。坐标为 WGS-84 度。
struct VectorLayer {
    std::string name;
    std::vector<VectorFeature> features;
    GeoBounds bounds;
    /// 量化参数：实际坐标 = 量化整数 * unit + origin。
    double unit = 1e-5;
    double originLon = 0.0;
    double originLat = 0.0;
};

/// 地图矢量边界数据（.kvb 二进制）。由 tools/gen_vector_map.py 生成。
///
/// 刻意不依赖 Qt（文件读取用 std::ifstream），以便进入 core_engine_test 与
/// run_core_tests.sh 的纯 g++ 编译路径。
///
/// 安全：assets/ 是拷贝到可执行文件旁、用户可写的目录，故解析全程做边界检查 ——
/// 截断或被改坏的文件必须返回失败而不是越界读。
class VectorMapData {
public:
    /// 从文件加载。失败（不存在 / 魔数不符 / 越界 / 截断）返回 false。
    bool loadFromFile(const std::string& path);
    /// 从内存加载，便于单测。数据需在返回后继续存活（解析过程中不持有引用）。
    bool loadFromMemory(const uint8_t* data, size_t size);

    bool loaded() const { return loaded_; }
    /// 取层；不存在返回 nullptr。
    const VectorLayer* layer(const std::string& name) const;
    /// 按名称查找要素（跨层搜索）。找不到返回 nullptr。
    const VectorFeature* findFeature(const std::string& layerName,
                                     const std::string& featureName) const;

    const std::vector<VectorLayer>& layers() const { return layers_; }

    /// 已加载的要素总数，用于日志与自检。
    size_t featureCount() const;

private:
    std::vector<VectorLayer> layers_;
    std::unordered_map<std::string, size_t> layerIndex_;
    bool loaded_ = false;
};

} // namespace komira