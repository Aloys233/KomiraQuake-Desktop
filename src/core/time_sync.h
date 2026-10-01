#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace komira::ntp {

/// 一次采样：`offset = serverTime − localWall`（ms），`delay` = 往返时延（ms）。
struct Sample {
    long long offsetMs = 0;
    long long delayMs = 0;

    bool valid() const { return delayMs >= 0; }
};

/// 从响应解析出的三个时间戳（epoch ms）。
struct Response {
    long long originateMs = 0; // T1 回显
    long long receiveMs = 0;   // T2
    long long transmitMs = 0;  // T3
};

/// 1900-01-01 → 1970-01-01 的秒数（NTP 纪元）。
constexpr long long kEpochOffsetSeconds = 2208988800LL;

constexpr int kPacketSize = 48;
constexpr int kPort = 123;

/// 默认丢弃阈值：往返时延超过 5 × 单次超时即视为异常样本。
constexpr long long kMaxDelayMs = 25000;

/// LI=0 / VN=4 / Mode=3（client），并把 `T1` 写入字节 40..47。
std::array<uint8_t, kPacketSize> buildRequest(long long transmitEpochMs);

/// 解析 48 字节响应；长度不足或模式不是服务端（4/5）返回 nullopt。
std::optional<Response> parseResponse(const uint8_t* data, int size);

/// `offset = ((T2 − T1) + (T3 − T4)) / 2`，`delay = (T4 − T1) − (T3 − T2)`。
Sample computeSample(long long t1Ms, const Response& response, long long t4Ms);

/// 丢弃无效/时延过大样本，按 delay 升序取最优；样本数 ≥ 3 时取最低时延前三者 offset 的中位数。
std::optional<Sample> chooseBest(const std::vector<Sample>& samples,
                                 long long maxDelayMs = kMaxDelayMs);

/// epoch ms → 64 位 NTP 时间戳（高 32 秒 + 低 32 小数）。
uint64_t toNtpTimestamp(long long epochMs);

/// 64 位 NTP 时间戳 → epoch ms。
long long toEpochMs(uint64_t ntpTimestamp);

/// HTTP 备用授时：`offset = serverMs − wallMid`，`wallMid` 取请求前后墙钟的中点。
Sample httpSample(long long serverMs, long long wallStartMs, long long wallEndMs,
                  long long roundTripMs);

} // namespace komira::ntp

namespace komira {

/// 校时状态。《NATIVE_PORT_SPEC》 §13.1 / §13.4。
enum class ClockState { Local, Synced, Stale };

/// 校时核心（纯 C++，无 Qt，可进 `run_core_tests.sh`）。
///
/// 时间以**单调时钟锚定**：校准成功时记下 `(monoAtSync, wallAtSync, offsetMs)`，之后
/// `now() = wallAtSync + offsetMs + (monoNow − monoAtSync)`。
/// 因此系统时间在两次校准之间被改动不会影响 `now()`。《NATIVE_PORT_SPEC》 §13.4。
class Clock {
public:
    using MonoFn = std::function<long long()>;
    using WallFn = std::function<long long()>;

    /// 生产构造：单调 = steady_clock，墙钟 = system_clock。
    Clock();
    /// 测试构造：注入假时钟。
    Clock(MonoFn mono, WallFn wall, long long staleAfterMs = kStaleAfterMs);

    /// 校正后的当前时刻；从未成功同步时回退本地墙钟。
    long long now() const;

    /// 单调耗时（毫秒）。用于量测往返/冷却，不用于绝对时刻。
    long long elapsedMs() const;

    /// 当前生效的 offset；未校准为 0。
    long long currentOffsetMs() const;

    ClockState state() const;

    long long lastSyncAtMs() const { return lastSyncAtMs_; }
    long long lastDelayMs() const { return lastDelayMs_; }
    const std::string& sourceLabel() const { return sourceLabel_; }

    /// 应用一次采样。[offsetMs] = serverTime − localWall。
    ///
    /// 与当前 offset 相差 < [kNoiseMs] 视为噪声：不改变 offset（避免无谓跳变），
    /// 但仍重新锚定（漂移被吸收、stale 计时重置）。
    /// @return 是否实际改动了 offset
    bool applyOffset(long long offsetMs, long long delayMs, const std::string& sourceLabel);

    void reset();

    static constexpr long long kNoiseMs = 50;
    static constexpr long long kStaleAfterMs = 30LL * 60LL * 1000LL;

private:
    MonoFn mono_;
    WallFn wall_;
    long long staleAfterMs_;
    bool synced_ = false;
    long long monoAtSync_ = 0;
    long long wallAtSync_ = 0;
    long long offsetMs_ = 0;
    long long lastSyncAtMs_ = 0;
    long long lastDelayMs_ = -1;
    std::string sourceLabel_;
};

} // namespace komira
