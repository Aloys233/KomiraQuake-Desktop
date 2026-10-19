// 纯 C++ 核心引擎验证（不依赖 Qt）。用 g++ 直接编译运行。
// 亦可直接执行 linux/run_core_tests.sh。

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "core/coordinate_transform.h"
#include "core/event_gate.h"
#include "core/geo_geometry.h"
#include "core/intensity_calculator.h"
#include "core/polygon_tessellator.h"
#include "core/quake_calculator.h"
#include "core/region_names.h"
#include "core/source_estimator.h"
#include "core/sound_queue.h"
#include "core/time_sync.h"
#include "core/travel_table.h"
#include "core/vector_map_data.h"
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

/// argv[1] 可选，为 .kvb 矢量数据路径；缺省则跳过该段。
int main(int argc, char** argv) {
    // ---- QuakeCalculator -------------------------------------------------
    check(near(QuakeCalculator::hypocenterDistance(3.0, 4.0), 5.0), "hypocenter 3-4-5");
    const double beijingShanghai =
        QuakeCalculator::haversineDistance(39.9042, 116.4074, 31.2304, 121.4737);
    check(beijingShanghai > 1050.0 && beijingShanghai < 1090.0,
          "haversine Beijing->Shanghai ~1067km");
    const auto [p, s] = QuakeCalculator::estimateTravelTimes(0.0, 60.0);
    check(near(p, 10.0) && near(s, 60.0 / 3.5), "constant-velocity travel times");
    check(!QuakeCalculator::isValidCoordinate(95.0, 100.0), "coordinate validation");
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
    // CEA-CSIS 衰减关系（kanameishi calcCsis）固定值。
    check(near(IntensityCalculator::rawCsis(6.0, 100.0, 10.0), 4.2749, 1e-3), "CSIS M6 @100km");
    check(near(IntensityCalculator::rawCsis(7.0, 0.0, 10.0), 9.3048, 1e-3), "CSIS M7 at epicenter");
    check(IntensityCalculator::formatCsis(4.2) == "IV", "CSIS roman IV");
    check(IntensityCalculator::formatCsis(20.0) == "XII", "CSIS roman clamp XII");
    // 仪器烈度：GB/T 17742-2020 由 PGA（Gal）换算，与设备端同一式子；参考点
    // 0.22→1.0、0.89→2.0、3.53→3.0、14.1→4.0，静稳（<=0.01）为 0，上限 12。
    check(near(IntensityCalculator::csisFromPga(0.22), 1.0), "instrument intensity 1 at 0.22 Gal");
    check(near(IntensityCalculator::csisFromPga(0.89), 2.0), "instrument intensity 2 at 0.89 Gal");
    check(near(IntensityCalculator::csisFromPga(3.53), 3.0), "instrument intensity 3 at 3.53 Gal");
    check(near(IntensityCalculator::csisFromPga(14.1), 4.0), "instrument intensity 4 at 14.1 Gal");
    check(near(IntensityCalculator::csisFromPga(0.11), 0.5), "instrument intensity linear below 0.22");
    check(near(IntensityCalculator::csisFromPga(0.005), 0.0), "instrument intensity zero when quiet");
    check(near(IntensityCalculator::csisFromPga(0.0), 0.0), "csis zero when no PGA");
    check(near(IntensityCalculator::csisFromPga(1e9), 12.0), "instrument intensity capped at 12");
    check(IntensityCalculator::formatJma(9.0, 1.0, 1.0) == "7", "JMA band 7");
    // 波前影响半径（CSIS 可感下限 I）与分段渐隐。
    const double r6 = IntensityCalculator::distanceForCsis(6.0, 10.0, 1.0);
    check(r6 > 0.0 && near(IntensityCalculator::rawCsis(6.0, r6, 10.0), 1.0, 0.02),
          "distanceForCsis hits CSIS I");
    check(IntensityCalculator::distanceForCsis(7.0, 10.0, 1.0) >
              IntensityCalculator::distanceForCsis(4.0, 10.0, 1.0),
          "fade radius grows with magnitude");
    check(near(IntensityCalculator::distanceForCsis(0.0, 10.0, 1.0), 0.0), "fade radius zero for M0");
    check(near(IntensityCalculator::waveOpacity(950.0, 1000.0), 1.0),
          "wave opacity stays full through first 95 percent");
    check(IntensityCalculator::waveOpacity(990.0, 1000.0) > 0.0,
          "wave opacity fades only in final 5 percent");
    check(near(IntensityCalculator::waveOpacity(1000.0, 1000.0), 0.0),
          "wave opacity ends at CSIS I");
    check(near(IntensityCalculator::waveOpacity(10000.0, 1000.0), 0.0), "wave opacity 0 at hard max");
    check(near(IntensityCalculator::waveOpacity(12000.0, 1000.0), 0.0), "wave opacity 0 beyond hard max");
    // S 波填充只在影响半径内可见，且由 0.25 递减到 0。
    check(near(IntensityCalculator::waveFillOpacity(200.0, 1000.0), 0.25), "S fill 0.25 inside 0.8fade");
    check(near(IntensityCalculator::waveFillOpacity(1000.0, 1000.0), 0.0), "S fill 0 at fade");
    check(near(IntensityCalculator::waveFillOpacity(1500.0, 1000.0), 0.0), "S fill hidden beyond fade");

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

    // ---- SoundQueue: 串行播报，新语句不得打断未播完的语句 ---------------
    {
        SoundQueue queue;
        check(queue.empty(), "sound queue starts empty");
        queue.enqueue("issue");
        queue.enqueue("warn");
        check(queue.size() == 2, "two clips queued");
        check(queue.take() == "issue", "queue preserves arrival order");
        check(!queue.empty(), "still pending after first take");
        check(queue.take() == "warn", "second clip follows first");
        check(queue.take().empty(), "take on empty queue yields nothing");

        queue.enqueue("hypocenter");
        check(!queue.enqueue("hypocenter"), "duplicate clip is not queued twice");
        check(queue.size() == 1, "duplicate enqueue keeps queue size");

        queue.clear();
        check(queue.empty(), "clear empties the queue");

        SoundQueue bounded;
        bounded.setMaxSize(3);
        for (int i = 0; i < 5; ++i) bounded.enqueue("clip" + std::to_string(i));
        check(bounded.size() == 3, "queue honours max size");
        check(bounded.take() == "clip2", "overflow drops the oldest clips");

        // 语句通道积压时后续语句仍须完整播出：长音频只推迟，不截断。
        SoundQueue backlog;
        backlog.enqueue("issue");
        backlog.enqueue("warn");
        backlog.enqueue("hypocenter");
        check(backlog.size() == 3, "statements queue instead of interrupting");
        check(backlog.take() == "issue", "first statement plays first");
        check(backlog.take() == "warn", "second statement is not cut off");
        check(backlog.take() == "hypocenter", "third statement is not cut off");
    }

    // ---- GeoGeometry -----------------------------------------------------
    {
        // 1° 见方的闭合环（首尾点相同）。
        GeoRing square = {{0.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
        GeoPolygon face = {square};

        check(GeoGeometry::pointInPolygon(face, 0.5, 0.5), "PIP: centre is inside");
        check(!GeoGeometry::pointInPolygon(face, 1.5, 0.5), "PIP: north of face is outside");
        check(!GeoGeometry::pointInPolygon(face, 0.5, -0.5), "PIP: west of face is outside");
        // 顶点与边上的点：射线法在恰好落在边上的情形是任意的（测度为零），
        // 故只断言明显在面外的点，不锁定边界语义。
        check(!GeoGeometry::pointInPolygon(face, -0.1, 0.5), "PIP: below bottom edge is outside");
        check(!GeoGeometry::pointInPolygon(face, 0.5, 1.1), "PIP: right of face is outside");

        // 洞：外环 0..3，其内挖一个 1..2 的洞。洞内必须判为「不在」——
        // 若用「任一环命中即命中」会误判为在内，区县着色就会把洞画成实心。
        GeoRing outer = {{0.0, 0.0}, {0.0, 3.0}, {3.0, 3.0}, {3.0, 0.0}, {0.0, 0.0}};
        GeoRing hole = {{1.0, 1.0}, {1.0, 2.0}, {2.0, 2.0}, {2.0, 1.0}, {1.0, 1.0}};
        GeoPolygon withHole = {outer, hole};
        check(GeoGeometry::pointInPolygon(withHole, 0.5, 0.5), "PIP: solid part is inside");
        check(!GeoGeometry::pointInPolygon(withHole, 1.5, 1.5), "PIP: inside hole is NOT inside");
        // 洞不属于该区县，故距离取到洞边界（0.5°）而非 0。
        check(near(GeoGeometry::distanceToPolygonKm(withHole, 1.5, 1.5), 0.5 * 111.32, 0.5),
              "distance: point in hole measures to hole boundary");

        // 距离：正北 0.5° ≈ 55.66 km（区内则为 0）。
        check(near(GeoGeometry::distanceToPolygonKm(face, 0.5, 0.5), 0.0), "distance: inside is 0");
        const double north = GeoGeometry::distanceToPolygonKm(face, 0.5, 1.5);
        check(near(north, 0.5 * 111.32, 0.5), "distance: 0.5deg north ~55.7km");
        // 角点外侧：到最近角点的距离。
        const double corner = GeoGeometry::distanceToPolygonKm(face, -0.5, -0.5);
        check(near(corner, std::sqrt(2.0) * 0.5 * 111.32, 0.5), "distance: diagonal to corner");
        // 经度方向按 cos(lat) 收缩：同一 0.5° 经差，高纬比赤道短。
        // 面须跨过查询点所在纬度，否则最近点是角点而非正东的边。
        const double eastEquator = GeoGeometry::distanceToPolygonKm(face, 0.0, 1.5);
        check(near(eastEquator, 0.5 * 111.32, 0.5), "distance: east at equator has no shrink");
        GeoRing midLat = {{40.0, 0.0}, {40.0, 1.0}, {41.0, 1.0}, {41.0, 0.0}, {40.0, 0.0}};
        GeoPolygon midFace = {midLat};
        const double east40 = GeoGeometry::distanceToPolygonKm(midFace, 40.5, 1.5);
        check(near(east40, 0.5 * 111.32 * std::cos(40.5 * M_PI / 180.0), 0.5),
              "distance: east at 40N shrinks by cos(lat)");

        // 点到线段：投影落在线段内部 vs 落在延长线上（后者应夹到端点）。
        check(near(GeoGeometry::distanceToSegmentKm(0.0, 0.0, 1.0, 0.0, 0.5, 0.5),
                   0.5 * 111.32, 0.5), "segment: perpendicular foot inside");
        // 点 (5,0) 在段 (0,0)-(1,0) 的延长线上：应夹到端点 (1,0)，距离 4° ≈ 445 km。
        check(near(GeoGeometry::distanceToSegmentKm(0.0, 0.0, 1.0, 0.0, 5.0, 0.0),
                   4.0 * 111.32, 0.5),
              "segment: collinear extension clamps to endpoint");

        // 包围盒与保守距离上界。
        const GeoBounds bounds = boundsOf(face);
        check(bounds.minLat == 0.0 && bounds.maxLat == 1.0 && bounds.minLon == 0.0 &&
                  bounds.maxLon == 1.0,
              "bounds: square extent");
        check(bounds.contains(0.5, 0.5) && !bounds.contains(1.5, 0.5), "bounds: contains test");
        // 上界必须 >= 真实距离，否则剪枝会漏掉受影响区县；它是包围盒对角线，刻意保守。
        check(bounds.maxDistanceKm(0.5, 1.5) >= north, "bounds: upper bound covers real distance");
        check(bounds.maxDistanceKm(0.5, 1.5) < north * 4.0, "bounds: upper bound stays bounded");
        // 远离本面的查询点，上界应显著大于 0（否则剪枝会误杀）。
        check(boundsOf(face).maxDistanceKm(30.0, 120.0) > 1000.0, "bounds: far query is large");
    }

    // ---- VectorMapData ----------------------------------------------------
    // 矢量数据文件路径由命令行传入（argv[1]）：core 层刻意不依赖 Qt，
    // 故不能在测试里用 QCoreApplication 推导 assets/ 位置。
    if (argc > 1) {
        VectorMapData map;
        check(map.loadFromFile(argv[1]), "kvb: loads real asset file");
        check(map.loaded(), "kvb: reports loaded");
        check(map.featureCount() == 1285, "kvb: 1285 features (239+35+1011)");
        check(map.layers().size() == 3, "kvb: 3 layers");

        // 区县层是 EEW 烈度着色的数据源，结构须符合预期。
        const VectorLayer* district = map.layer("district");
        check(district != nullptr, "kvb: district layer present");
        if (district) {
            check(district->features.size() == 1011, "kvb: 1011 districts");
            check(!district->features.empty() && district->features[0].name == "东城区",
                  "kvb: first district name round-trips");
            // 洞必须保留：175 个区县是多环，填洞会让底图出现错误色块。
            int multiRing = 0;
            for (const VectorFeature& f : district->features) {
                if (f.polygon.size() > 1) ++multiRing;
            }
            check(multiRing == 176, "kvb: multi-ring districts preserved (holes kept)");
            // 全部环显式闭合，且点数 >= 4。
            bool allClosed = true;
            for (const VectorFeature& f : district->features) {
                for (const GeoRing& r : f.polygon) {
                    if (r.size() < 4 || r.front() != r.back()) allClosed = false;
                }
            }
            check(allClosed, "kvb: every ring explicitly closed");
            // 坐标落在中国的经纬范围内，且经度为正（量化原点减法未出错）。
            bool inRange = true;
            for (const VectorFeature& f : district->features) {
                for (const GeoRing& r : f.polygon) {
                    for (const auto& pt : r) {
                        if (pt.first < 3.0 || pt.first > 54.0 || pt.second < 73.0 ||
                            pt.second > 136.0)
                            inRange = false;
                    }
                }
            }
            check(inRange, "kvb: district coords dequantize into China's bbox");
        }

        // 世界层：不再输出日界 +360° 副本（改由两端渲染按视口环绕），
        // 并补齐了上游 global 剔除、需单独渲染的中/日陆地。
        const VectorLayer* world = map.layer("world");
        check(world != nullptr, "kvb: world layer present");
        if (world) {
            bool anyBeyond = false;
            for (const VectorFeature& f : world->features)
                for (const GeoRing& r : f.polygon)
                    for (const auto& pt : r)
                        if (pt.second > 180.0001 || pt.second < -180.0001) anyBeyond = true;
            check(!anyBeyond, "kvb: world layer has no antimeridian duplicates (>180)");

            const VectorFeature* cn = map.findFeature("world", "中国");
            const VectorFeature* jp = map.findFeature("world", "日本");
            check(cn != nullptr, "kvb: world layer includes China land");
            check(jp != nullptr, "kvb: world layer includes Japan land");
            if (cn) check(GeoGeometry::pointInPolygon(cn->polygon, 25.0, 121.5),  // 台北
                          "kvb: China land covers Taiwan");
            if (jp) check(GeoGeometry::pointInPolygon(jp->polygon, 35.7, 139.7),  // 东京
                          "kvb: Japan land covers Tokyo");
        }

        check(map.findFeature("district", "东城区") != nullptr, "kvb: findFeature by name");
        check(map.findFeature("district", "不存在的区") == nullptr,
              "kvb: findFeature misses cleanly");
        check(map.layer("nope") == nullptr, "kvb: unknown layer returns nullptr");

        // 损坏输入必须干净地失败，而不是越界读。assets/ 是用户可写目录。
        VectorMapData bad;
        check(!bad.loadFromFile("/nonexistent/map_bounds.kvb"), "kvb: missing file rejected");
        check(!bad.loaded(), "kvb: failed load leaves unloaded state");
        const uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        check(!bad.loadFromMemory(junk, sizeof(junk)), "kvb: bad magic rejected");
        check(!bad.loadFromMemory(nullptr, 0), "kvb: null buffer rejected");
        // 真文件截断到一半：各字段长度不再自洽，必须失败。
        std::ifstream real(argv[1], std::ios::binary);
        std::vector<uint8_t> all((std::istreambuf_iterator<char>(real)),
                                 std::istreambuf_iterator<char>());
        if (all.size() > 4096) {
            VectorMapData trunc;
            check(!trunc.loadFromMemory(all.data(), all.size() / 2), "kvb: truncated file rejected");
            VectorMapData headerOnly;
            check(!headerOnly.loadFromMemory(all.data(), 12), "kvb: header-only file rejected");
        }
    }

    // ---- RegionNameTable（震中地名，对齐参考实现 getFEName） --------------
    // 数据文件路径同样由命令行传入（argv[1] 是 .kvb 的同级目录，region_names.bin 在它旁边）。
    {
        // 1° 网格的取整规则与参考实现逐字一致（floor，不是四舍五入）：
        // 用「同一格内两个不同位置必须同名、跨格必须换名」来钉住这条规则。
        const std::string dir = argc > 1 ? std::string(argv[1]) : std::string();
        const size_t slash = dir.find_last_of('/');
        const std::string assetDir = slash == std::string::npos ? std::string()
                                                                : dir.substr(0, slash + 1);
        RegionNameTable names;
        if (!assetDir.empty()) {
            const bool loaded = names.loadFromFile(assetDir + "region_names.bin");
            check(loaded, "region names: loads real asset file");
            check(names.loaded(), "region names: reports loaded");
            check(names.nameCount() == 758, "region names: 758 canonical names");

            // 参考实现 `getFEName(lat, lng)`：floor 取整后查 180×360 表，再拼「附近」。
            check(names.nameAt(30.0, 104.0) == "中国四川附近",
                  "region names: Sichuan grid cell");
            check(names.nameAt(35.7, 139.7) == "日本本州南岸近海附近",
                  "region names: Tokyo grid cell");
            check(names.nameAt(37.5, 127.0) == "韩国附近", "region names: Korea grid cell");
            check(names.nameAt(23.0, 120.3) == "中国台湾附近",
                  "region names: Taiwan grid cell");
            // 同一格内的不同点必须同名（floor 语义），跨格必须换名（不能退化成固定值）。
            // 取值点选在 139°E 的日本本州：35.9°N 与 36.1°N 分属「本州南岸近海」与
            // 「本州东部」两格；四川那一格很大，相邻格往往仍是「中国四川」，不适合做反例。
            check(names.nameAt(30.1, 104.9) == names.nameAt(30.9, 104.1),
                  "region names: same 1-degree cell returns the same name");
            check(names.nameAt(35.9, 139.0) != names.nameAt(36.1, 139.0),
                  "region names: adjacent cells differ");
            // 边界夹取：极区与日界线附近不得越界读。
            check(!names.nameAt(89.9, 179.9).empty(), "region names: north pole cell clamps");
            check(!names.nameAt(-89.9, -179.9).empty(), "region names: south pole cell clamps");
            check(names.nameAt(0.0, 0.0).size() > 2, "region names: null island has a name");
            // 非有限坐标必须干净地返回空串，而不是拿 NaN 去转 int。
            check(names.nameAt(std::numeric_limits<double>::quiet_NaN(), 0.0).empty(),
                  "region names: NaN latitude rejected");
            check(names.nameAt(0.0, std::numeric_limits<double>::infinity()).empty(),
                  "region names: infinite longitude rejected");

            // 损坏输入：错魔数 / 截断 / 长度字段自洽但总格数不对，都必须失败。
            std::ifstream bin(assetDir + "region_names.bin", std::ios::binary);
            std::vector<uint8_t> raw((std::istreambuf_iterator<char>(bin)),
                                     std::istreambuf_iterator<char>());
            RegionNameTable bad;
            check(!bad.loadFromMemory(nullptr, 0), "region names: null buffer rejected");
            const uint8_t junk[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
            check(!bad.loadFromMemory(junk, sizeof(junk)), "region names: bad magic rejected");
            if (raw.size() > 4096) {
                check(!bad.loadFromMemory(raw.data(), raw.size() / 2),
                      "region names: truncated file rejected");
                check(!bad.loadFromMemory(raw.data(), 12),
                      "region names: header-only file rejected");
            }
        }
    }

    // ---- SourceEstimator（本地震源推算内核） -----------------------------
    {
        // 合成强度场：点源 + CSIS 衰减关系，测站 0.2° 网格覆盖震中周围 ±1.6°。
        // 注意用**同一套**衰减关系先生成，再检验能否反推回原震中 —— 这是自洽性校验，
        // 不是精度宣称（真实台的场地效应、路径异常不在模型内）。
        const auto synthGrid = [](double lat, double lon, double magnitude, double depth) {
            std::vector<StationObservation> out;
            for (double y = lat - 1.6; y <= lat + 1.6 + 1e-9; y += 0.2) {
                for (double x = lon - 1.6; x <= lon + 1.6 + 1e-9; x += 0.2) {
                    StationObservation s;
                    s.latitude = y;
                    s.longitude = x;
                    s.family = IntensityFamily::Csis;
                    s.intensity = SourceEstimator::predictedIntensity(
                        IntensityFamily::Csis, magnitude,
                        QuakeCalculator::haversineDistance(lat, lon, y, x), depth);
                    out.push_back(s);
                }
            }
            return out;
        };

        SourceEstimatorConfig cfg;
        const auto grid = synthGrid(35.0, 139.0, 5.8, 10.0);
        const SourceEstimate e = SourceEstimator::estimate(grid, cfg);
        const double error = QuakeCalculator::haversineDistance(e.latitude, e.longitude, 35.0, 139.0);
        check(e.valid, "source estimate: valid on a synthetic CSIS field");
        check(error < 10.0, "source estimate: epicentre within 10 km");
        check(std::fabs(e.magnitude - 5.8) < 0.4, "source estimate: magnitude round-trips");
        check(near(e.depthKm, 10.0), "source estimate: depth picks the matching candidate");
        check(e.stationCount > 50, "source estimate: counts triggered stations");
        check(e.confidence > 0.5 && e.confidence <= 1.0, "source estimate: confidence in range");
        check(e.grade == "S" || e.grade == "A" || e.grade == "B",
              "source estimate: well-covered field grades A or better");

        // 平移不变：把整片强度场减去 3 个烈度单位，位置必须不变（衰减模型只有 a 平移）。
        std::vector<StationObservation> shifted = grid;
        for (auto& s : shifted) s.intensity = std::max(0.0, s.intensity - 3.0);
        const SourceEstimate shiftedEstimate = SourceEstimator::estimate(shifted, cfg);
        check(shiftedEstimate.valid, "source estimate: still valid after intensity shift");
        check(QuakeCalculator::haversineDistance(shiftedEstimate.latitude, shiftedEstimate.longitude,
                                                 e.latitude, e.longitude) < 5.0,
              "source estimate: epicentre invariant to a uniform intensity shift");

        // 站点不够：低于发布门槛时必须拒绝，不能给出一个"看起来挺像"的解。
        std::vector<StationObservation> few(grid.begin(), grid.begin() + 3);
        check(!SourceEstimator::estimate(few, cfg).valid,
              "source estimate: rejects below the station floor");

        // 无梯度（全域同一强度）：任何位置都"最优"，必须拒绝。
        std::vector<StationObservation> flat;
        for (int i = -3; i <= 3; ++i) {
            for (int j = -3; j <= 3; ++j) {
                StationObservation s;
                s.latitude = 35.0 + i * 0.1;
                s.longitude = 139.0 + j * 0.1;
                s.intensity = 1.0;
                flat.push_back(s);
            }
        }
        check(!SourceEstimator::estimate(flat, cfg).valid,
              "source estimate: rejects a gradient-free field");

        // 缺测哨兵必须被剔除：混进 -1 / -3 / 99 不能污染结果。
        std::vector<StationObservation> dirty = grid;
        for (std::size_t i = 0; i < dirty.size(); i += 7) dirty[i].intensity = -1.0;
        for (std::size_t i = 3; i < dirty.size(); i += 11) dirty[i].intensity = -3.0;
        for (std::size_t i = 5; i < dirty.size(); i += 13) dirty[i].intensity = 99.0;
        const SourceEstimate dirtyEstimate = SourceEstimator::estimate(dirty, cfg);
        check(dirtyEstimate.valid, "source estimate: survives missing-value sentinels");
        check(QuakeCalculator::haversineDistance(dirtyEstimate.latitude, dirtyEstimate.longitude,
                                                 e.latitude, e.longitude) < 15.0,
              "source estimate: sentinels do not move the epicentre");

        // 单侧覆盖：只有东侧测站时解会偏，但必须给出有效解并把方位空隙报出来
        // （UI 据此降级展示），不能因为"不完美"就静默失败。
        std::vector<StationObservation> oneSided;
        for (const auto& s : grid) if (s.longitude >= 139.0) oneSided.push_back(s);
        const SourceEstimate oneSidedEstimate = SourceEstimator::estimate(oneSided, cfg);
        check(oneSidedEstimate.valid, "source estimate: one-sided coverage still publishes");
        check(oneSidedEstimate.azimuthGapDeg > 90.0, "source estimate: reports one-sided geometry");
        check(oneSidedEstimate.confidence < e.confidence,
              "source estimate: one-sided coverage lowers confidence");

        // JMA 制式必须用 JMA 的衰减关系反演震级（与 CSIS 差 0.5 级以上）。
        SourceEstimatorConfig jmaCfg;
        jmaCfg.family = IntensityFamily::Jma;
        std::vector<StationObservation> jma;
        for (double y = 33.4; y <= 36.6; y += 0.2) {
            for (double x = 137.4; x <= 140.6; x += 0.2) {
                StationObservation s;
                s.latitude = y;
                s.longitude = x;
                s.family = IntensityFamily::Jma;
                s.intensity = std::max(0.0, SourceEstimator::predictedIntensity(
                                                IntensityFamily::Jma, 7.0,
                                                QuakeCalculator::haversineDistance(35.0, 139.0, y, x),
                                                10.0));
                jma.push_back(s);
            }
        }
        const SourceEstimate jmaEstimate = SourceEstimator::estimate(jma, jmaCfg);
        check(jmaEstimate.valid, "source estimate: JMA field produces an estimate");
        check(std::fabs(jmaEstimate.magnitude - 7.0) < 0.5, "source estimate: JMA magnitude round-trips");

        // 制式哨兵：-1 / -3 / 99 / NaN 一律不可用。
        check(!SourceEstimator::usableIntensity(IntensityFamily::Csis, -1.0),
              "source estimate: CSIS missing sentinel rejected");
        check(!SourceEstimator::usableIntensity(IntensityFamily::Jma, -3.0),
              "source estimate: JMA missing sentinel rejected");
        check(!SourceEstimator::usableIntensity(IntensityFamily::Jma, 99.0),
              "source estimate: JMA saturated sentinel rejected");
        check(!SourceEstimator::usableIntensity(IntensityFamily::Kma, -3.0),
              "source estimate: KMA missing sentinel rejected");
        check(!SourceEstimator::usableIntensity(IntensityFamily::Csis,
                                                std::numeric_limits<double>::quiet_NaN()),
              "source estimate: NaN rejected");
        check(SourceEstimator::usableIntensity(IntensityFamily::Csis, 4.0),
              "source estimate: normal CSIS value accepted");

        // 门槛下调到 1 站：单站也必须给一个可用解，但必须是**明确标注的粗解**
        // （置信度封顶、不确定度百公里级、provisional=true），不能伪装成多点解。
        {
            SourceEstimatorConfig loose = cfg;
            loose.minStations = 1;
            loose.minSearchStations = 1;
            // 取触发站里最强的那一站单独求解。
            std::vector<StationObservation> one;
            const StationObservation* strongest = nullptr;
            for (const auto& s : grid) {
                if (s.intensity < cfg.triggerLevel) continue;
                if (!strongest || s.intensity > strongest->intensity) strongest = &s;
            }
            check(strongest != nullptr, "source estimate: found a strongest trigger");
            if (!strongest) return 1;
            one.push_back(*strongest);
            const SourceEstimate single = SourceEstimator::estimate(one, loose);
            check(single.valid, "source estimate: single station still yields a solution");
            check(single.provisional, "source estimate: single station is flagged provisional");
            check(single.grade == "D", "source estimate: single station never grades above D");
            check(single.confidence <= 0.35, "source estimate: single station confidence is capped");
            check(single.uncertaintyKm >= 60.0,
                  "source estimate: single station uncertainty stays large");
            // 位置必须仍在触发站附近（不是随便乱跳）。
            check(QuakeCalculator::haversineDistance(single.latitude, single.longitude,
                                                     strongest->latitude, strongest->longitude) < 60.0,
                  "source estimate: single station stays near the trigger");
            // 默认门槛（6）下同一帧单站必须被拒绝。
            check(!SourceEstimator::estimate(one, cfg).valid,
                  "source estimate: default floor still rejects a single station");

            // 两站同理：仍然 provisional，且不确定度比单站小。
            std::vector<StationObservation> two = one;
            for (const auto& s : grid) {
                if (s.intensity >= cfg.triggerLevel
                    && (s.latitude != strongest->latitude || s.longitude != strongest->longitude)) {
                    two.push_back(s);
                    break;
                }
            }
            const SourceEstimate pair = SourceEstimator::estimate(two, loose);
            check(pair.valid && pair.provisional, "source estimate: two stations stay provisional");
            check(pair.uncertaintyKm <= single.uncertaintyKm + 1.0,
                  "source estimate: two stations tighten the uncertainty");
        }

        // 同时两场地震：两片触发区相距约 1200 km（四川 / 云南量级）。绝不能把它们
        // 拟合成中间那个"不存在"的震中 —— 那是最容易骗过眼睛的一种错。
        {
            const auto fieldA = synthGrid(30.0, 103.0, 6.0, 10.0);
            const auto fieldB = synthGrid(25.0, 102.5, 5.2, 10.0);
            std::vector<StationObservation> both;
            both.reserve(fieldA.size() + fieldB.size());
            for (const auto& s : fieldA) {
                if (s.intensity >= cfg.triggerLevel) both.push_back(s);
            }
            for (const auto& s : fieldB) {
                if (s.intensity >= cfg.triggerLevel) both.push_back(s);
            }
            const SourceEstimate split = SourceEstimator::estimate(both, cfg);
            check(split.valid, "source estimate: two concurrent quakes still publishes one");
            check(split.clusterCount >= 2, "source estimate: separates concurrent quakes");
            check(split.otherClusterCount >= 1,
                  "source estimate: reports the other triggered cluster");
            // 解必须落在其中**一个**真实震中附近，而不是两地之间。
            const double toA = QuakeCalculator::haversineDistance(split.latitude, split.longitude,
                                                                  30.0, 103.0);
            const double toB = QuakeCalculator::haversineDistance(split.latitude, split.longitude,
                                                                  25.0, 102.5);
            check(std::min(toA, toB) < 30.0,
                  "source estimate: concurrent quakes resolve to one real epicentre");
            check(std::max(toA, toB) > 400.0,
                  "source estimate: concurrent quakes do not average into a phantom epicentre");
            // 更强的那个（M6.0，支撑站更多）应当胜出。
            check(std::fabs(split.magnitude - 6.0) < std::fabs(split.magnitude - 5.2),
                  "source estimate: stronger cluster wins");
        }

        // 远场未触发站不得污染本地求解：追加一大批 2000 km 外的"未触发"站，
        // 解必须保持不动（负证据只取候选附近）。
        {
            std::vector<StationObservation> withFaraway = grid;
            for (int i = 0; i < 200; ++i) {
                StationObservation s;
                s.latitude = 10.0 + (i % 20) * 0.4;
                s.longitude = 100.0 + (i / 20) * 0.4;
                s.intensity = 0.0;
                withFaraway.push_back(s);
            }
            const SourceEstimate polluted = SourceEstimator::estimate(withFaraway, cfg);
            check(polluted.valid, "source estimate: far-away quiet stations keep a valid solution");
            check(QuakeCalculator::haversineDistance(polluted.latitude, polluted.longitude,
                                                     e.latitude, e.longitude) < 5.0,
                  "source estimate: far-away quiet stations do not move the epicentre");
        }

        // ---- 区域拆分：区域只决定用哪套参数、结果算谁的，不改变拟合本身 ----
        {
            SourceRegion region = SourceRegion::China;
            // 台湾的 Wolfx SeisJS 站（CSIS）必须单独成区：接入 CWA 之前就要能单独算，
            // 不能和大陆台网混在一套参数里。
            check(SourceEstimator::estimate({}, cfg).region == cfg.region,
                  "region: estimate carries the configured region");
            check(sourceRegionForStation(IntensityFamily::Csis, 23.0, 120.3, &region)
                      && region == SourceRegion::Taiwan,
                  "region: Taiwan CSIS station maps to the Taiwan region");
            check(sourceRegionForStation(IntensityFamily::Jma, 23.0, 120.3, &region)
                      && region == SourceRegion::Taiwan,
                  "region: Taiwan JMA station (future CWA) maps to Taiwan, not Japan");
            check(sourceRegionForStation(IntensityFamily::Csis, 30.6, 104.1, &region)
                      && region == SourceRegion::China,
                  "region: Sichuan CSIS station maps to China");
            check(sourceRegionForStation(IntensityFamily::Csis, 39.9, 121.8, &region)
                      && region == SourceRegion::China,
                  "region: Liaoning station stays in China (not Japan)");
            check(sourceRegionForStation(IntensityFamily::Jma, 35.0, 139.0, &region)
                      && region == SourceRegion::Japan,
                  "region: Tokyo JMA station maps to Japan");
            check(sourceRegionForStation(IntensityFamily::Kma, 37.5, 127.0, &region)
                      && region == SourceRegion::Korea,
                  "region: KMA station maps to Korea");
            check(!sourceRegionForStation(IntensityFamily::Csis, 0.0, 0.0, &region),
                  "region: out-of-area CSIS station is not assigned");
            check(!sourceRegionForStation(IntensityFamily::Jma, std::nan(""), 139.0, &region),
                  "region: non-finite coordinates are rejected");
            // 制式 ↔ 中国烈度等效：阈值换算必须能来回走（误差在 0.1 度以内）。
            for (const IntensityFamily family : {IntensityFamily::Csis, IntensityFamily::Jma,
                                                 IntensityFamily::Kma}) {
                const double back = intensityFromCsis(family, csisEquivalent(family, 3.4));
                check(std::fabs(back - 3.4) < 0.06,
                      "region: family trigger conversion round-trips");
            }
            check(std::fabs(intensityFromCsis(IntensityFamily::Jma, 3.0) - 2.5) < 0.06,
                  "region: CSIS 3.0 is JMA 2.5");
            check(std::fabs(intensityFromCsis(IntensityFamily::Kma, 3.0) - 4.0) < 0.06,
                  "region: CSIS 3.0 is MMI 4.0");
            // 区域默认值：日本门槛对齐 kanameishi 的 `minInferenceStationCount = 5`
            // （压噪声靠邻域联动 + 着未着惩罚，不是抬高站数）；台湾目前只有零星站（门槛 1）。
            check(sourceRegionDefaults(SourceRegion::Japan).minStations == 5,
                  "region: Japan default floor matches kanameishi (5 stations)");
            check(sourceRegionDefaults(SourceRegion::Japan).minStations
                      < sourceRegionDefaults(SourceRegion::China).minStations,
                  "region: Japan floor is below China (dense network needs fewer stations)");
            check(sourceRegionDefaults(SourceRegion::Taiwan).minStations == 1,
                  "region: Taiwan default floor accepts a single station");
            check(std::string(sourceRegionId(SourceRegion::Taiwan)) == "taiwan"
                      && std::string(sourceRegionLabel(SourceRegion::Taiwan)) == "台湾",
                  "region: id and label are stable for settings/QML");
            SourceRegion parsed = SourceRegion::China;
            check(sourceRegionFromId("korea", &parsed) && parsed == SourceRegion::Korea,
                  "region: id round-trips");
            check(!sourceRegionFromId("mars", &parsed), "region: unknown id is rejected");
        }

        // 性能：一帧 960 站必须在几十毫秒内算完（1 Hz 调度，跑 worker 线程）。
        const auto benchStart = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; ++i) (void)SourceEstimator::estimate(grid, cfg);
        const double benchMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - benchStart)
                .count() / 3.0;
        std::printf("        (source estimate: %.1f ms for %zu stations)\n", benchMs, grid.size());
        check(benchMs < 120.0, "source estimate: under 120 ms for a 960-station frame");
    }

    // ---- 日本区到时推算（ArrivalEstimator）------------------------------
    // 用一张合成的「解析走时表」验证算法本身：vP=6 km/s、vS=3.5 km/s，
    // 走时 = 震源距 / 速度。真实 JMA2001 表由 service 层注入（见 ServiceTravelTimes）。
    {
        class AnalyticTravelTimes final : public ArrivalEstimator::TravelTimes {
        public:
            double pSeconds(double depthKm, double distanceKm) const override {
                return std::sqrt(depthKm * depthKm + distanceKm * distanceKm) / 6.0;
            }
            double sSeconds(double depthKm, double distanceKm) const override {
                return std::sqrt(depthKm * depthKm + distanceKm * distanceKm) / 3.5;
            }
        };
        const AnalyticTravelTimes travelTimes;

        // 合成一场 M6.0 / 深度 10 km、震中 35.0N 139.0E 的到时场：
        // 每站 onset = origin + tP(distance)。触发区 0.3° 网格覆盖 ±1.2°。
        const long long originMs = 1'800'000'000'000LL;
        const double eqLat = 35.0;
        const double eqLon = 139.0;
        const double depthKm = 10.0;
        std::vector<ArrivalObservation> arrivals;
        for (double lat = 33.8; lat <= 36.2 + 1e-9; lat += 0.3) {
            for (double lon = 137.8; lon <= 140.2 + 1e-9; lon += 0.3) {
                const double distance =
                    QuakeCalculator::haversineDistance(eqLat, eqLon, lat, lon);
                ArrivalObservation a;
                a.latitude = lat;
                a.longitude = lon;
                a.onsetEpochMs =
                    originMs + static_cast<long long>(std::llround(
                                   travelTimes.pSeconds(depthKm, distance) * 1000.0));
                // 强度必须由**同一套 JMA 衰减关系**生成，否则「位置已定」之后反推的
                // 震级会系统偏大（实测用一个随手写的 6.5 − d/60 会解出 M8.2）。
                a.intensity = SourceEstimator::predictedIntensity(IntensityFamily::Jma, 6.0,
                                                                 distance, depthKm);
                a.maxIntensity = a.intensity;
                arrivals.push_back(a);
            }
        }

        ArrivalEstimatorConfig arrivalConfig;
        const ArrivalEstimate arrival = ArrivalEstimator::estimate(arrivals, arrivalConfig, travelTimes);
        check(arrival.valid, "arrival: analytic field resolves a hypocenter");
        check(QuakeCalculator::haversineDistance(arrival.latitude, arrival.longitude, eqLat, eqLon)
                  < 25.0,
              "arrival: epicentre within 25 km of truth");
        check(near(arrival.depthKm, depthKm, 1e-6), "arrival: depth locked to the true candidate");
        check(std::llabs(arrival.originEpochMs - originMs) < 1500,
              "arrival: origin time within 1.5 s");
        check(arrival.pCount > 0, "arrival: classifies stations as P phase");
        // 定级用「有效站数 + 质量分」双门槛（kanameishi）。这里 81 站、
        // effectiveCount = (81 P 相 + 0 S 相 + 81 站)/2 = 81 → 够 B（30 站）够 A（100 站不够）。
        check(arrival.grade == "A" || arrival.grade == "B",
              "arrival: dense field grades A/B by effective-count gate");
        check(arrival.qualityScore >= 1.0, "arrival: clean field scores high on quality");
        // 站数门槛是真的在生效：把 A 的门槛抬到 500，同一份数据必须掉下来。
        {
            ArrivalEstimatorConfig strictRanks = arrivalConfig;
            strictRanks.rankMinStationsA = 500;
            strictRanks.rankMinStationsB = 400;
            strictRanks.rankMinStationsC = 300;
            const ArrivalEstimate capped = ArrivalEstimator::estimate(arrivals, strictRanks,
                                                                       travelTimes);
            check(capped.valid && capped.grade == "D",
                  "arrival: station gate caps the grade when not enough stations");
        }
        check(arrival.rmsSeconds < 0.5, "arrival: residual stays sub-second on clean data");
        // 震级：位置定住之后对强度做一维拟合，必须落在真值附近。
        check(std::abs(arrival.magnitude - 6.0) < 0.6,
              "arrival: magnitude fitted from intensity once the location is fixed");

        // 平移不变：整场到时同时推迟 10 s（模拟时钟偏移），解的发震时刻同样推迟。
        {
            std::vector<ArrivalObservation> shifted = arrivals;
            for (ArrivalObservation& a : shifted) a.onsetEpochMs += 10'000;
            const ArrivalEstimate moved = ArrivalEstimator::estimate(shifted, arrivalConfig,
                                                                     travelTimes);
            check(moved.valid, "arrival: clock shift keeps a valid solution");
            check(std::llabs((moved.originEpochMs - 10'000) - originMs) < 1500,
                  "arrival: clock shift moves origin by the same amount");
            check(QuakeCalculator::haversineDistance(moved.latitude, moved.longitude, eqLat, eqLon)
                      < 25.0,
                  "arrival: clock shift does not move the epicentre");
        }

        // 站数不足时不发布：3 站以下既定不出位置也定不出时刻。
        {
            std::vector<ArrivalObservation> few(arrivals.begin(), arrivals.begin() + 2);
            ArrivalEstimatorConfig strict = arrivalConfig;
            strict.minStations = 5;
            const ArrivalEstimate rejected = ArrivalEstimator::estimate(few, strict, travelTimes);
            check(!rejected.valid, "arrival: below minimum stations yields no solution");
        }

        // 门槛放宽到 1–2 站时必须标 provisional，而不是给出一个看起来精确的坐标。
        {
            std::vector<ArrivalObservation> few(arrivals.begin(), arrivals.begin() + 2);
            ArrivalEstimatorConfig loose = arrivalConfig;
            loose.minStations = 2;
            loose.publishMinStations = 5;   // 发布门槛仍是 5 → provisional
            const ArrivalEstimate weak = ArrivalEstimator::estimate(few, loose, travelTimes);
            check(weak.valid && weak.provisional,
                  "arrival: two stations produce a provisional solution");
            check(weak.grade == "D", "arrival: provisional solution never grades above D");
            check(weak.uncertaintyKm >= 60.0, "arrival: provisional uncertainty stays large");
        }

        // 邻域联动：把 10 个触发站拆成互不相邻的孤立点，凑不出邻域团 → 不发布。
        // 这正是参考实现用邻接表想要挡掉的「远场孤立强噪声」。
        {
            std::vector<ArrivalObservation> scattered;
            for (int i = 0; i < 10; ++i) {
                ArrivalObservation a;
                // 每站相隔约 1.5°（≈165 km），远大于 30 km 邻域半径。
                a.latitude = 33.0 + i * 0.15;
                a.longitude = 137.0 + i * 0.9;
                a.onsetEpochMs = originMs + i * 1000;
                a.intensity = 4.0;
                a.maxIntensity = 4.0;
                scattered.push_back(a);
            }
            ArrivalEstimatorConfig loose = arrivalConfig;
            loose.minStations = 5;
            loose.publishMinStations = 5;
            const ArrivalEstimate isolated = ArrivalEstimator::estimate(scattered, loose,
                                                                        travelTimes);
            check(!isolated.valid,
                  "arrival: scattered isolated triggers never form a neighbourhood");
        }

        // 着未着惩罚：候选解若落在「一片本该已到波却毫无反应」的站中间，罚项会抬高它。
        // 构造：真实触发在西南侧，正北 200 km 内有一群未触发站（P 走时早已过去）。
        {
            std::vector<std::pair<double, double>> quiet;
            const long long nowMs = originMs + 60'000;
            for (double lat = 36.2; lat <= 37.2 + 1e-9; lat += 0.2) {
                for (double lon = 138.4; lon <= 139.6 + 1e-9; lon += 0.2) {
                    quiet.emplace_back(lat, lon);
                }
            }
            // 只保留西南侧触发站，模拟单侧台网（外海地震）。
            std::vector<ArrivalObservation> oneSided;
            for (const ArrivalObservation& a : arrivals) {
                if (a.latitude <= eqLat + 0.1 && a.longitude <= eqLon + 0.1) oneSided.push_back(a);
            }
            const ArrivalEstimate withQuiet = ArrivalEstimator::estimate(
                oneSided, arrivalConfig, travelTimes, quiet, nowMs);
            const ArrivalEstimate withoutQuiet =
                ArrivalEstimator::estimate(oneSided, arrivalConfig, travelTimes);
            check(withQuiet.valid && withoutQuiet.valid,
                  "arrival: quiet evidence keeps a valid solution");
            // 未触发站必须**被计入**（否则惩罚形同虚设）。注意不能要求「解一定更好」——
            // 在台网完全单侧且未触发站恰好都在远侧时，它们本来就不构成矛盾；
            // 这里能断言的是「矛盾站被识别出来了」与「解没有被推走」。
            check(withQuiet.penalizedQuietCount >= 0,
                  "arrival: quiet evidence is evaluated without crashing");
            const double withQuietError =
                QuakeCalculator::haversineDistance(withQuiet.latitude, withQuiet.longitude,
                                                   eqLat, eqLon);
            const double withoutQuietError =
                QuakeCalculator::haversineDistance(withoutQuiet.latitude, withoutQuiet.longitude,
                                                   eqLat, eqLon);
            check(withQuietError <= withoutQuietError + 1.0,
                  "arrival: quiet evidence does not make a one-sided solution worse");

            // 真正有区分力的场景：台网只覆盖震源南侧，未触发站紧贴在触发区北侧
            // （真实外海地震的样子）。此时「北侧一片死寂」本身就是位置信息。
            std::vector<ArrivalObservation> southOnly;
            for (const ArrivalObservation& a : arrivals) {
                if (a.latitude <= eqLat - 0.2) southOnly.push_back(a);
            }
            std::vector<std::pair<double, double>> northQuiet;
            for (double lat = eqLat + 0.3; lat <= eqLat + 1.1 + 1e-9; lat += 0.2) {
                for (double lon = eqLon - 0.8; lon <= eqLon + 0.8 + 1e-9; lon += 0.2) {
                    northQuiet.emplace_back(lat, lon);
                }
            }
            if (southOnly.size() >= 5 && !northQuiet.empty()) {
                const ArrivalEstimate northWithout =
                    ArrivalEstimator::estimate(southOnly, arrivalConfig, travelTimes);
                const ArrivalEstimate northWith = ArrivalEstimator::estimate(
                    southOnly, arrivalConfig, travelTimes, northQuiet, originMs + 25'000);
                check(northWith.valid && northWithout.valid,
                      "arrival: one-sided + dead zone keeps a valid solution");
                // 未触发站必须被识别为矛盾（北侧站此刻本该已到波）。
                check(northWith.penalizedQuietCount > 0,
                      "arrival: dead-zone stations are counted as contradictions");
                // 并且解不能被推到北侧死寂区里：纬度不应明显越过触发区上沿。
                check(northWith.latitude <= eqLat + 0.4,
                      "arrival: dead zone does not pull the solution into it");
            }
        }

        // 关联门：加一个「距离上不可能这么晚才到」的站（约 500 km 外却晚了 300 s，
        // 而 500 km 的 P 走时只有约 85 s），它必须被门挡掉而不是把解拉走。
        {
            std::vector<ArrivalObservation> polluted = arrivals;
            ArrivalObservation far;
            far.latitude = eqLat + 4.0;    // 约 445 km 以北
            far.longitude = eqLon;
            far.onsetEpochMs = originMs + 300'000;
            far.intensity = 3.0;
            far.maxIntensity = 3.0;
            polluted.push_back(far);
            const ArrivalEstimate gated = ArrivalEstimator::estimate(polluted, arrivalConfig,
                                                                     travelTimes);
            check(gated.valid, "arrival: association gate keeps a valid solution");
            check(QuakeCalculator::haversineDistance(gated.latitude, gated.longitude, eqLat, eqLon)
                      < 40.0,
                  "arrival: unrelated far station does not move the solution");
        }

        // 无到时（onset 为 0）的观测必须被剔除，不能当成「刚好在震源处」。
        {
            std::vector<ArrivalObservation> broken = arrivals;
            for (ArrivalObservation& a : broken) a.onsetEpochMs = 0;
            const ArrivalEstimate none = ArrivalEstimator::estimate(broken, arrivalConfig,
                                                                    travelTimes);
            check(!none.valid, "arrival: observations without onset are rejected");
        }

        // 转换成统一结果：方法标识与相位数要带出去，供卡片区分展示。
        {
            const SourceEstimate unified = ArrivalEstimator::toSourceEstimate(arrival);
            check(unified.valid && unified.region == SourceRegion::Japan,
                  "arrival: converts to the unified SourceEstimate");
            check(unified.method.rfind("arrival_time", 0) == 0,
                  "arrival: method id marks the arrival-time path");
            check(unified.originEpochMs == arrival.originEpochMs,
                  "arrival: origin time survives conversion");
            check(unified.pPhaseCount == arrival.pCount && unified.sPhaseCount == arrival.sCount,
                  "arrival: phase counts survive conversion");
        }
    }


    // ---- PolygonTessellator ----------------------------------------------
    //
    // 三角化是 GPU 矢量层的地基：它把 even-odd 的多环面变成三角形。
    // 这里断言的是**语义**（面积守恒、洞镂空、绕向无关、裁剪正确），
    // 不是具体三角形布局 —— 布局可以优化，面积语义不能变。
    {
        using Ring = PolygonTessellator::Ring;
        using Mesh = PolygonTessellator::Mesh;
        using FR = PolygonTessellator::FillRule;
        const auto area = [](const Mesh& m) { return PolygonTessellator::meshArea(m); };

        // 单位方形：面积 100，且梯形合并后只出 2 个三角形。
        {
            std::vector<double> sq = {0, 0, 10, 0, 10, 10, 0, 10};
            std::vector<Ring> rings{Ring(sq)};
            Mesh m;
            PolygonTessellator::tessellate(rings, FR::EvenOdd, &m);
            check(near(area(m), 100.0), "tess: square area 100");
            check(m.triangleCount() == 2, "tess: square merges into 2 triangles");
            check(m.vertexCount() == 4, "tess: square reuses 4 vertices");
        }

        // 洞：even-odd 下绕向无关（真实数据绕向不可信，必须两种都给对）。
        {
            const std::vector<double> outer = {0, 0, 10, 0, 10, 10, 0, 10};
            for (const std::vector<double>& hole :
                 {std::vector<double>{3, 3, 3, 7, 7, 7, 7, 3},
                  std::vector<double>{3, 3, 7, 3, 7, 7, 3, 7}}) {
                std::vector<Ring> rings{Ring(outer), Ring(hole)};
                Mesh m;
                PolygonTessellator::tessellate(rings, FR::EvenOdd, &m);
                check(near(area(m), 84.0), "tess: hole punched out regardless of winding");
            }
        }

        // 凹多边形（L 形）：面积 64。
        {
            std::vector<double> l = {0, 0, 10, 0, 10, 4, 4, 4, 4, 10, 0, 10};
            std::vector<Ring> rings{Ring(l)};
            Mesh m;
            PolygonTessellator::tessellate(rings, FR::EvenOdd, &m);
            check(near(area(m), 64.0), "tess: concave L shape area 64");
        }

        // 嵌套分组：一堆互不相交的岛（而不是「外环+洞」）必须与联合扫描等价。
        // 真实数据里世界层的日本要素就是 352 个环的岛群，联合扫描会产生 2 倍三角形。
        {
            std::vector<double> a = {0, 0, 10, 0, 10, 10, 0, 10};
            std::vector<double> b = {20, 0, 30, 0, 30, 10, 20, 10};
            std::vector<Ring> rings{Ring(a), Ring(b)};
            Mesh joint, nested;
            PolygonTessellator::tessellate(rings, FR::EvenOdd, &joint);
            PolygonTessellator::tessellateNested(rings, FR::EvenOdd, &nested);
            check(near(area(joint), 200.0) && near(area(nested), 200.0),
                  "tess: disjoint islands keep total area");
            check(nested.triangleCount() <= joint.triangleCount(),
                  "tess: nesting groups never produce more triangles");
        }

        // 裁剪：语义是「面积 ∩ 矩形」。这是分块建网格的正确性前提。
        {
            std::vector<double> sq = {0, 0, 100, 0, 100, 100, 0, 100};
            std::vector<Ring> rings{Ring(sq)};
            struct Case {
                PolygonTessellator::ClipRect rect;
                double expect;
                const char* name;
            };
            const Case cases[] = {
                {{0, 0, 50, 100}, 5000.0, "tess: clip left half"},
                {{25, 25, 75, 75}, 2500.0, "tess: clip centre square"},
                {{200, 200, 300, 300}, 0.0, "tess: clip outside yields nothing"},
            };
            for (const Case& c : cases) {
                Mesh m;
                PolygonTessellator::tessellate(rings, FR::EvenOdd, &m, &c.rect);
                check(near(area(m), c.expect), c.name);
            }
            // 带洞 + 裁剪：左半 50*100 减去洞的左半 25*50 = 3750。
            std::vector<double> hole = {25, 25, 25, 75, 75, 75, 75, 25};
            std::vector<Ring> withHole{Ring(sq), Ring(hole)};
            PolygonTessellator::ClipRect left{0, 0, 50, 100};
            Mesh m;
            PolygonTessellator::tessellate(withHole, FR::EvenOdd, &m, &left);
            check(near(area(m), 3750.0), "tess: clipped hole area 3750");
        }

        // 退化输入一律不产出、不崩溃（坏数据/极小环/共线/NaN）。
        {
            Mesh m;
            PolygonTessellator::tessellate({}, FR::EvenOdd, &m);
            check(m.empty(), "tess: empty ring list yields nothing");
            std::vector<double> two = {0, 0, 1, 1};
            PolygonTessellator::tessellate({Ring(two)}, FR::EvenOdd, &m);
            check(m.empty(), "tess: two-point ring yields nothing");
            std::vector<double> collinear = {0, 0, 1, 1, 2, 2, 3, 3};
            PolygonTessellator::tessellate({Ring(collinear)}, FR::EvenOdd, &m);
            check(m.empty(), "tess: collinear ring yields nothing");
            std::vector<double> nan = {0, 0, std::nan(""), 0, 10, 10};
            PolygonTessellator::tessellate({Ring(nan)}, FR::EvenOdd, &m);
            check(m.empty(), "tess: NaN coordinates reject the whole ring");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
