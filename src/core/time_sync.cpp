#include "core/time_sync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace komira::ntp {

namespace {

constexpr double kTwoPow32 = 4294967296.0;
constexpr int kTimestampSpace = 8;

void writeTimestamp(uint8_t* packet, int offset, long long epochMs) {
    const uint64_t timestamp = toNtpTimestamp(epochMs);
    for (int i = 0; i < kTimestampSpace; ++i) {
        packet[offset + i] = static_cast<uint8_t>((timestamp >> (56 - 8 * i)) & 0xFF);
    }
}

long long readTimestamp(const uint8_t* packet, int offset) {
    uint64_t value = 0;
    for (int i = 0; i < kTimestampSpace; ++i) {
        value = (value << 8) | static_cast<uint64_t>(packet[offset + i]);
    }
    return toEpochMs(value);
}

} // namespace

std::array<uint8_t, kPacketSize> buildRequest(long long transmitEpochMs) {
    std::array<uint8_t, kPacketSize> packet{};
    // LI=0 VN=4 Mode=3 → 0b00100011 = 35
    packet[0] = static_cast<uint8_t>((4 << 3) | 3);
    writeTimestamp(packet.data(), 40, transmitEpochMs);
    return packet;
}

std::optional<Response> parseResponse(const uint8_t* data, int size) {
    if (data == nullptr || size < kPacketSize) return std::nullopt;
    const int mode = data[0] & 0x07;
    if (mode != 4 && mode != 5) return std::nullopt;

    Response response;
    response.originateMs = readTimestamp(data, 24);
    response.receiveMs = readTimestamp(data, 32);
    response.transmitMs = readTimestamp(data, 40);
    return response;
}

Sample computeSample(long long t1Ms, const Response& response, long long t4Ms) {
    Sample sample;
    sample.offsetMs = ((response.receiveMs - t1Ms) + (response.transmitMs - t4Ms)) / 2;
    sample.delayMs = (t4Ms - t1Ms) - (response.transmitMs - response.receiveMs);
    return sample;
}

std::optional<Sample> chooseBest(const std::vector<Sample>& samples, long long maxDelayMs) {
    std::vector<Sample> usable;
    usable.reserve(samples.size());
    for (const Sample& sample : samples) {
        if (sample.valid() && sample.delayMs <= maxDelayMs) usable.push_back(sample);
    }
    if (usable.empty()) return std::nullopt;

    std::sort(usable.begin(), usable.end(),
              [](const Sample& a, const Sample& b) { return a.delayMs < b.delayMs; });
    if (usable.size() < 3) return usable.front();

    // 最低时延前三者取 offset 中位数，抗单点抖动。
    long long offsets[3] = {usable[0].offsetMs, usable[1].offsetMs, usable[2].offsetMs};
    std::sort(offsets, offsets + 3);
    return Sample{offsets[1], usable.front().delayMs};
}

uint64_t toNtpTimestamp(long long epochMs) {
    const long long shifted = epochMs + kEpochOffsetSeconds * 1000LL;
    const long long seconds = shifted >= 0 ? shifted / 1000LL : (shifted - 999LL) / 1000LL;
    const long long millis = shifted - seconds * 1000LL;
    const uint64_t fraction = static_cast<uint64_t>(
        static_cast<double>(millis) / 1000.0 * kTwoPow32) & 0xFFFFFFFFULL;
    return (static_cast<uint64_t>(seconds) << 32) | fraction;
}

long long toEpochMs(uint64_t ntpTimestamp) {
    const uint64_t seconds = ntpTimestamp >> 32;
    const uint64_t fraction = ntpTimestamp & 0xFFFFFFFFULL;
    // 四舍五入而非截断：与 toNtpTimestamp 往返无损。
    const long long millis = std::llround(static_cast<double>(fraction) / kTwoPow32 * 1000.0);
    return (static_cast<long long>(seconds) - kEpochOffsetSeconds) * 1000LL + millis;
}

Sample httpSample(long long serverMs, long long wallStartMs, long long wallEndMs,
                  long long roundTripMs) {
    const long long wallMid = wallStartMs + (wallEndMs - wallStartMs) / 2;
    return Sample{serverMs - wallMid, roundTripMs};
}

} // namespace komira::ntp

namespace komira {

namespace {

long long steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

long long systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Clock::Clock() : Clock(steadyNowMs, systemNowMs, kStaleAfterMs) {}

Clock::Clock(MonoFn mono, WallFn wall, long long staleAfterMs)
    : mono_(std::move(mono)), wall_(std::move(wall)), staleAfterMs_(staleAfterMs) {}

long long Clock::now() const {
    if (!synced_) return wall_();
    return wallAtSync_ + offsetMs_ + (mono_() - monoAtSync_);
}

long long Clock::elapsedMs() const { return mono_(); }

long long Clock::currentOffsetMs() const { return synced_ ? offsetMs_ : 0; }

ClockState Clock::state() const {
    if (!synced_) return ClockState::Local;
    // 用单调时钟判过期：系统墙钟被改动不会影响 stale 判定。
    if (mono_() - monoAtSync_ > staleAfterMs_) return ClockState::Stale;
    return ClockState::Synced;
}

bool Clock::applyOffset(long long offsetMs, long long delayMs, const std::string& sourceLabel) {
    const bool changed = !synced_ || std::llabs(offsetMs - offsetMs_) >= kNoiseMs;
    if (changed) offsetMs_ = offsetMs;
    lastDelayMs_ = delayMs;
    sourceLabel_ = sourceLabel;
    synced_ = true;
    monoAtSync_ = mono_();
    wallAtSync_ = wall_();
    lastSyncAtMs_ = wallAtSync_;
    return changed;
}

void Clock::reset() {
    synced_ = false;
    monoAtSync_ = 0;
    wallAtSync_ = 0;
    offsetMs_ = 0;
    lastSyncAtMs_ = 0;
    lastDelayMs_ = -1;
    sourceLabel_.clear();
}

} // namespace komira
