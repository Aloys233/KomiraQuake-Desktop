#include "core/vector_map_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace komira {
namespace {

constexpr char kMagic[4] = {'K', 'Q', 'V', 'B'};
constexpr uint16_t kVersion = 1;
/// 与生成器一致：墨卡托纬度截断。
constexpr double kLatLimit = 85.05112878;

/// 小端只读游标。每次读取都先检查剩余长度，越界即置失败位。
///
/// assets/ 是拷贝到可执行文件旁、用户可写的目录，文件可能被截断或改坏，
/// 因此**每个**字段读取都必须过这个检查，绝不能信任文件里的长度/偏移。
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    /// 剩余可读字节数。
    size_t remaining() const { return ok_ ? size_ - pos_ : 0; }
    /// 当前绝对偏移（用于记录特征区起点）。
    size_t position() const { return pos_; }

    bool u8(uint8_t& out) {
        if (!ensure(1)) return false;
        out = data_[pos_];
        pos_ += 1;
        return true;
    }
    bool u16(uint16_t& out) {
        if (!ensure(2)) return false;
        out = static_cast<uint16_t>(data_[pos_]) |
              static_cast<uint16_t>(static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& out) {
        if (!ensure(4)) return false;
        out = static_cast<uint32_t>(data_[pos_]) |
              (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
              (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
              (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }
    bool i32(int32_t& out) {
        uint32_t v = 0;
        if (!u32(v)) return false;
        out = static_cast<int32_t>(v);
        return true;
    }
    bool f64(double& out) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            if (!ensure(1)) return false;
            bits |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 8;
        std::memcpy(&out, &bits, sizeof(out));
        return std::isfinite(out);
    }
    /// 读定长字节串。
    bool bytes(uint8_t* out, size_t n) {
        if (!ensure(n)) return false;
        if (out) std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    /// 跳到绝对偏移（先校验目标在界内）。
    bool seek(size_t pos) {
        if (!ok_ || pos > size_) return fail();
        pos_ = pos;
        return true;
    }
    bool fail() {
        ok_ = false;
        return false;
    }

private:
    bool ensure(size_t n) {
        if (!ok_) return false;
        // pos_ + n 溢出时同样视为越界。
        if (n > size_ || pos_ > size_ - n) return fail();
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

/// 单个要素允许的最大环数/顶点数。用于挡住「长度字段被改成垃圾值」导致的巨额分配。
/// 实测最大环数 141、最大环顶点数远低于此，故留足余量。
constexpr uint32_t kMaxRings = 4096;
constexpr uint32_t kMaxPointsPerRing = 1u << 20;

} // namespace

bool VectorMapData::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    // 读入前先取长度（fileSize 可能超出一个可表示的 size_t，故先判）。
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(end));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), end)) return false;
    return loadFromMemory(buffer.data(), buffer.size());
}

