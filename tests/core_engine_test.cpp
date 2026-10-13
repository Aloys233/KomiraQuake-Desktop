// 纯 C++ 核心引擎验证（不依赖 Qt）。用 g++ 直接编译运行。
// 亦可直接执行 linux/run_core_tests.sh。

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/coordinate_transform.h"
#include "core/event_gate.h"
#include "core/geo_geometry.h"
#include "core/intensity_calculator.h"
#include "core/quake_calculator.h"
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

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
