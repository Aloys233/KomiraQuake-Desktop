#pragma once

#include <deque>
#include <string>

namespace komira {

/// 语句通道播放队列。串行播放，避免新播报打断尚未播完的语句。
/// 纯逻辑，不依赖 Qt，便于单元测试。
///
/// 倒计时与警报音不走本队列：它们每秒触发，长音频（intense 3.1s、hypocenter 5.4s）
/// 会堵死队列导致秒数被吞，见 AlertSoundService 的提示通道。
class SoundQueue {
public:
    /// 入队；同一路径已在队列中时不重复排队。队列满时丢弃最旧的一条。
    /// @return true 表示该条被接受
    bool enqueue(const std::string& path) {
        for (const auto& pending : items_) {
            if (pending == path) return false;
        }
        if (static_cast<int>(items_.size()) >= maxSize_) items_.pop_front();
        items_.push_back(path);
        return true;
    }

    bool empty() const { return items_.empty(); }
    int size() const { return static_cast<int>(items_.size()); }

    /// 取下一条待播项；队列为空返回空字符串。
    std::string take() {
        if (items_.empty()) return {};
        std::string front = items_.front();
        items_.pop_front();
        return front;
    }

    void clear() { items_.clear(); }

    void setMaxSize(int maxSize) {
        maxSize_ = maxSize > 0 ? maxSize : 1;
        while (static_cast<int>(items_.size()) > maxSize_) items_.pop_front();
    }

private:
    std::deque<std::string> items_;
    int maxSize_ = 16;
};

} // namespace komira