#include "core/region_names.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace komira {
namespace {

constexpr char kMagic[4] = {'K', 'Q', 'R', 'N'};
constexpr uint16_t kVersion = 1;
constexpr int kRows = 180;
constexpr int kCols = 360;
constexpr size_t kCellCount = static_cast<size_t>(kRows) * kCols;
/// 名称条数上限：数据来自 758 条的表，留足余量但挡住被改坏的长度字段。
constexpr uint32_t kMaxNames = 65'536;
constexpr uint32_t kMaxRuns = kCellCount;

/// 小端游标。资产在用户可写目录，故每次读取都先校验剩余长度。
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    size_t position() const { return pos_; }
    size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

    bool u8(uint8_t& out) {
        if (!ensure(1)) return false;
        out = data_[pos_++];
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
    bool bytes(const uint8_t*& out, size_t n) {
        if (!ensure(n)) return false;
        out = data_ + pos_;
        pos_ += n;
        return true;
    }

private:
    bool ensure(size_t n) {
        if (!ok_) return false;
        if (n > size_ || pos_ > size_ - n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace

RegionNameTable& RegionNameTable::instance() {
    static RegionNameTable table;
    return table;
}

bool RegionNameTable::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    return loadFromMemory(bytes.data(), bytes.size());
}

bool RegionNameTable::loadFromMemory(const uint8_t* data, size_t size) {
    loaded_ = false;
    names_.clear();
    cells_.clear();
    if (!data || size < 12) return false;

    Reader reader(data, size);
    const uint8_t* magic = nullptr;
    if (!reader.bytes(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint32_t nameCount = 0;
    if (!reader.u16(version) || !reader.u16(reserved) || !reader.u32(nameCount)) return false;
    if (version != kVersion || nameCount > kMaxNames) return false;

    std::vector<std::string> names;
    names.reserve(nameCount);
    for (uint32_t i = 0; i < nameCount; ++i) {
        uint8_t length = 0;
        if (!reader.u8(length)) return false;
        const uint8_t* raw = nullptr;
        if (!reader.bytes(raw, length)) return false;
        names.emplace_back(reinterpret_cast<const char*>(raw), length);
    }

    uint32_t runCount = 0;
    if (!reader.u32(runCount)) return false;
    if (runCount == 0 || runCount > kMaxRuns) return false;

    std::vector<uint16_t> cells;
    cells.reserve(kCellCount);
    for (uint32_t run = 0; run < runCount; ++run) {
        uint16_t index = 0;
        uint32_t count = 0;
        if (!reader.u16(index) || !reader.u32(count)) return false;
        if (index > names.size()) return false;
        // 总长必须正好铺满这张表：多一格少一格都说明文件被改坏了。
        if (count > kCellCount - cells.size()) return false;
        cells.insert(cells.end(), count, index);
    }
    if (cells.size() != kCellCount) return false;

    names_ = std::move(names);
    cells_ = std::move(cells);
    loaded_ = true;
    return true;
}

std::string RegionNameTable::nameAt(double latitude, double longitude) const {
    if (!loaded_ || !std::isfinite(latitude) || !std::isfinite(longitude)) return std::string();
    // 取整规则与参考实现 `getFEName` 逐字一致（floor，不是四舍五入）。
    int latIndex = static_cast<int>(std::floor(latitude + 90.0));
    int lonIndex = static_cast<int>(std::floor(longitude + 180.0));
    latIndex = std::max(0, std::min(kRows - 1, latIndex));
    lonIndex = std::max(0, std::min(kCols - 1, lonIndex));
    const uint16_t index = cells_[static_cast<size_t>(latIndex) * kCols + lonIndex];
    if (index == 0 || index > names_.size()) return std::string();
    // 参考实现无条件拼「附近」（`feNames[...] + "附近"`），这里照做：
    // 表里给的是「震源区」名（中国四川 / 日本本州南岸近海），不是精确到点的地名。
    return names_[index - 1] + "附近";
}

} // namespace komira