bool VectorMapData::loadFromMemory(const uint8_t* data, size_t size) {
    layers_.clear();
    layerIndex_.clear();
    loaded_ = false;
    if (!data || size < 12) return false;

    Reader reader(data, size);
    uint8_t magic[4];
    if (!reader.bytes(magic, 4)) return false;
    if (std::memcmp(magic, kMagic, 4) != 0) return false;

    uint16_t version = 0;
    uint16_t layerCount = 0;
    uint32_t reserved = 0;
    if (!reader.u16(version) || !reader.u16(layerCount) || !reader.u32(reserved)) return false;
    if (version != kVersion) return false;
    // 图层数同样来自文件，需设上限，避免为垃圾值做无谓循环。
    if (layerCount == 0 || layerCount > 64) return false;

    for (uint16_t li = 0; li < layerCount; ++li) {
        VectorLayer layer;
        uint8_t nameLen = 0;
        if (!reader.u8(nameLen)) return false;
        std::string name(nameLen, '\0');
        if (nameLen > 0 && !reader.bytes(reinterpret_cast<uint8_t*>(name.data()), nameLen))
            return false;

        double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
        if (!reader.f64(minLon) || !reader.f64(minLat) || !reader.f64(maxLon) ||
            !reader.f64(maxLat))
            return false;
        double originLon = 0, originLat = 0;
        if (!reader.f64(originLon) || !reader.f64(originLat)) return false;
        int32_t step = 0;
        int32_t featureCount = 0;
        if (!reader.i32(step) || !reader.i32(featureCount)) return false;
        if (step <= 0 || featureCount < 0 || featureCount > 1'000'000) return false;

        const double unit = static_cast<double>(step) * 1e-5;

        // bbox 表（featureCount × 4 个 i32）。读入后即可用于按影响半径裁剪，
        // 无需触碰顶点数据。
        std::vector<int32_t> bbox(static_cast<size_t>(featureCount) * 4);
        for (size_t i = 0; i < bbox.size(); ++i) {
            if (!reader.i32(bbox[i])) return false;
        }

        // offset 表（featureCount+1 个 u32），最后一个应等于特征区长度。
        std::vector<uint32_t> offsets(static_cast<size_t>(featureCount) + 1);
        for (size_t i = 0; i < offsets.size(); ++i) {
            if (!reader.u32(offsets[i])) return false;
        }
        if (featureCount > 0) {
            // 偏移必须单调不减，且全部落在文件内。
            for (size_t i = 0; i < offsets.size(); ++i) {
                if (static_cast<size_t>(offsets[i]) > size) return false;
                if (i > 0 && offsets[i] < offsets[i - 1]) return false;
            }
        }

        const size_t featureBase = reader.position();
        // 特征区必须整体在文件内。
        if (featureCount > 0) {
            const size_t span = offsets.back();
            if (span > reader.remaining()) return false;
        }

        layer.name = name;
        layer.unit = unit;
        layer.originLon = originLon;
        layer.originLat = originLat;
        layer.bounds.minLon = minLon;
        layer.bounds.minLat = minLat;
        layer.bounds.maxLon = maxLon;
        layer.bounds.maxLat = maxLat;
        layer.features.reserve(static_cast<size_t>(featureCount));

        for (int32_t fi = 0; fi < featureCount; ++fi) {
            // 块内游标：从 offset[fi] 起读，且不得超过 offset[fi+1]。
            if (!reader.seek(featureBase + offsets[fi])) return false;
            const size_t blockEnd = featureBase + offsets[fi + 1];

            VectorFeature feature;
            uint8_t fnameLen = 0;
            if (!reader.u8(fnameLen)) return false;
            feature.name.assign(fnameLen, '\0');
            if (fnameLen > 0 &&
                !reader.bytes(reinterpret_cast<uint8_t*>(feature.name.data()), fnameLen))
                return false;

            uint16_t ringCount = 0;
            if (!reader.u16(ringCount)) return false;
            if (ringCount > kMaxRings) return false;
            feature.polygon.reserve(ringCount);

            for (uint16_t ri = 0; ri < ringCount; ++ri) {
                uint32_t pointCount = 0;
                if (!reader.u32(pointCount)) return false;
                if (pointCount < 3 || pointCount > kMaxPointsPerRing) return false;
                // 读前先确认这一环的字节数在本块内。
                if (pointCount > (blockEnd - reader.remaining()) / 8) return false;

                GeoRing ring;
                ring.reserve(pointCount);
                for (uint32_t pi = 0; pi < pointCount; ++pi) {
                    int32_t qx = 0;
                    int32_t qy = 0;
                    if (!reader.i32(qx) || !reader.i32(qy)) return false;
                    const double lon = static_cast<double>(qx) * unit + originLon;
                    const double lat = static_cast<double>(qy) * unit + originLat;
                    // GeoRing 是 (lat, lon) 顺序 —— 与 geo_geometry 的取值口径一致。
                    ring.emplace_back(std::max(-kLatLimit, std::min(kLatLimit, lat)), lon);
                }
                feature.polygon.push_back(std::move(ring));
            }
            layer.features.push_back(std::move(feature));
        }

        // 游标复位到本层特征区末尾，继续读下一层。
        if (featureCount > 0) {
            if (!reader.seek(featureBase + offsets.back())) return false;
        }

        layerIndex_.emplace(layer.name, layers_.size());
        layers_.push_back(std::move(layer));
    }

    loaded_ = true;
    return true;
}

const VectorLayer* VectorMapData::layer(const std::string& name) const {
    if (!loaded_) return nullptr;
    const auto it = layerIndex_.find(name);
    if (it == layerIndex_.end()) return nullptr;
    return &layers_[it->second];
}

const VectorFeature* VectorMapData::findFeature(const std::string& layerName,
                                                const std::string& featureName) const {
    const VectorLayer* l = layer(layerName);
    if (!l) return nullptr;
    for (const VectorFeature& f : l->features) {
        if (f.name == featureName) return &f;
    }
    return nullptr;
}

size_t VectorMapData::featureCount() const {
    size_t total = 0;
    for (const VectorLayer& l : layers_) total += l.features.size();
    return total;
}

} // namespace komira