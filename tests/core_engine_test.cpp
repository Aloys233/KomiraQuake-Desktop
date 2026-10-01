// 纯 C++ 核心引擎验证（不依赖 Qt）。用 g++ 直接编译运行。
// 亦可直接执行 linux/run_core_tests.sh。

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "core/coordinate_transform.h"
#include "core/event_gate.h"
#include "core/intensity_calculator.h"
#include "core/quake_calculator.h"
#include "core/time_sync.h"
#include "core/travel_table.h"
#include "model/earthquake_event.h"

using namespace komira;

namespace {
int g_failures = 0;

void check(bool ok, const std::string& name) {
    std::printf("%s  %s\n", ok ? "[ PASS ]" : "[ FAIL ]", name.c_str());
    if (!ok) ++g_failures;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
} // namespace

int main() {
    // ---- QuakeCalculator -------------------------------------------------
    check(near(QuakeCalculator::hypocenterDistance(3.0, 4.0), 5.0), "hypocenter 3-4-5");
    const double beijingShanghai =
        QuakeCalculator::haversineDistance(39.9042, 116.4074, 31.2304, 121.4737);
    check(beijingShanghai > 1050.0 && beijingShanghai < 1090.0,
          "haversine Beijing->Shanghai ~1067km");
    const auto [p, s] = QuakeCalculator::estimateTravelTimes(0.0, 60.0);
    check(near(p, 10.0) && near(s, 60.0 / 3.5), "constant-velocity travel times");
    check(!QuakeCalculator::isValidCoordinate(95.0, 100.0), "coordinate validation");
    // 波前离开中国范围的隐藏判定：震中成都（30.66,104.06）。
    check(!QuakeCalculator::bothWavesBeyondChina(30.66, 104.06, 100.0, 80.0),
          "waves inside China stay visible");
    check(!QuakeCalculator::bothWavesBeyondChina(30.66, 104.06, 100.0, 30000.0),
          "P inside China keeps both visible");
    check(QuakeCalculator::bothWavesBeyondChina(30.66, 104.06, 30000.0, 25000.0),
          "both waves beyond China hidden");
    check(QuakeCalculator::bothWavesBeyondChina(30.66, 104.06, -1.0, -1.0),
          "out-of-range waves count as beyond China");
    // 同一地震判定：实时预警与目录发震时刻/震中一致但 ID 不同。
    const long long t0 = 1'700'000'000'000LL;
    check(QuakeCalculator::isSameQuake(t0, 25.088, 102.737, t0, 25.09, 102.73),
          "same quake matches live and catalog reports");
    check(!QuakeCalculator::isSameQuake(t0, 25.088, 102.737, t0 + 120'000, 25.09, 102.73),
          "same quake rejects later origin time");
    check(!QuakeCalculator::isSameQuake(t0, 25.088, 102.737, t0, 26.5, 102.73),
          "same quake rejects distant epicenter");

    // ---- IntensityCalculator --------------------------------------------
    check(IntensityCalculator::rawCsis(6.0, 100.0, 10.0) >
              IntensityCalculator::rawCsis(3.0, 100.0, 10.0),
          "CSIS grows with magnitude");
    check(near(IntensityCalculator::rawCsis(0.0, 10.0, 10.0), 0.0), "CSIS zero for M0");
    check(IntensityCalculator::formatCsis(4.2) == "IV", "CSIS roman IV");
    check(IntensityCalculator::formatCsis(20.0) == "XII", "CSIS roman clamp XII");
    check(IntensityCalculator::formatJma(9.0, 1.0, 1.0) == "7", "JMA band 7");

    // ---- CoordinateTransform --------------------------------------------
    const auto identity = CoordinateTransform::wgs84ToGcj02(35.0, 139.0);
    check(near(identity.first, 35.0) && near(identity.second, 139.0), "GCJ identity outside China");
    const auto gcj = CoordinateTransform::wgs84ToGcj02(30.6586, 104.0648);
    const auto back = CoordinateTransform::gcj02ToWgs84(gcj.first, gcj.second);
    check(near(back.first, 30.6586, 1e-5) && near(back.second, 104.0648, 1e-5),
          "GCJ round-trip inside China");

    // ---- TravelTable -----------------------------------------------------
    TravelTable table(
        {0.0, 10.0},
        {0.0, 100.0},
        {{0.0, 10.0}, {0.0, 20.0}},
        {{0.0, 20.0}, {0.0, 40.0}});
    check(near(table.estimateP(0.0, 50.0), 5.0), "travel P depth0 dist50 = 5s");
    check(near(table.estimateP(5.0, 50.0), 7.5), "travel P depth5 dist50 = 7.5s");
    check(near(table.estimateS(5.0, 50.0), 15.0), "travel S depth5 dist50 = 15s");
    check(near(table.estimateP(0.0, 9999.0), 10.0), "travel clamps beyond bounds");
    check(near(table.distanceForTime(0.0, 5.0, true), 50.0), "travel inverse 5s -> 50km");

    // ---- EventGate -------------------------------------------------------
    EventGate gate;
    EarthquakeEvent e1;
    e1.id = "1";
    e1.source = "wolfx";
    e1.reportNum = 1;
    e1.magnitude = 5.0;
    check(gate.admit(e1, 1000) == EventGateDecision::Pass, "gate first pass");
    check(gate.admit(e1, 2000) == EventGateDecision::Duplicate, "gate duplicate");
    EarthquakeEvent e2 = e1;
    e2.reportNum = 2;
    e2.magnitude = 5.4;
    check(gate.admit(e2, 3000) == EventGateDecision::Pass, "gate higher report pass");
    check(gate.admit(e1, 4000) == EventGateDecision::Stale, "gate lower report stale");
    EarthquakeEvent e3 = e2;
    e3.magnitude = 5.6;
    check(gate.admit(e3, 5000) == EventGateDecision::Correction, "gate same report correction");

    // ---- EarthquakeEvent derived ----------------------------------------
    EarthquakeEvent timed;
    timed.timestamp = 0;
    timed.sWaveArrival = 10'000;
    check(timed.remainingSeconds(5'000) == 5, "remainingSeconds = 5");
    check(timed.remainingSeconds(20'000) == 0, "remainingSeconds clamps at 0");

    // ---- ntp: 报文与 offset 计算《NATIVE_PORT_SPEC》 §13.3 -----------------
    const auto request = ntp::buildRequest(1'700'000'000'123LL);
    check(request.size() == static_cast<size_t>(ntp::kPacketSize), "ntp packet is 48 bytes");
    check((request[0] & 0x07) == 3, "ntp mode = 3 (client)");
    check(((request[0] >> 3) & 0x07) == 4, "ntp version = 4");
    check(ntp::toEpochMs(ntp::toNtpTimestamp(1'700'000'000'123LL)) == 1'700'000'000'123LL,
          "ntp timestamp round-trips exactly");
    check(ntp::toEpochMs(static_cast<uint64_t>(ntp::kEpochOffsetSeconds) << 32) == 0,
          "ntp epoch maps to unix epoch");
    check(!ntp::parseResponse(nullptr, 0).has_value(), "ntp rejects short packet");
    {
        std::array<uint8_t, ntp::kPacketSize> response{};
        response[0] = 36; // LI=0 VN=4 Mode=4（server）
        const long long stamps[3] = {1'700'000'000'000LL, 1'700'000'000'050LL,
                                     1'700'000'000'060LL};
        const int offsets[3] = {24, 32, 40};
        for (int k = 0; k < 3; ++k) {
            const uint64_t ts = ntp::toNtpTimestamp(stamps[k]);
            for (int i = 0; i < 8; ++i) {
                response[offsets[k] + i] = static_cast<uint8_t>((ts >> (56 - 8 * i)) & 0xFF);
            }
        }
        const auto parsed =
            ntp::parseResponse(response.data(), static_cast<int>(response.size()));
        check(parsed.has_value() && parsed->receiveMs == 1'700'000'000'050LL, "ntp parses T2");
        check(parsed.has_value() && parsed->transmitMs == 1'700'000'000'060LL, "ntp parses T3");
    }
    {
        // 本地墙钟快 30 s、单程 50 ms：offset 应恰好抵消 30 s，delay = 100 ms
        const ntp::Response response{0, 1'700'000'000'050LL, 1'700'000'000'050LL};
        const ntp::Sample sample =
            ntp::computeSample(1'700'000'030'000LL, response, 1'700'000'030'100LL);
        check(sample.offsetMs == -30'000, "ntp offset = -30000 ms");
        check(sample.delayMs == 100, "ntp delay = 100 ms");
    }
    check(!ntp::chooseBest({}).has_value(), "ntp chooseBest empty -> nullopt");
    check(!ntp::chooseBest({ntp::Sample{100, -5}}).has_value(),
          "ntp chooseBest rejects negative delay");
    {
        const auto best = ntp::chooseBest({ntp::Sample{40, 20}, ntp::Sample{30, 10}});
        check(best.has_value() && best->offsetMs == 30 && best->delayMs == 10,
              "ntp chooseBest lowest delay");
    }
    {
        const auto best = ntp::chooseBest({ntp::Sample{100, 500}, ntp::Sample{9000, 100},
                                           ntp::Sample{50, 200}, ntp::Sample{-1, 90'000}});
        check(best.has_value() && best->offsetMs == 100 && best->delayMs == 100,
              "ntp chooseBest median of 3 lowest delays");
    }
    {
        const ntp::Sample http = ntp::httpSample(1'700'000'050'000LL, 1'700'000'000'000LL,
                                                 1'700'000'001'000LL, 1'000);
        // wallMid = 1'700'000'000'500 → offset = 49'500
        check(http.offsetMs == 49'500 && http.delayMs == 1'000, "ntp http sample RTT/2 correction");
    }

    // ---- Clock: 单调锚定与状态机《NATIVE_PORT_SPEC》 §13.4 -----------------
    {
        long long mono = 10'000;
        long long wall = 1'700'000'030'000LL; // 系统墙钟快 30 s
        Clock clock([&mono]() { return mono; }, [&wall]() { return wall; });

        check(clock.state() == ClockState::Local, "clock local before sync");
        check(clock.now() == wall, "clock now falls back to wall clock");

        check(clock.applyOffset(-30'000, 42, "SNTP ntp.aliyun.com"), "clock applies first offset");
        check(clock.state() == ClockState::Synced, "clock synced after apply");
        check(clock.now() == 1'700'000'000'000LL, "clock now corrected by offset");
        check(clock.lastDelayMs() == 42, "clock records last delay");

        mono += 5'000;
        check(clock.now() == 1'700'000'005'000LL, "clock advances with monotonic clock");
        wall += 60'000;
        check(clock.now() == 1'700'000'005'000LL, "clock immune to forward wall change");
        wall -= 120'000;
        check(clock.now() == 1'700'000'005'000LL, "clock immune to backward wall change");

        check(!clock.applyOffset(-30'030, 10, "noise"), "clock ignores sub-noise offset");
        check(clock.currentOffsetMs() == -30'000, "clock keeps offset on noise sample");
        check(clock.applyOffset(-30'080, 10, "step"), "clock applies larger offset");
        check(clock.currentOffsetMs() == -30'080, "clock updates offset past noise threshold");
    }
    {
        long long mono = 0;
        Clock clock([&mono]() { return mono; }, []() { return 0LL; }, 1'000);
        clock.applyOffset(0, 5, "x");
        check(clock.state() == ClockState::Synced, "clock synced before threshold");
        mono = 1'001;
        check(clock.state() == ClockState::Stale, "clock stale past threshold");
        check(clock.now() == 1'001, "clock keeps anchoring while stale");
        clock.reset();
        check(clock.state() == ClockState::Local, "clock reset returns to local");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
