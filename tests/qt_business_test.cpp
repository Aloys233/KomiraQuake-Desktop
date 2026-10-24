#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QSet>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <QWebSocket>
#include <QWebSocketServer>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include "core/ip_geo_lookup.h"
#include "core/quake_calculator.h"
#include "core/quake_key.h"
#include "core/travel_time_service.h"
#include "core/version_compare.h"
#include "core/warning_session.h"
#include "model/data_source_info.h"
#include "model/event_list_model.h"
#include "prefs/settings_store.h"
#include "service/autostart.h"
#include "service/sentry_service.h"
#include "source/eew_parser.h"
#include "source/jian_parser.h"
#include "source/kma_station_parser.h"
#include "source/kmoni_station_parser.h"
#include "source/pancakes_parser.h"
#include "source/pancakes_protocol.h"
#include "source/pancakes_seis_parser.h"
#include "source/simulated_parser.h"
#include "source/simulated_protocol.h"
#include "source/sub_source_catalog.h"
#include "source/snet_station_parser.h"
#include "source/whews_parser.h"
#include "source/whews_protocol.h"
#include "source/wolfx_seis_parser.h"
#include "core/quake_calculator.h"
#include "core/region_names.h"
#include "core/source_estimator.h"
#include "store/history_store.h"

// No production clock/network seam exists for message injection. Restrict access
// widening to these headers; all of their Qt/STL dependencies are included above.
// 合成一段而非多段：moc 处理本文件时也会看到这些宏，分段会让 vtable 声明失配。
#define private public
#include "service/source_estimation_service.h"
#include "source/wolfx_source.h"
#include "source/pancakes_seis_source.h"
#include "source/whews_source.h"
#include "source/simulated_source.h"
#include "source/pancakes_source.h"
#include "source/jian_source.h"
#undef private

using namespace komira;

namespace {
constexpr long long origin = 1'800'000'000'000LL;
EarthquakeEvent makeEvent(const std::string& id = "A") {
    EarthquakeEvent e;
    e.id = "wolfx_" + id;
    e.eventId = id;
    e.source = "CENC";
    e.sourceProvider = "Wolfx";
    e.sourceAgency = "CENC";
    e.timestamp = origin;
    e.reportTime = origin + 1000;
    e.magnitude = 5.5;
    e.latitude = 30.0;
    e.longitude = 103.0;
    e.location = "test epicenter";
    e.distanceKm = -1;
    e.estimatedIntensity = "--";
    return e;
}
QJsonObject payload() {
    return {{"type", "cenc_eew"}, {"EventID", "A"}, {"Magnitude", 5.5},
            {"Latitude", 30.0}, {"Longitude", 103.0}, {"Depth", 10.0},
            {"OriginTime", QString::number(origin)},
            {"ReportTime", QString::number(origin + 1000)},
            {"ReportNum", 1}, {"MaxIntensity", 7.0}};
}
}

class QtBusinessTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir settingsDir_;
private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        qputenv("XDG_CONFIG_HOME", (settingsDir_.path() + "/xdg-config").toUtf8());
        QCoreApplication::setOrganizationName("KomiraQuakeBusinessTests");
        QCoreApplication::setApplicationName("IsolatedSettings");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir_.path());
        qRegisterMetaType<EarthquakeEvent>();
        qRegisterMetaType<SourceEventKind>();
        const QString asset = QFINDTESTDATA("../assets/travel_times.json");
        QVERIFY2(!asset.isEmpty(), "The real travel asset must be available; do not silently use fallback speeds");
        QVERIFY(TravelTimeService::instance().loadFromFile(asset));
        QVERIFY(TravelTimeService::instance().isLoaded());
        // 震中地名表：卡片的大号标题走它（参考实现 `getFEName` 的 1° 网格）。
        // 缺失不是致命错误（卡片会退回坐标文本），但测试必须跑在真实资产上。
        const QString regionNames = QFINDTESTDATA("../assets/region_names.bin");
        QVERIFY2(!regionNames.isEmpty(), "The real region-name asset must be available");
        QVERIFY(RegionNameTable::instance().loadFromFile(regionNames.toStdString()));
    }

    // NIED Kmoni 测站通道：首包台站表 + 增量震度数组解析，及连续震度->JMA 震度阶文本。
    void kmoniStationParsesStationsAndIntensity() {
        const QJsonObject stationsFrame = QJsonDocument::fromJson(R"({
            "type":"kmoni","timestamp":1791893684277,
            "stations":[
                {"code":"ABSH01","lat":44.5276,"lon":142.8444,"name":"雄武","region":"北海道","station_type":"KiK-net"},
                {"code":"ABSH02","lat":44.4234,"lon":143.0264,"name":"興部西","region":"北海道","station_type":"KiK-net"}
            ]})").object();
        const auto stations = KmoniStationParser::parseStations(stationsFrame);
        QVERIFY(stations.has_value());
        QCOMPARE(stations->size(), 2);
        QCOMPARE(stations->at(0).code, QStringLiteral("ABSH01"));
        QCOMPARE(stations->at(0).latitude, 44.5276);
        QCOMPARE(stations->at(0).stationType, QStringLiteral("KiK-net"));
        // 首包不含 int[]；增量帧不含 stations。
        QVERIFY(!KmoniStationParser::parseIntensity(stationsFrame).has_value());

        const QJsonObject frame = QJsonDocument::fromJson(R"({
            "type":"kmoni","time":"20261013234717","timestamp":1791902839000,
            "int":[-3, 0.5, 2.4, -3]})").object();
        const auto intensity = KmoniStationParser::parseIntensity(frame);
        QVERIFY(intensity.has_value());
        QCOMPARE(intensity->size(), 4);
        QCOMPARE(intensity->at(1), 0.5);
        QVERIFY(!KmoniStationParser::parseStations(frame).has_value());

        QCOMPARE(jmaShindoText(-3.0), QStringLiteral("?"));
        QCOMPARE(jmaShindoText(0.4), QStringLiteral("0"));
        QCOMPARE(jmaShindoText(2.4), QStringLiteral("2"));
        QCOMPARE(jmaShindoText(4.6), QStringLiteral("5弱"));
        QCOMPARE(jmaShindoText(5.2), QStringLiteral("5強"));
        QCOMPARE(jmaShindoText(6.7), QStringLiteral("7"));
    }

    // S-Net 测站通道：首包台站表（code/lat/lon/name/type）+ 增量 int[] 解析。
    void snetStationParsesStationsAndIntensity() {
        const QJsonObject stationsFrame = QJsonDocument::fromJson(R"({
            "type":"s-net","timestamp":1791893685468,
            "stations":[
                {"code":"N.S1N01","lat":35.8968,"lon":141.0535,"name":"S1N01","type":"S-net"},
                {"code":"N.ST1H","lat":34.59556,"lon":139.91831,"name":"ST1H","type":"sagami"}
            ]})").object();
        const auto stations = SnetStationParser::parseStations(stationsFrame);
        QVERIFY(stations.has_value());
        QCOMPARE(stations->size(), 2);
        QCOMPARE(stations->at(0).code, QStringLiteral("N.S1N01"));
        QCOMPARE(stations->at(0).latitude, 35.8968);
        QCOMPARE(stations->at(0).name, QStringLiteral("S1N01"));
        QCOMPARE(stations->at(1).stationType, QStringLiteral("sagami"));
        // 首包不含 int[]；增量帧不含 stations。
        QVERIFY(!SnetStationParser::parseIntensity(stationsFrame).has_value());

        const QJsonObject frame = QJsonDocument::fromJson(R"({
            "type":"s-net","time":"20261017105400","timestamp":1791893685468,
            "int":[-3, 2.4, 99]})").object();
        const auto intensity = SnetStationParser::parseIntensity(frame);
        QVERIFY(intensity.has_value());
        QCOMPARE(intensity->size(), 3);
        QCOMPARE(intensity->at(0), -3.0);
        QCOMPARE(intensity->at(1), 2.4);
        // 饱和值原样保留（显示层按上限处理）。
        QCOMPARE(intensity->at(2), 99.0);
        QVERIFY(!SnetStationParser::parseStations(frame).has_value());
    }

    // KMA 测站通道：首包坐标数组（仅 lat/lon）+ 增量 mmi[] 解析。
    void kmaStationParsesStationsAndMmi() {
        const QJsonObject stationsFrame = QJsonDocument::fromJson(R"({
            "type":"kma-station","timestamp":1791893685065,
            "stations":[{"lat":36.41,"lon":128.94},{"lat":36.57,"lon":128.7}]})").object();
        const auto stations = KmaStationParser::parseStations(stationsFrame);
        QVERIFY(stations.has_value());
        QCOMPARE(stations->size(), 2);
        QCOMPARE(stations->at(0).latitude, 36.41);
        QCOMPARE(stations->at(1).longitude, 128.7);
        QVERIFY(!KmaStationParser::parseMmi(stationsFrame).has_value());

        const QJsonObject frame = QJsonDocument::fromJson(R"({
            "type":"kma-station","time":"20261017105545","timestamp":1791893685065,
            "phase":1,"phase_name":"normal","mmi":[1, -3, 4]})").object();
        const auto mmi = KmaStationParser::parseMmi(frame);
        QVERIFY(mmi.has_value());
        QCOMPARE(mmi->size(), 3);
        QCOMPARE(mmi->at(0), 1.0);
        QCOMPARE(mmi->at(1), -3.0);
        QCOMPARE(mmi->at(2), 4.0);
        QVERIFY(!KmaStationParser::parseStations(frame).has_value());
    }

    // PancakesSeis 测站通道（v4 广播格式）：档案例报文 + 实时帧解析、sid 索引、
    // 健康码剔除、以及 GB/T 17742-2020 仪器烈度换算。
    void pancakesSeisParsesStationsAndSamples() {
        const QJsonObject stationsFrame = QJsonDocument::fromJson(R"({
            "type":"stations","timestamp":1792147945777,"total":1,
            "data":[
                {"sid":0,"name":"四川成都郫都红光监测站 01",
                 "location":{"lat":30.8105,"lon":103.8865,"alt":540,
                             "region":"四川省成都市郫都区"}}
            ]})").object();
        QVERIFY(PancakesSeisParser::isStationFrame(stationsFrame));
        const auto stations = PancakesSeisParser::parseStations(stationsFrame);
        QVERIFY(stations.has_value());
        QCOMPARE(stations->size(), 1);
        QCOMPARE(stations->at(0).sid, 0);
        QCOMPARE(stations->at(0).name, QStringLiteral("四川成都郫都红光监测站 01"));
        QCOMPARE(stations->at(0).latitude, 30.8105);
        QCOMPARE(stations->at(0).longitude, 103.8865);
        QCOMPARE(stations->at(0).altitude, 540.0);
        QCOMPARE(stations->at(0).region, QStringLiteral("四川省成都市郫都区"));
        // 档案不含实时字段；实时帧不含 type。
        QVERIFY(!PancakesSeisParser::parseSample(stationsFrame).has_value());

        // HTTP 列表与广播档案字段一致（无 `type`）。
        const QJsonObject listRoot = QJsonDocument::fromJson(R"({
            "code":200,"total":1,
            "data":[{"sid":0,"name":"四川成都郫都红光监测站 01",
                     "location":{"lat":30.8105,"lon":103.8865,"alt":540,
                                 "region":"四川省成都市郫都区"}}]})").object();
        const auto listed = PancakesSeisParser::parseStationList(listRoot);
        QVERIFY(listed.has_value());
        QCOMPARE(listed->size(), 1);
        QCOMPARE(listed->at(0).sid, 0);
        QVERIFY(!PancakesSeisParser::parseStationList(QJsonObject{}).has_value());

        // 现行帧：设备端算好的仪器烈度 `i` 与 2 分钟最大烈度 `m` 原样透传。
        // 上游已移除 `x`/`y`/`z` 三轴峰值加速度，帧里只剩 `s`/`p`/`v`/`i`/`m`/`t`。
        const QJsonObject frame = QJsonDocument::fromJson(
            R"({"s":0,"p":12.5,"v":8.75,"i":3.7,"m":4.1,"t":1729000000000})").object();
        const auto sample = PancakesSeisParser::parseSample(frame);
        QVERIFY(sample.has_value());
        QCOMPARE(sample->sid, 0);
        QCOMPARE(sample->pga, 12.5);
        QCOMPARE(sample->pgv, 8.75);
        QCOMPARE(sample->intensity, 3.7);
        QCOMPARE(sample->maxIntensity, 4.1);
        QCOMPARE(sample->errorCode, 0);
        QCOMPARE(sample->timestamp, 1729000000000LL);

        // 静稳最短形态：可选字段整体省略，`i` 仍为 0（区分「烈度就是 0」与「无帧」）。
        const QJsonObject idle = QJsonDocument::fromJson(
            R"({"s":0,"p":0.005,"i":0,"t":1729000000000})").object();
        const auto quiet = PancakesSeisParser::parseSample(idle);
        QVERIFY(quiet.has_value());
        QCOMPARE(quiet->pgv, 0.0);
        QCOMPARE(quiet->intensity, 0.0);
        QCOMPARE(quiet->maxIntensity, 0.0);

        // 健康码 `ec` 非 0 原样解析（剔除由 source 负责）。
        const QJsonObject bad = QJsonDocument::fromJson(
            R"({"s":3,"p":9.5,"i":2.8,"ec":7,"t":1729000000000})").object();
        const auto faulty = PancakesSeisParser::parseSample(bad);
        QVERIFY(faulty.has_value());
        QCOMPARE(faulty->sid, 3);
        QCOMPARE(faulty->errorCode, 7);

        // `ec` 为 0 时上游省略该字段。
        QVERIFY(!frame.contains(QStringLiteral("ec")));
        // 缺 `s` 或 `p` 的帧不是数据帧；带 `type` 的也不是。
        QVERIFY(!PancakesSeisParser::parseSample(
                     QJsonDocument::fromJson(R"({"p":1.0,"i":1.0})").object())
                     .has_value());
        QVERIFY(!PancakesSeisParser::parseSample(
                     QJsonDocument::fromJson(R"({"type":"pong","s":0,"p":1.0})").object())
                     .has_value());

        // 线上抓到的真实报文样本：① 2026-10-18 18:15 的旧帧（还带 `x`/`y`/`z`）；
        // ② 2026-10-18 20:04 的现行帧——三轴已被上游**移除**，只剩 `s`/`p`/`i`/`m`/`t`。
        // `i` 与 GB/T 公式逐帧一致——这正是「直接用设备值」的对照基准。
        const QJsonObject liveStations = QJsonDocument::fromJson(R"({
            "data":[{"sid":0,"name":"四川省成都市郫都区",
                     "location":{"lat":30.797,"lon":103.9078,
                                 "region":"四川省成都市郫都区"}}],
            "timestamp":1792318500280,"total":1,"type":"stations"})").object();
        const auto live = PancakesSeisParser::parseStations(liveStations);
        QVERIFY(live.has_value());
        QCOMPARE(live->size(), 1);
        QCOMPARE(live->at(0).name, QStringLiteral("四川省成都市郫都区"));
        QCOMPARE(live->at(0).latitude, 30.797);
        QCOMPARE(live->at(0).altitude, 0.0);   // 无 alt 时按 0，不伪造

        struct LiveFrame {
            const char* json;
            double pga;
            double intensity;
            double maxIntensity;
        };
        const LiveFrame liveFrames[] = {
            // 旧帧：三轴还在，解析时忽略、不影响其余字段。
            {R"({"s":0,"p":1.511,"v":0.202,"x":0.994,"y":1,"z":1.12,"i":2.4,"m":4.2,"t":1792318500985})",
             1.511, 2.4, 4.2},
            {R"({"s":0,"p":1.397,"v":0.086,"x":0.86,"y":0.984,"z":1.227,"i":2.3,"m":4.2,"t":1792318502985})",
             1.397, 2.3, 4.2},
            {R"({"s":0,"p":15.321,"v":0.32,"x":15.32,"y":1.228,"z":1.233,"i":4.1,"m":4.2,"t":1792318503985})",
             15.321, 4.1, 4.2},
            // 现行帧（20:04）：三轴已移除；`p`、`i` 同为 0（静稳）也必须落值，
            // `m` 仍保留 2 分钟窗口最大值。
            {R"({"s":0,"p":0,"i":0,"m":3.1,"t":1792325082617})",
             0.0, 0.0, 3.1},
            {R"({"s":0,"p":2.096,"v":0.307,"i":2.6,"m":6.1,"t":1792325083616})",
             2.096, 2.6, 6.1},
        };
        for (const LiveFrame& entry : liveFrames) {
            const auto parsed =
                PancakesSeisParser::parseSample(QJsonDocument::fromJson(entry.json).object());
            QVERIFY(parsed.has_value());
            QCOMPARE(parsed->pga, entry.pga);
            QCOMPARE(parsed->intensity, entry.intensity);
            QCOMPARE(parsed->maxIntensity, entry.maxIntensity);
            QCOMPARE(parsed->errorCode, 0);
            // 设备值与我方兜底公式必须逐帧吻合，否则说明两端换算口径已经分叉。
            QCOMPARE(instrumentIntensityFromPga(entry.pga), entry.intensity);
        }
        // 整数 `y` 是旧报文才有的字段；帧里出现与否都不影响 `parseSample`。
        const auto intY = PancakesSeisParser::parseSample(
            QJsonDocument::fromJson(R"({"s":0,"p":1.511,"y":1,"i":2.4,"t":1})").object());
        QVERIFY(intY.has_value());
        QCOMPARE(intY->intensity, 2.4);

        // 仪器烈度换算（GB/T 17742-2020）：参考点 0.22→1.0、0.89→2.0、3.53→3.0、14.1→4.0。
        QCOMPARE(instrumentIntensityFromPga(0.005), 0.0);
        QCOMPARE(instrumentIntensityFromPga(0.01), 0.0);
        QCOMPARE(instrumentIntensityFromPga(0.11), 0.5);
        QCOMPARE(instrumentIntensityFromPga(0.22), 1.0);
        QCOMPARE(instrumentIntensityFromPga(0.89), 2.0);
        QCOMPARE(instrumentIntensityFromPga(3.53), 3.0);
        QCOMPARE(instrumentIntensityFromPga(14.1), 4.0);
        QCOMPARE(instrumentIntensityFromPga(0.0), 0.0);
        // 上限 12。
        QCOMPARE(instrumentIntensityFromPga(1e9), 12.0);
    }

    // PancakesSeis 源：`s` 按 sid（而非数组下标）对齐、健康码非 0 剔除、静默回落。
    void pancakesSeisSourceAlignsBySidAndDropsFaultyFrames() {
        PancakesSeisSource source;
        source.running_ = true;   // 注入报文前不必真的建连
        QSignalSpy stations(&source, &PancakesSeisSource::stationsChanged);
        QSignalSpy data(&source, &PancakesSeisSource::dataChanged);
        QVERIFY(stations.isValid());
        QVERIFY(data.isValid());

        auto feed = [&](const char* json) {
            source.handleMessage(QString::fromUtf8(json));
        };

        // sid 非连续：数组下标 0/1 对应 sid 3/7，数据帧必须按 sid 找站。
        feed(R"({"type":"stations","total":2,"timestamp":1729000000000,"data":[
            {"sid":3,"name":"甲站","location":{"lat":30.0,"lon":103.0,"region":"四川"}},
            {"sid":7,"name":"乙站","location":{"lat":31.0,"lon":104.0,"region":"云南"}}]})");
        QCOMPARE(stations.count(), 1);
        QCOMPARE(source.stations().size(), 2);
        QCOMPARE(source.intensity().size(), 2);
        QCOMPARE(source.intensity().at(0), PancakesSeisProtocol::kMissingIntensity);
        QCOMPARE(source.intensity().at(1), PancakesSeisProtocol::kMissingIntensity);

        feed(R"({"s":7,"p":12.5,"i":3.7,"t":1729000001000})");
        QCOMPARE(data.count(), 1);
        QCOMPARE(source.intensity().at(1), 3.7);   // sid 7 → 下标 1
        QCOMPARE(source.intensity().at(0), PancakesSeisProtocol::kMissingIntensity);
        QCOMPARE(source.maxIntensity(), 3.7);
        QCOMPARE(source.lastTimestamp(), 1729000001000LL);

        // 静稳帧：i 为 0 也要落值（区别于缺测哨兵）。
        feed(R"({"s":3,"p":0.005,"i":0,"t":1729000002000})");
        QCOMPARE(source.intensity().at(0), 0.0);
        QCOMPARE(source.intensity().at(0) > PancakesSeisProtocol::kMissingIntensity, true);

        // 健康码非 0：该站该秒必须剔除（回落缺测），且不影响其它站。
        feed(R"({"s":3,"p":40.0,"i":6.2,"ec":5,"t":1729000003000})");
        QCOMPARE(source.intensity().at(0), PancakesSeisProtocol::kMissingIntensity);
        QCOMPARE(source.intensity().at(1), 3.7);

        // 档案重发（测站上下线）：按 sid 保留仍在线的旧读数，不整网瞬时回落。
        feed(R"({"type":"stations","total":2,"timestamp":1729000004000,"data":[
            {"sid":7,"name":"乙站","location":{"lat":31.0,"lon":104.0,"region":"云南"}},
            {"sid":9,"name":"丙站","location":{"lat":32.0,"lon":105.0,"region":"贵州"}}]})");
        QCOMPARE(stations.count(), 2);
        QCOMPARE(source.stations().size(), 2);
        QCOMPARE(source.stations().at(0).sid, 7);
        QCOMPARE(source.intensity().at(0), 3.7);   // 仍在线的乙站保留读数
        QCOMPARE(source.intensity().at(1), PancakesSeisProtocol::kMissingIntensity);
        // 档案里已无 sid 3：迟到的帧被忽略，不会写越界。
        feed(R"({"s":3,"p":1.0,"i":1.0,"t":1729000005000})");
        QCOMPARE(source.intensity().at(0), 3.7);
        QCOMPARE(source.intensity().at(1), PancakesSeisProtocol::kMissingIntensity);

        // 内容完全相同的档案重发不触发 stationsChanged（上游按变更广播，但常重复）。
        feed(R"({"type":"stations","total":2,"timestamp":1729000006000,"data":[
            {"sid":7,"name":"乙站","location":{"lat":31.0,"lon":104.0,"region":"云南"}},
            {"sid":9,"name":"丙站","location":{"lat":32.0,"lon":105.0,"region":"贵州"}}]})");
        QCOMPARE(stations.count(), 2);

        // 静默超过 TTL：回落缺测（清掉 seenAt_ 后手动触发扫描）。
        source.seenAt_.clear();
        for (int sid : {7, 9}) {
            source.seenAt_.insert(sid, QDateTime::currentMSecsSinceEpoch()
                                          - PancakesSeisProtocol::kLiveTtlMs - 1000);
        }
        const int before = data.count();
        source.expireStale();
        QCOMPARE(data.count(), before + 1);
        QCOMPARE(source.intensity().at(0), PancakesSeisProtocol::kMissingIntensity);
        QCOMPARE(source.maxIntensity(), PancakesSeisProtocol::kMissingIntensity);
        source.running_ = false;   // 不让析构去动未建立的连接
    }

    // Wolfx SeisJS 测站通道：自描述单站帧（自带坐标 + 計測震度）解析，及心跳/pong 判别。
    void wolfxSeisParsesStationFrames() {
        const QJsonObject heartbeat = QJsonDocument::fromJson(
            R"({"type":"heartbeat","ver":11,"id":"82034e2d4b9ff89e","timestamp":"1792149278960"})")
            .object();
        QVERIFY(WolfxSeisParser::isHeartbeat(heartbeat));
        QVERIFY(!WolfxSeisParser::isPong(heartbeat));
        QVERIFY(!WolfxSeisParser::parseStation(heartbeat).has_value());

        const QJsonObject pong = QJsonDocument::fromJson(
            R"({"type":"pong","timestamp":"1792149279108"})").object();
        QVERIFY(WolfxSeisParser::isPong(pong));
        QVERIFY(!WolfxSeisParser::parseStation(pong).has_value());

        const QJsonObject frame = QJsonDocument::fromJson(R"({
            "type":"cfbb5286-e6ce-41a4-89e1-3938cb5e30ee","region":"四川省成都市",
            "latitude":30.6,"longitude":104.1,"PGA":0.123,
            "Shindo":"0","CalcShindo":-3.5,"Max_CalcShindo":-2.7,
            "Intensity":1,"update_at":"2026-10-16 19:15:12"})").object();
        QVERIFY(!WolfxSeisParser::isHeartbeat(frame));
        const auto station = WolfxSeisParser::parseStation(frame);
        QVERIFY(station.has_value());
        QCOMPARE(station->id, QStringLiteral("cfbb5286-e6ce-41a4-89e1-3938cb5e30ee"));
        QCOMPARE(station->region, QStringLiteral("四川省成都市"));
        QCOMPARE(station->latitude, 30.6);
        QCOMPARE(station->longitude, 104.1);
        QCOMPARE(station->calcShindo, -3.5);
        QCOMPARE(station->csis, 1);
        QCOMPARE(station->updatedAt, QStringLiteral("2026-10-16 19:15:12"));

        // 缺 `Intensity` 的帧不伪造烈度，置缺测哨兵 -1。
        const QJsonObject noIntensity = QJsonDocument::fromJson(R"({
            "type":"cfbb5286-e6ce-41a4-89e1-3938cb5e30ee","region":"四川省成都市",
            "latitude":30.6,"longitude":104.1,"CalcShindo":-2.0,
            "update_at":"2026-10-16 19:15:12"})").object();
        const auto withoutIntensity = WolfxSeisParser::parseStation(noIntensity);
        QVERIFY(withoutIntensity.has_value());
        QCOMPARE(withoutIntensity->csis, -1);

        // 缺坐标的帧不可展示。
        QVERIFY(!WolfxSeisParser::parseStation({{"type", "abc"}}).has_value());
    }

    void linuxAutostartMigratesStorePath() {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
        qunsetenv("APPIMAGE");
        const QString configHome = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QVERIFY2(configHome.startsWith(settingsDir_.path()), qPrintable(configHome));
        const QString path = configHome + "/autostart/komiraquake.desktop";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));

        QFile legacy(path);
        QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
        legacy.write("[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=KomiraQuake\n"
                     "Exec=\"/nix/store/old-komiraquake/bin/.komiraquake-wrapped\"\n"
                     "Icon=komiraquake\n");
        legacy.close();

        AutoStartService service;
        QVERIFY(service.isEnabled());

        QFile migrated(path);
        QVERIFY(migrated.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(migrated.readAll());
        QVERIFY(content.contains(QStringLiteral("Exec=komiraquake\n")));
        QVERIFY(!content.contains(QStringLiteral("/nix/store/old-komiraquake")));
#endif
    }

    void realTravelAsset() {
        QFile file(QFINDTESTDATA("../assets/travel_times.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto table = QJsonDocument::fromJson(file.readAll()).object()
                               .value("jma2001").toObject();
        const auto depths = table.value("depths").toArray();
        const auto distances = table.value("distances").toArray();
        QVERIFY(depths.size() > 2);
        QVERIFY(distances.size() > 2);
        // An exact interior grid point independently verifies that the service
        // uses the supplied asset, rather than merely returning plausible speeds.
        const int d = depths.size() / 2;
        const int r = distances.size() / 2;
        const auto actual = TravelTimeService::instance().estimate(depths[d].toDouble(), distances[r].toDouble());
        const double p = table.value("p_times").toArray()[d].toArray()[r].toDouble();
        const double s = table.value("s_times").toArray()[d].toArray()[r].toDouble();
        QVERIFY(p > 0 && s > p);
        QVERIFY(std::abs(actual.first - p) < 1e-6);
        QVERIFY(std::abs(actual.second - s) < 1e-6);
    }

    void parserRecomputesLocation() {
        auto parsed = EewParser::parse(payload(), std::nullopt, IntensityStandard::Csis,
                                       "CENC", "wolfx_", origin + 2000);
        QVERIFY(parsed.has_value());
        auto& e = *parsed;
        QCOMPARE(e.distanceKm, EewParser::kUnknownDistance);
        QCOMPARE(e.estimatedIntensity, std::string("--"));
        QCOMPARE(e.rawIntensity, 0.0);
        QVERIFY(!e.pWaveArrival && !e.sWaveArrival);
        QCOMPARE(e.remainingSeconds(origin), -1);
        QCOMPARE(e.eventId, std::string("A"));
        QCOMPARE(e.reportTime, origin + 1000);
        QCOMPARE(e.maxIntensityRaw, 7.0);
        const auto maxText = e.maxIntensityText;
        EewParser::recompute(e, std::make_pair(30.1, 103.1), IntensityStandard::Csis);
        QVERIFY(e.distanceKm > 0);
        QVERIFY(e.rawIntensity > 0);
        QVERIFY(e.estimatedIntensity != "--");
        QVERIFY(e.pWaveArrival && e.sWaveArrival);
        const auto nearDistance = e.distanceKm;
        const auto nearS = *e.sWaveArrival;
        const auto nearIntensity = e.rawIntensity;
        EewParser::recompute(e, std::make_pair(32.0, 105.0), IntensityStandard::Csis);
        QVERIFY(e.distanceKm > nearDistance);
        QVERIFY(e.sWaveArrival && *e.sWaveArrival > nearS);
        QVERIFY(e.rawIntensity < nearIntensity);
        // Invalidated location is represented by nullopt at the parser boundary.
        EewParser::recompute(e, std::nullopt, IntensityStandard::Csis);
        QCOMPARE(e.distanceKm, EewParser::kUnknownDistance);
        QCOMPARE(e.rawIntensity, 0.0);
        QCOMPARE(e.estimatedIntensity, std::string("--"));
        QVERIFY(!e.pWaveArrival && !e.sWaveArrival);
        QCOMPARE(e.warningLevel, EewParser::deriveLevelByMagnitude(e.magnitude, false));
        QCOMPARE(e.maxIntensityRaw, 7.0);
        QCOMPARE(e.maxIntensityText, maxText);
        QCOMPARE(e.eventId, std::string("A"));
        QCOMPARE(e.reportTime, origin + 1000);
    }

    void sequenceCorrectionFinalAndCancelAreEventScoped() {
        WarningSession session;
        auto a = makeEvent();
        auto b = makeEvent("B");
        using Change = WarningSession::Change;
        QCOMPARE(session.accept(a, origin), Change::Added);
        QCOMPARE(session.accept(b, origin), Change::Added);
        QCOMPARE(session.accept(a, origin + 1), Change::Ignored);
        a.reportNum = 2;
        QCOMPARE(session.accept(a, origin + 2), Change::Updated);
        auto stale = a;
        stale.reportNum = 1;
        stale.isCanceled = true;
        QCOMPARE(session.accept(stale, origin + 3), Change::Ignored);
        a.magnitude = 6.0;
        a.maxIntensityRaw = 8;
        a.maxIntensityText = "VIII";
        a.reportTime += 1000;
        QCOMPARE(session.accept(a, origin + 4), Change::Corrected);
        QCOMPARE(session.active.at(a.identity()).event.maxIntensityRaw, 8.0);
        a.isFinal = true;
        QCOMPARE(session.accept(a, origin + 5), Change::Corrected);
        QVERIFY(session.active.at(a.identity()).event.isFinal);
        QCOMPARE(session.active.size(), size_t(2)); // Final is not cancellation.
        a.isCanceled = true;
        QCOMPARE(session.accept(a, origin + 6), Change::Ended);
        QVERIFY(!session.active.count(a.identity()));
        QVERIFY(session.active.count(b.identity()));
        a.isCanceled = false;
        a.reportNum = 3;
        QCOMPARE(session.accept(a, origin + 7), Change::Ignored);
        QCOMPARE(session.active.size(), size_t(1));
    }

    // 跨聚合商的同一份 JMA EEW 报文：identity 不含 provider，二者落进同一会话；
    // 同报次现任优先（不抖动），更高报次由另一路接管（互为备份）。
    void crossSourceLiveReportsMergeIntoOneSession() {
        WarningSession session;
        auto wolfx = makeEvent();
        wolfx.sourceProvider = "Wolfx";
        wolfx.sourceAgency = "JMA";
        wolfx.eventId = "jma_eew:20261005223109";
        wolfx.reportNum = 4;
        wolfx.magnitude = 4.6;
        auto pancakes = wolfx;
        pancakes.sourceProvider = "Pancakes";
        pancakes.magnitude = 4.7;   // 同报次、字段略有差异
        QCOMPARE(wolfx.identity(), pancakes.identity());

        QCOMPARE(session.accept(wolfx, origin), WarningSession::Change::Added);
        // 同报次的另一聚合商：现任优先 → 不覆盖、不新增。
        QCOMPARE(session.accept(pancakes, origin + 1), WarningSession::Change::Ignored);
        QCOMPARE(session.active.size(), size_t(1));
        QCOMPARE(session.active.at(wolfx.identity()).event.magnitude, 4.6);

        // 更高报次由另一路接管。
        pancakes.reportNum = 5;
        pancakes.magnitude = 4.8;
        QCOMPARE(session.accept(pancakes, origin + 2), WarningSession::Change::Updated);
        QCOMPARE(session.active.size(), size_t(1));
        auto& state = session.active.at(wolfx.identity()).event;
        QCOMPARE(state.magnitude, 4.8);
        QCOMPARE(QString::fromStdString(state.sourceProvider), QStringLiteral("Pancakes"));

        // 任一路取消 → 合并键落 tombstone，另一路后续报文被挡住、不复活。
        pancakes.isCanceled = true;
        QCOMPARE(session.accept(pancakes, origin + 3), WarningSession::Change::Ended);
        wolfx.reportNum = 6;
        wolfx.isCanceled = false;
        QCOMPARE(session.accept(wolfx, origin + 4), WarningSession::Change::Ignored);
        QVERIFY(session.active.empty());
    }

    // Jian（api.sismotide.top）：频道映射 + 字段解析 + 跨源合并键。
    void jianRecordsMapToChannelsAndMergeKeys() {
        // 未接入频道（气象/海啸等）不解析。
        QVERIFY(jianChannelFor(QStringLiteral("weather")) == nullptr);
        QVERIFY(jianChannelFor(QStringLiteral("jma-tsunami")) == nullptr);
        const JianChannel* ceaChannel = jianChannelFor(QStringLiteral("cea"));
        const JianChannel* cencChannel = jianChannelFor(QStringLiteral("cenc"));
        const JianChannel* jmaEewChannel = jianChannelFor(QStringLiteral("jma-eew"));
        const JianChannel* jmaChannel = jianChannelFor(QStringLiteral("jma"));
        QVERIFY(ceaChannel && cencChannel && jmaEewChannel && jmaChannel);

        // CEA 速报 → 告警链路；eventId 频道化后与 Wolfx cenc_eew 对齐。
        auto cea = JianParser::parseRecord(
            *ceaChannel,
            QJsonObject{{"id", "202608201100.0001"}, {"number", 2}, {"originTime", static_cast<double>(1787194858000)},
                        {"latitude", 35.789}, {"longitude", 115.7}, {"depth", 16}, {"magnitude", 4.1},
                        {"placeName", "山东菏泽市郓城县"}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(cea.has_value());
        QCOMPARE(QString::fromStdString(cea->sourceProvider), QStringLiteral("Jian"));
        QCOMPARE(QString::fromStdString(cea->sourceAgency), QStringLiteral("CEA"));
        QCOMPARE(QString::fromStdString(cea->eventId), QStringLiteral("cenc_eew:202608201100.0001"));
        QCOMPARE(cea->reportNum, 2);
        QCOMPARE(cea->timestamp, 1787194858000LL);
        QVERIFY(!cea->isFinal);

        // CENC 目录：id 去掉 _M/_A 后与 Wolfx cenc_eqlist 的 EventID 对齐；目录不产生高级别告警。
        auto cenc = JianParser::parseRecord(
            *cencChannel,
            QJsonObject{{"id", "CD.20260819132221.000_M"}, {"originTime", static_cast<double>(1787116941000)},
                        {"latitude", 37.84}, {"longitude", 95.62}, {"depth", 10.0}, {"magnitude", 3.7},
                        {"placeName", "青海海西州直辖区"}, {"infoTypeName", "[正式测定]"}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(cenc.has_value());
        QCOMPARE(QString::fromStdString(cenc->eventId), QStringLiteral("CD.20260819132221.000"));
        QCOMPARE(QString::fromStdString(cenc->sourceAgency), QStringLiteral("CENC"));
        QVERIFY(cenc->isFinal);
        QVERIFY(cenc->warningLevel != WarningLevel::Critical && cenc->warningLevel != WarningLevel::Warning);

        // JMA 速报（EEW）：originTime 是带 +09:00 的 ISO，必须按偏移解析。
        const long long jstEpoch =
            QDateTime::fromString(QStringLiteral("2026-08-20T08:38:21+09:00"), Qt::ISODate)
                .toMSecsSinceEpoch();
        auto jma = JianParser::parseRecord(
            *jmaEewChannel,
            QJsonObject{{"id", "20260820083831"}, {"originTime", "2026-08-20T08:38:21+09:00"},
                        {"latitude", 36.5}, {"longitude", 140.6}, {"depth", 70.0}, {"magnitude", 3.5},
                        {"placeName", "茨城県北部"}, {"infoTypeName", "予報"}, {"intensity", "2"},
                        {"serial", 3}, {"isFinal", true}, {"isCancel", false}},
            std::nullopt, IntensityStandard::Jma);
        QVERIFY(jma.has_value());
        QCOMPARE(jma->timestamp, jstEpoch);
        QCOMPARE(QString::fromStdString(jma->eventId), QStringLiteral("jma_eew:20260820083831"));
        QCOMPARE(QString::fromStdString(jma->sourceAgency), QStringLiteral("JMA"));
        QCOMPARE(QString::fromStdString(jma->maxIntensityText), QStringLiteral("2"));
        QCOMPARE(jma->reportNum, 3);
        QVERIFY(jma->isFinal);

        // JMA 速报（目录）：infoType=取消 → 取消报。
        auto jmaCancel = JianParser::parseRecord(
            *jmaChannel,
            QJsonObject{{"id", "20260820094551"}, {"originTime", "2026-08-20T09:45:00+09:00"},
                        {"latitude", 32.6}, {"longitude", 130.7}, {"depth", 10.0}, {"magnitude", 2.3},
                        {"placeName", "熊本県熊本地方"}, {"infoType", "取消"}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(jmaCancel.has_value());
        QVERIFY(jmaCancel->isCanceled);
        QCOMPARE(QString::fromStdString(jmaCancel->eventId), QStringLiteral("jma_eqlist:20260820094551"));

        // 必填字段缺失 → 丢弃。
        QVERIFY(!JianParser::parseRecord(*cencChannel,
                                        QJsonObject{{"id", "x"}, {"originTime", static_cast<double>(1787116941000)}},
                                        std::nullopt, IntensityStandard::Csis).has_value());
    }

    void jianAuthTokenParsing() {
        QCOMPARE(JianParser::parseAuthToken(QByteArrayLiteral("{\"ok\":true,\"token\":\"rt_x\"}"))
                     .value_or(QString()),
                 QStringLiteral("rt_x"));
        QVERIFY(!JianParser::parseAuthToken(QByteArrayLiteral("{\"ok\":false,\"code\":4101}")).has_value());
        QVERIFY(!JianParser::parseAuthToken(QByteArrayLiteral("not json")).has_value());
    }

    void hiddenMutedStopAndReplay() {
        WarningSession session;
        auto a = makeEvent();
        auto b = makeEvent("B");
        QCOMPARE(session.accept(a, origin), WarningSession::Change::Added);
        QCOMPARE(session.accept(b, origin), WarningSession::Change::Added);
        auto& state = session.active.at(a.identity());
        state.hidden = true;
        state.muted = true;
        state.arrived = true;
        a.reportNum++;
        QCOMPARE(session.accept(a, origin + 1), WarningSession::Change::Updated);
        a.location = "corrected epicenter";
        QCOMPARE(session.accept(a, origin + 2), WarningSession::Change::Corrected);
        QVERIFY(state.hidden && state.muted && state.arrived);
        QVERIFY(!session.active.at(b.identity()).hidden);
        QVERIFY(!session.active.at(b.identity()).muted);
        session.finish(a.identity(), origin + 3); // User stop, not just hide.
        a.reportNum++;
        QCOMPARE(session.accept(a, origin + 4), WarningSession::Change::Ignored);
        QVERIFY(!session.active.count(a.identity()));
        QVERIFY(session.active.count(b.identity()));
    }

    void expiryBoundaries_data() {
        QTest::addColumn<qint64>("sOffset");
        QTest::addColumn<qint64>("expiryOffset");
        QTest::newRow("unknown-five-minutes") << qint64(-1) << qint64(300000);
        QTest::newRow("S-plus-sixty") << qint64(90000) << qint64(150000);
        QTest::newRow("hard-thirty-minutes") << qint64(3600000) << qint64(1800000);
    }
    void expiryBoundaries() {
        QFETCH(qint64, sOffset);
        QFETCH(qint64, expiryOffset);
        auto a = makeEvent();
        if (sOffset >= 0) a.sWaveArrival = origin + sOffset;
        WarningSession session;
        QCOMPARE(session.accept(a, origin), WarningSession::Change::Added);
        QVERIFY(!a.expired(origin + expiryOffset - 1));
        QVERIFY(session.tick(origin + expiryOffset - 1).empty());
        QVERIFY(a.expired(origin + expiryOffset));
        const auto ended = session.tick(origin + expiryOffset);
        QCOMPARE(ended.size(), size_t(1));
        QCOMPARE(ended.front(), a.identity());
        QVERIFY(session.active.empty());
        QVERIFY(session.tick(origin + expiryOffset + 1).empty());
        a.reportNum++;
        QCOMPARE(session.accept(a, origin + expiryOffset + 2), WarningSession::Change::Ignored);
    }

    void correctedOriginCannotExtendHardDeadline() {
        auto a = makeEvent();
        a.sWaveArrival = origin + 3600000;
        WarningSession session;
        QCOMPARE(session.accept(a, origin), WarningSession::Change::Added);
        a.timestamp += 600000;
        a.reportNum++;
        QCOMPARE(session.accept(a, origin + 600000), WarningSession::Change::Updated);
        QCOMPARE(session.active.at(a.identity()).firstOrigin, origin);
        QVERIFY(!a.expired(origin + 1800000));
        QVERIFY(session.tick(origin + 1800000 - 1).empty());
        QCOMPARE(session.tick(origin + 1800000).size(), size_t(1));
    }

    // 本地震源推算服务：投喂一帧合成强度场，必须产出可展示的解，并在关闭开关后清空。
    // 这里检验的是"服务层的簿记与门控"，算法本身的精度由 core_engine_test 覆盖。
    void sourceEstimationServicePublishesAndClears() {
        QSettings().clear();
        SettingsStore settings;
        settings.setSourceEstimationEnabled(true);
        // 逐区开关默认关闭：本用例只验中国区，显式打开它。
        // 中国区默认只对开发者模式开放（只有日本区在真实台网上标定过）。
        settings.setDeveloperMode(true);
        settings.setSourceEstimationRegionEnabled("china", true);
        settings.setSourceEstimationMinStations(6);
        settings.setSourceEstimationTrigger(3.0);

        SourceEstimationService service(&settings);
        long long now = origin;
        service.setNowProvider([&] { return now; });
        // 关掉自动 tick，让测试手动驱动，避免定时器与断言竞争。
        service.ticker_.stop();

        // 合成一场 M5.8 / 深度 10 km 的 CSIS 强度场（中国区）：0.2° 网格覆盖 ±1.6°。
        // 位置用四川而不是东京：CSIS 台网在中国，区域归派按制式 + 坐标判定，
        // 把 CSIS 场画在东京会被判成区域外的坏坐标（见 core 的 sourceRegionForStation）。
        std::vector<StationSnapshot::Entry> frame;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.csis = true;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                frame.push_back(e);
            }
        }
        service.ingestObservations(frame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount >= 6);
        QCOMPARE(service.marker().value("visible").toBool(), false);   // 还没算
        service.runEstimation();
        QVERIFY(service.watcher_.isRunning() || service.regionState(SourceRegion::China).estimate.valid);
        QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::China).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        // waitForFinished 已触发 finished，但需要让直连槽跑完。
        QCoreApplication::processEvents();

        const QVariantMap info = service.info();
        QCOMPARE(info.value("state").toString(), QStringLiteral("ready"));
        QCOMPARE(info.value("visible").toBool(), true);
        QVERIFY(std::abs(info.value("latitude").toDouble() - 30.0) < 0.3);
        QVERIFY(std::abs(info.value("longitude").toDouble() - 104.0) < 0.3);
        QCOMPARE(info.value("id").toString(), QStringLiteral("china"));
        QVERIFY(std::abs(info.value("magnitude").toDouble() - 5.8) < 0.6);
        QVERIFY(info.value("stationCount").toInt() > 20);
        QVERIFY(!info.value("grade").toString().isEmpty());
        QVERIFY(info.value("confidence").toDouble() > 0.5);
        QCOMPARE(info.value("family").toString(), QStringLiteral("csis"));
        QVERIFY(service.marker().value("visible").toBool());

        // 卡片改版（对齐普通地震 HUD）所需的三个量必须在载荷里：
        // ① 地名走参考实现 `getFEName` 的 1° 网格（四川 → 「中国四川附近」）；
        // ② 第几报：首报必须是 1；
        // ③ 发震时刻：强度路径没有到时解，用「首触发 − P 走时」反推，必须落在发震时刻
        //    之前一点点（阈值爬升 + 最近站 P 走时），且不能晚于注入的 `origin`。
        QCOMPARE(info.value("location").toString(), QStringLiteral("中国四川附近"));
        QCOMPARE(info.value("reportNum").toInt(), 1);
        QCOMPARE(info.value("precision").toString(), info.value("grade").toString());
        QCOMPARE(info.value("precisionText").toString(),
                 QStringLiteral("精度 ") + info.value("grade").toString());
        const long long derivedOrigin = info.value("originEpochMs").toLongLong();
        QVERIFY2(derivedOrigin > 0, "强度路径也必须给出反推的发震时刻");
        QVERIFY2(derivedOrigin <= origin,
                 qPrintable(QStringLiteral("反推发震时刻不能晚于首次越阈时刻: %1 > %2")
                                .arg(derivedOrigin).arg(origin)));
        QVERIFY2(origin - derivedOrigin < 60'000,
                 qPrintable(QStringLiteral("反推发震时刻偏差过大: %1 ms").arg(origin - derivedOrigin)));
        QVERIFY(info.value("originText").toString().contains("UTC") == false);
        QCOMPARE(info.value("originText").toString().size(), 19);   // yyyy-MM-dd HH:mm:ss

        // 解没变时重算不得把报次顶上去（1 Hz 重算的常态，必须仍是第 1 报）。
        service.runEstimation();
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();
        QCOMPARE(service.info().value("reportNum").toInt(), 1);
        // 报次跟着「解变了」走：时钟推进让触发簿记重算不出新解时仍为 1，
        // 换一帧更强/更弱的场（震级变 0.3）后必须变成第 2 报。
        {
            std::vector<StationSnapshot::Entry> shifted = frame;
            for (StationSnapshot::Entry& e : shifted) {
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 6.2,
                    QuakeCalculator::haversineDistance(30.0, 104.0, e.lat, e.lon), 10.0);
            }
            now += 5000;
            service.ingestObservations(shifted);
            service.runEstimation();
            QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::China).estimate.valid, 5000);
            if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
            QCoreApplication::processEvents();
            QVERIFY2(service.info().value("reportNum").toInt() >= 2,
                     qPrintable(QStringLiteral("解显著变化后报次未增加: %1")
                                    .arg(service.info().value("reportNum").toInt())));
        }

        // 地图标记的显示开关在 Controller 层裁决（服务只给几何），这里只校验服务侧开关联动。

        // 保持窗口到期后必须清空：证据没了还挂着旧震中是最坏的一种"看起来还行"。
        now += 1000LL * (settings.sourceEstimationHoldSeconds() + 30);
        service.tick();
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        QCOMPARE(service.info().value("state").toString(), QStringLiteral("expired"));
        QCOMPARE(service.marker().value("visible").toBool(), false);

        // 关闭总开关：立即清空，且不再消耗新的观测。
        service.ingestObservations(frame);
        service.runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::China).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();
        settings.setSourceEstimationEnabled(false);
        service.reloadSettings();
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        QCOMPARE(service.info().value("state").toString(), QStringLiteral("disabled"));
        QCOMPARE(service.info().value("visible").toBool(), false);

        // 阈值升高后同一帧不再触发足够多的站。
        settings.setSourceEstimationEnabled(true);
        settings.setSourceEstimationTrigger(6.5);
        settings.setSourceEstimationMinStations(30);
        service.reloadSettings();
        service.ingestObservations(frame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount < 30);
        service.tick();
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
    }

    /// 区域拆分：四个区域各自一套参数、各自出解，互不覆盖。
    /// 这里检验的是服务层的分桶与发布，算法精度仍由 core_engine_test 覆盖。
    /// 日本区走**到时反演**：首次越阈时刻当作 P 波到时，用 JMA2001 走时表反解发震时刻。
    ///
    /// 这条用例覆盖三件强度场做不到的事：① 给出**发震时刻**；② 把深度从「不可辨识」
    /// 变成可辨识（离散候选里唯一可选的档位）；③ 时钟整体平移时发震时刻跟着平移、
    /// 震中不动。合成场用解析走时（vP=6 km/s），与 core 的用例同一套物理。
    void sourceEstimationJapanUsesArrivalTimes() {
        QSettings().clear();
        SettingsStore settings;
        settings.setSourceEstimationEnabled(true);
        // 只开日本区：正是「只开 NIED」的场景。
        settings.setSourceEstimationRegionEnabled("japan", true);
        settings.setSourceEstimationMinStations("japan", 5);
        settings.setSourceEstimationTrigger("japan", 2.0);
        settings.setSourceEstimationClusterRadius("japan", 200);

        SourceEstimationService service(&settings);
        long long now = origin;
        service.setNowProvider([&] { return now; });
        service.ticker_.stop();

        // 在一帧里按「解析 P 走时」错开各站的首次越阈时刻：这正是 NIED 真实数据的样子
        // （波前扫过时邻站依次越过阈值）。必须先逐帧投喂，才能让 service 记录 onset。
        const double eqLat = 35.0;
        const double eqLon = 139.0;
        const double depthKm = 10.0;
        const long long eqOrigin = origin;
        std::vector<StationSnapshot::Entry> frame;
        for (double lat = 34.0; lat <= 36.0 + 1e-9; lat += 0.25) {
            for (double lon = 138.0; lon <= 140.0 + 1e-9; lon += 0.25) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Jma, 6.2,
                    QuakeCalculator::haversineDistance(eqLat, eqLon, lat, lon), depthKm);
                frame.push_back(e);
            }
        }

        // 逐帧推进：每帧只让「到时已到」的站显示高值，其余站保持在阈值以下。
        // 这样 service 记录的 firstTriggerMs 就是各站的 P 到时。
        for (int second = 0; second <= 40; ++second) {
            now = eqOrigin + static_cast<long long>(second) * 1000;
            std::vector<StationSnapshot::Entry> tick = frame;
            for (StationSnapshot::Entry& e : tick) {
                const double distance =
                    QuakeCalculator::haversineDistance(eqLat, eqLon, e.lat, e.lon);
                const double travel = std::sqrt(depthKm * depthKm + distance * distance) / 6.0;
                if (travel > static_cast<double>(second) + 0.5) {
                    e.raw = 0.0;   // 波前还没到
                }
            }
            service.ingestObservations(tick);
        }
        now = eqOrigin + 42'000;
        service.ingestObservations(frame);

        QVERIFY(service.regionState(SourceRegion::Japan).triggeredCount >= 5);
        service.runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(
            service.regionState(SourceRegion::Japan).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();

        const SourceEstimate japan = service.regionState(SourceRegion::Japan).estimate;
        QVERIFY(japan.valid);
        QCOMPARE(japan.region, SourceRegion::Japan);
        QVERIFY2(japan.method.rfind("arrival_time", 0) == 0,
                 qPrintable(QStringLiteral("日本区应走到时反演，实际 method=%1")
                                .arg(QString::fromStdString(japan.method))));
        QVERIFY2(QuakeCalculator::haversineDistance(japan.latitude, japan.longitude, eqLat, eqLon)
                     < 60.0,
                 qPrintable(QStringLiteral("到时解偏离真值过远: %1,%2")
                                .arg(japan.latitude).arg(japan.longitude)));
        QVERIFY2(std::llabs(japan.originEpochMs - eqOrigin) < 5000,
                 qPrintable(QStringLiteral("发震时刻偏差 %1 ms")
                                .arg(japan.originEpochMs - eqOrigin)));
        QVERIFY(japan.pPhaseCount > 0);

        // 载荷里必须带出可展示的发震时刻（HH:mm:ss）与相位计数，卡片据此换脚注。
        const QVariantMap info = service.info();
        const QVariantList regions = info.value("regions").toList();
        QCOMPARE(regions.size(), 4);
        const QVariantMap japanRow = regions[1].toMap();
        QCOMPARE(japanRow.value("id").toString(), QStringLiteral("japan"));
        QCOMPARE(japanRow.value("usesArrivalTimes").toBool(), true);
        QVERIFY(!japanRow.value("originUtc8").toString().isEmpty());
        QVERIFY(japanRow.value("pPhases").toInt() > 0);

        // 中国区没有到时证据：同一帧里没有 CSIS 站，必须保持无解（不能被日本区的解污染）。
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        QCOMPARE(regions[0].toMap().value("visible").toBool(), false);
    }

    void sourceEstimationRegionsAreIndependent() {
        QSettings().clear();
        SettingsStore settings;
        settings.setSourceEstimationEnabled(true);
        // 中国 / 韩国 / 台湾三区默认只对开发者模式开放（只有日本区在真实台网上标定过），
        // 本用例要四区同跑，先开开发者模式。
        settings.setDeveloperMode(true);
        // 四个区都显式打开（逐区开关默认关闭）。
        for (const char* region : {"china", "japan", "korea", "taiwan"}) {
            settings.setSourceEstimationRegionEnabled(region, true);
        }
        // 三片台网各给一套可区分的参数：中国 6 站、日本 2 站（好让日本更快满足）、
        // 台湾 1 站（单站粗解）、韩国 5 站。
        settings.setSourceEstimationMinStations("china", 6);
        settings.setSourceEstimationMinStations("japan", 2);
        settings.setSourceEstimationMinStations("korea", 2);
        settings.setSourceEstimationMinStations("taiwan", 1);
        settings.setSourceEstimationTrigger("china", 3.0);
        settings.setSourceEstimationTrigger("japan", 2.0);
        settings.setSourceEstimationTrigger("korea", 3.5);
        settings.setSourceEstimationTrigger("taiwan", 3.0);

        SourceEstimationService service(&settings);
        long long now = origin;
        service.setNowProvider([&] { return now; });
        service.ticker_.stop();

        // 一帧里同时放三片台网：四川 CSIS、东京 JMA、首尔 MMI、台南 CSIS。
        std::vector<StationSnapshot::Entry> frame;
        const auto addGrid = [&](double centerLat, double centerLon, double magnitude,
                                 IntensityFamily family, double radiusDeg) {
            for (double lat = centerLat - radiusDeg; lat <= centerLat + radiusDeg + 1e-9;
                 lat += 0.2) {
                for (double lon = centerLon - radiusDeg; lon <= centerLon + radiusDeg + 1e-9;
                     lon += 0.2) {
                    StationSnapshot::Entry e;
                    e.lat = lat;
                    e.lon = lon;
                    const double distance = QuakeCalculator::haversineDistance(centerLat, centerLon,
                                                                               lat, lon);
                    if (family == IntensityFamily::Jma) {
                        e.raw = SourceEstimator::predictedIntensity(family, magnitude, distance,
                                                                    10.0);
                    } else if (family == IntensityFamily::Kma) {
                        e.kma = true;
                        e.raw = SourceEstimator::predictedIntensity(family, magnitude, distance,
                                                                    10.0);
                    } else {
                        e.csis = true;
                        e.raw = SourceEstimator::predictedIntensity(family, magnitude, distance,
                                                                    10.0);
                    }
                    frame.push_back(e);
                }
            }
        };
        addGrid(30.0, 104.0, 5.8, IntensityFamily::Csis, 1.2);   // 中国
        addGrid(35.7, 139.7, 5.2, IntensityFamily::Jma, 0.8);    // 日本
        addGrid(37.5, 127.0, 4.6, IntensityFamily::Kma, 0.6);    // 韩国
        // 台湾：只放一个站 —— 正是「CWA 未接入、只有个别 Wolfx 站」的现状。
        {
            StationSnapshot::Entry e;
            e.lat = 23.0;
            e.lon = 120.3;
            e.csis = true;
            e.raw = 5.4;
            frame.push_back(e);
        }

        service.ingestObservations(frame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount >= 6);
        QVERIFY(service.regionState(SourceRegion::Japan).triggeredCount >= 2);
        QVERIFY(service.regionState(SourceRegion::Korea).triggeredCount >= 2);
        QCOMPARE(service.regionState(SourceRegion::Taiwan).triggeredCount, 1);

        service.runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::China).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();

        // 四区各自出解，位置互不串。
        const SourceEstimate china = service.regionState(SourceRegion::China).estimate;
        const SourceEstimate japan = service.regionState(SourceRegion::Japan).estimate;
        const SourceEstimate korea = service.regionState(SourceRegion::Korea).estimate;
        const SourceEstimate taiwan = service.regionState(SourceRegion::Taiwan).estimate;
        QVERIFY(china.valid);
        QVERIFY(japan.valid);
        QVERIFY(korea.valid);
        QVERIFY(taiwan.valid);
        QCOMPARE(china.region, SourceRegion::China);
        QCOMPARE(japan.region, SourceRegion::Japan);
        QCOMPARE(korea.region, SourceRegion::Korea);
        QCOMPARE(taiwan.region, SourceRegion::Taiwan);
        QVERIFY(QuakeCalculator::haversineDistance(china.latitude, china.longitude, 30.0, 104.0)
                < 60.0);
        QVERIFY(QuakeCalculator::haversineDistance(japan.latitude, japan.longitude, 35.7, 139.7)
                < 60.0);
        QVERIFY(QuakeCalculator::haversineDistance(korea.latitude, korea.longitude, 37.5, 127.0)
                < 80.0);
        // 台湾只有一个站：只能是 provisional 粗解，且必须落在那站附近。
        QVERIFY(taiwan.provisional);
        QVERIFY(QuakeCalculator::haversineDistance(taiwan.latitude, taiwan.longitude, 23.0, 120.3)
                < 40.0);

        // 载荷：regions 固定四行、每行 id 可读；markers 每个有解的区域一个。
        const QVariantMap info = service.info();
        const QVariantList regions = info.value("regions").toList();
        QCOMPARE(regions.size(), 4);
        QCOMPARE(regions[0].toMap().value("id").toString(), QStringLiteral("china"));
        QCOMPARE(regions[1].toMap().value("id").toString(), QStringLiteral("japan"));
        QCOMPARE(regions[2].toMap().value("id").toString(), QStringLiteral("korea"));
        QCOMPARE(regions[3].toMap().value("id").toString(), QStringLiteral("taiwan"));
        for (const QVariant& row : regions) {
            QVERIFY(row.toMap().value("visible").toBool());
        }
        QCOMPARE(regions[3].toMap().value("provisional").toBool(), true);
        const QVariantList markers = service.marker().value("markers").toList();
        QCOMPARE(markers.size(), 4);
        QCOMPARE(markers[3].toMap().value("id").toString(), QStringLiteral("taiwan"));
        QVERIFY(service.marker().value("visible").toBool());
        // 顶层字段与最强的一区一致（兼容只读单值的调用方）。
        QVERIFY(!info.value("id").toString().isEmpty());
        QCOMPARE(info.value("visible").toBool(), true);
        QCOMPARE(info.value("markers").toList().size(), 4);

        // 只改日本区的门槛：中国区已有的解必须原样保留（区域互不作废）。
        const SourceEstimate chinaBefore = china;
        settings.setSourceEstimationMinStations("japan", 40);
        service.reloadSettings();
        QCoreApplication::processEvents();
        QVERIFY(service.regionState(SourceRegion::China).estimate.valid);
        QCOMPARE(service.regionState(SourceRegion::China).estimate.latitude,
                 chinaBefore.latitude);
        QVERIFY(!service.regionState(SourceRegion::Japan).estimate.valid);
        // 阈值改动会清空该区的触发簿记，重新投喂后仍能再出解。
        service.ingestObservations(frame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount >= 6);

        // 关掉总开关：四区一起清空。
        settings.setSourceEstimationEnabled(false);
        service.reloadSettings();
        for (const SourceRegion region : sourceRegions()) {
            QVERIFY(!service.regionState(region).estimate.valid);
        }
        QCOMPARE(service.info().value("visible").toBool(), false);
        QVERIFY(service.info().value("markers").toList().isEmpty());
    }

    /// 逐区域开关：可以只开某一片台网（例如只开日本 NIED）。
    ///
    /// 语义是「启用集合」且**默认全关**：总开关打开但一个区都没选时，任何区都不记账、
    /// 不计算、不发布 —— 不能出现「打开总开关就自动全区开跑」。
    void sourceEstimationRegionToggles() {
        QSettings().clear();
        SettingsStore settings;
        // 本用例末尾要「重新打开中国区」，而中国区默认只对开发者模式开放。
        settings.setDeveloperMode(true);
        QVERIFY(!settings.isSourceEstimationRegionEnabled("china"));
        QVERIFY(!settings.isSourceEstimationRegionEnabled("japan"));
        QVERIFY(!settings.hasAnySourceEstimationRegion());
        QVERIFY(settings.enabledSourceEstimationRegions().isEmpty());

        settings.setSourceEstimationEnabled(true);
        settings.setSourceEstimationRegionEnabled("japan", true);
        QVERIFY(settings.isSourceEstimationRegionEnabled("japan"));
        QVERIFY(!settings.isSourceEstimationRegionEnabled("china"));
        QVERIFY(settings.hasAnySourceEstimationRegion());
        QCOMPARE(settings.enabledSourceEstimationRegions(), QStringList{"japan"});

        // 幂等：重复设同一状态不发 changed。
        {
            QSignalSpy spy(&settings, &SettingsStore::changed);
            settings.setSourceEstimationRegionEnabled("japan", true);
            QCOMPARE(spy.count(), 0);
            settings.setSourceEstimationRegionEnabled("japan", false);
            QCOMPARE(spy.count(), 1);
        }
        settings.setSourceEstimationRegionEnabled("japan", true);

        // 服务层：只开日本时，喂一帧中国 CSIS 场必须完全没有动静。
        SourceEstimationService service(&settings);
        long long now = origin;
        service.setNowProvider([&] { return now; });
        service.ticker_.stop();

        std::vector<StationSnapshot::Entry> chinaFrame;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.csis = true;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                chinaFrame.push_back(e);
            }
        }
        service.ingestObservations(chinaFrame);
        QCOMPARE(service.regionState(SourceRegion::China).triggeredCount, 0);
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        service.runEstimation();
        QCoreApplication::processEvents();
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        // 载荷里中国行必须是 disabled，且不带解。
        {
            const QVariantList regions = service.info().value("regions").toList();
            QCOMPARE(regions.size(), 4);
            const QVariantMap china = regions[0].toMap();
            QCOMPARE(china.value("id").toString(), QStringLiteral("china"));
            QCOMPARE(china.value("regionEnabled").toBool(), false);
            QCOMPARE(china.value("enabled").toBool(), false);
            QCOMPARE(china.value("state").toString(), QStringLiteral("disabled"));
            QCOMPARE(china.value("visible").toBool(), false);
            const QVariantMap japan = regions[1].toMap();
            QCOMPARE(japan.value("regionEnabled").toBool(), true);
        }
        QCOMPARE(service.info().value("anyRegionEnabled").toBool(), true);

        // 日本区仍能正常出解（喂日本 JMA 场）。
        std::vector<StationSnapshot::Entry> japanFrame;
        for (double lat = 34.0; lat <= 36.0 + 1e-9; lat += 0.2) {
            for (double lon = 138.0; lon <= 140.0 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Jma, 6.4,
                    QuakeCalculator::haversineDistance(35.0, 139.0, lat, lon), 10.0);
                japanFrame.push_back(e);
            }
        }
        now += 5000;
        service.ingestObservations(japanFrame);
        QVERIFY(service.regionState(SourceRegion::Japan).triggeredCount >= 5);
        service.runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::Japan).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);

        // 关掉日本区：该区的解必须立刻清空，且不再记账。
        settings.setSourceEstimationRegionEnabled("japan", false);
        service.reloadSettings();
        QVERIFY(!service.regionState(SourceRegion::Japan).estimate.valid);
        QCOMPARE(service.regionState(SourceRegion::Japan).triggeredCount, 0);
        QCOMPARE(service.info().value("anyRegionEnabled").toBool(), false);
        // 一个区都没有：载荷状态是 disabled，不是 collecting。
        QCOMPARE(service.info().value("state").toString(), QStringLiteral("disabled"));

        // 重新打开中国区：可以立刻工作。
        settings.setSourceEstimationRegionEnabled("china", true);
        service.reloadSettings();
        now += 5000;
        service.ingestObservations(chinaFrame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount >= 6);
        service.runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(service.regionState(SourceRegion::China).estimate.valid, 5000);
        if (service.watcher_.isRunning()) service.watcher_.waitForFinished();
        QCoreApplication::processEvents();
        QVERIFY(service.regionState(SourceRegion::China).estimate.valid);
    }

    /// 区域可用性门控：中国 / 韩国 / 台湾 只有开发者模式才「可配置且生效」。
    ///
    /// 需求原话是「只有开启了开发者模式才显示」——显示只是表象，真正的契约是
    /// **生效**：关掉开发者模式后，即使设置里仍留着中国区的开启标记，服务层也
    /// 必须把它当关处理，签名里不能出现它。可用性由
    /// `SettingsStore::isSourceEstimationRegionAvailable` 一处判定，四处共用
    /// （QML 卡片显隐、快照 enabled/available、签名、服务层 syncConfig）。
    void sourceEstimationRegionAvailabilityFollowsDeveloperMode() {
        QSettings().clear();
        SettingsStore settings;
        settings.setSourceEstimationEnabled(true);

        // 日本区永远可用；其余三区默认不可用。
        QVERIFY(settings.isSourceEstimationRegionAvailable("japan"));
        QVERIFY(!settings.isSourceEstimationRegionAvailable("china"));
        QVERIFY(!settings.isSourceEstimationRegionAvailable("korea"));
        QVERIFY(!settings.isSourceEstimationRegionAvailable("taiwan"));

        // 用户在开发者模式下打开三区。
        settings.setDeveloperMode(true);
        QVERIFY(settings.isSourceEstimationRegionAvailable("china"));
        QVERIFY(settings.isSourceEstimationRegionAvailable("korea"));
        QVERIFY(settings.isSourceEstimationRegionAvailable("taiwan"));
        for (const char* region : {"china", "korea", "taiwan"}) {
            settings.setSourceEstimationRegionEnabled(region, true);
        }
        settings.setSourceEstimationRegionEnabled("japan", true);
        // 原始启用集合保留用户的选择（不因可用性被改写）。列表是插入序，比较前排序。
        {
            QStringList raw = settings.enabledSourceEstimationRegions();
            raw.sort();
            QCOMPARE(raw, QStringList({"china", "japan", "korea", "taiwan"}));
        }
        // 签名只含「可用且已启用」的区域。
        QCOMPARE(settings.sourceEstimationRegionSignature(),
                 QStringLiteral("china,japan,korea,taiwan"));
        QVERIFY(settings.sourceEstimationRegionActive());

        // 关掉开发者模式：三区立刻从「生效」里消失，日本区不受影响。
        settings.setDeveloperMode(false);
        QCOMPARE(settings.sourceEstimationRegionSignature(), QStringLiteral("japan"));
        QVERIFY(settings.sourceEstimationRegionActive());
        // 用户的选择还在（开发者模式是开关，不是清空）。
        {
            QStringList raw = settings.enabledSourceEstimationRegions();
            raw.sort();
            QCOMPARE(raw, QStringList({"china", "japan", "korea", "taiwan"}));
        }

        // 快照与签名同口径：三区 available=false 且 enabled=false，日本区可用。
        {
            const QVariantMap params = settings.sourceEstimationRegionParams();
            for (const char* region : {"china", "korea", "taiwan"}) {
                const QVariantMap row = params.value(region).toMap();
                QCOMPARE(row.value("available").toBool(), false);
                QCOMPARE(row.value("enabled").toBool(), false);
            }
            const QVariantMap japan = params.value("japan").toMap();
            QCOMPARE(japan.value("available").toBool(), true);
            QCOMPARE(japan.value("enabled").toBool(), true);
        }

        // 只有日本区时，签名非空（这正是 QML 用来判断「有区域在跑」的属性）。
        settings.setSourceEstimationRegionEnabled("japan", false);
        QVERIFY(settings.sourceEstimationRegionSignature().isEmpty());
        QVERIFY(!settings.sourceEstimationRegionActive());

        // 服务层同一判据：开发者模式关掉后，中国区即使标记为启用也不记账、不出解。
        settings.setDeveloperMode(true);
        settings.setSourceEstimationRegionEnabled("china", true);
        SourceEstimationService service(&settings);
        long long now = origin;
        service.setNowProvider([&] { return now; });
        service.ticker_.stop();
        settings.setDeveloperMode(false);
        service.reloadSettings();
        std::vector<StationSnapshot::Entry> frame;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.csis = true;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                frame.push_back(e);
            }
        }
        service.ingestObservations(frame);
        service.runEstimation();
        QCoreApplication::processEvents();
        QCOMPARE(service.regionState(SourceRegion::China).triggeredCount, 0);
        QVERIFY(!service.regionState(SourceRegion::China).estimate.valid);
        QCOMPARE(service.info().value("anyRegionEnabled").toBool(), false);
        QCOMPARE(service.info().value("state").toString(), QStringLiteral("disabled"));

        // 开发者模式一开，同一片台网立刻能算（关着的时候那些观测根本没记账，
        // 所以这里要重新喂一帧，和 sourceEstimationRegionToggles 里的口径一致）。
        settings.setDeveloperMode(true);
        service.reloadSettings();
        now += 5000;
        service.ingestObservations(frame);
        QVERIFY(service.regionState(SourceRegion::China).triggeredCount >= 6);
    }

    void settingsChangedIsIdempotentAndPersistent() {
        QSettings().clear();
        SettingsStore settings;
        QSignalSpy changed(&settings, &SettingsStore::changed);
        QVERIFY(changed.isValid());
        settings.setSourceEnabled("wolfx", false);
        QCOMPARE(changed.count(), 1);
        settings.setSourceEnabled("wolfx", false);
        QCOMPARE(changed.count(), 1);
        settings.setIsMuted(true);
        settings.setIntensityStandard(1);
        QCOMPARE(changed.count(), 3);
        SettingsStore reloaded;
        QVERIFY(!reloaded.isSourceEnabled("wolfx"));
        QVERIFY(reloaded.isMuted());
        QCOMPARE(reloaded.intensityStandard(), 1);
        settings.resetToDefaults();
        QCOMPARE(changed.count(), 4);
        QVERIFY(settings.isSourceEnabled("wolfx"));
        QVERIFY(!settings.isMuted());
        QCOMPARE(settings.intensityStandard(), 0);
    }

    /// 子数据源默认全部启用；禁用键按 `sourceId:subId` 隔离（同名频道跨源互不影响），
    /// 且必须经 QSettings 往返存活。
    void subSourcesDefaultEnabledAndScopedPerSource() {
        QSettings().clear();
        SettingsStore settings;
        QSignalSpy changed(&settings, &SettingsStore::disabledSubSourcesChanged);
        QVERIFY(changed.isValid());

        QVERIFY(settings.isSubSourceEnabled("wolfx", "jma_eew"));
        QVERIFY(settings.isSubSourceEnabled("whews", "jma_eew"));

        settings.setSubSourceEnabled("wolfx", "jma_eew", false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!settings.isSubSourceEnabled("wolfx", "jma_eew"));
        // 同名频道在别的聚合源下不受影响；也不影响顶层源开关。
        QVERIFY(settings.isSubSourceEnabled("whews", "jma_eew"));
        QVERIFY(settings.isSourceEnabled("wolfx"));
        // 幂等：重复关闭不再发信号。
        settings.setSubSourceEnabled("wolfx", "jma_eew", false);
        QCOMPARE(changed.count(), 1);

        SettingsStore reloaded;
        QVERIFY(!reloaded.isSubSourceEnabled("wolfx", "jma_eew"));
        QVERIFY(reloaded.isSubSourceEnabled("whews", "jma_eew"));

        // 分组批量启停（全开 / 全不选）：一次写入，只发一次通知。
        const QStringList ids = {"cenc_eew", "sc_eew", "jma_eew"};
        reloaded.setSubSourcesEnabled("wolfx", ids, false);
        QVERIFY(!reloaded.isSubSourceEnabled("wolfx", "cenc_eew"));
        QVERIFY(!reloaded.isSubSourceEnabled("wolfx", "sc_eew"));
        reloaded.setSubSourcesEnabled("wolfx", ids, true);
        QVERIFY(reloaded.isSubSourceEnabled("wolfx", "jma_eew"));

        reloaded.resetToDefaults();
        QVERIFY(reloaded.disabledSubSources().isEmpty());
        QVERIFY(reloaded.isSubSourceEnabled("wolfx", "jma_eew"));
    }

    /// 子源目录必须覆盖各聚合源实际接入的频道，且源内 id 唯一、名称非空。
    void subSourceCatalogCoversAggregatorChannels() {
        const QVector<SubSourceInfo>& wolfx = subSourcesFor("wolfx");
        QCOMPARE(wolfx.size(), 8); // 6 个 EEW + 2 个目录端点
        QCOMPARE(wolfx.first().id, QStringLiteral("cenc_eew"));
        QCOMPARE(wolfx.last().id, QStringLiteral("jma_eqlist"));
        QVERIFY(wolfx.last().kind == SourceEventKind::Directory);

        QCOMPARE(subSourcesFor("pancakes").size(), 4);
        QCOMPARE(subSourcesFor("jian").size(), 7);
        QCOMPARE(subSourcesFor("whews").size(), 41);
        QVERIFY(subSourcesFor("simulated").isEmpty());

        for (const QString& sourceId : {"wolfx", "pancakes", "jian", "whews"}) {
            QSet<QString> seen;
            for (const SubSourceInfo& sub : subSourcesFor(sourceId)) {
                QVERIFY2(!seen.contains(sub.id), qPrintable(sourceId + ':' + sub.id));
                seen.insert(sub.id);
                QVERIFY(!sub.name.isEmpty());
            }
        }
    }

    /// 逐项 NOTIFY 必须真的隔离：改一个设置只能惊动它自己的绑定。
    ///
    /// 回归点（2026-10）：49 个属性原先全挂聚合的 changed()，改任意一项都会让 QML 里
    /// `app.settings.*` 的全部绑定（160 处引用）重新求值 —— 设置页拖滑块时每帧重算整棵
    /// 地图子树的绑定。拆分之后必须保证：① 相关信号发出；② 不相关的信号不发；
    /// ③ 恢复默认时**所有**信号都发（否则 UI 停在旧值上，曾让主题按钮不回落）。
    void settingsPerPropertySignalsAreIsolated() {
        QSettings().clear();
        SettingsStore settings;
        QSignalSpy aggregate(&settings, &SettingsStore::changed);
        QSignalSpy theme(&settings, &SettingsStore::themeModeChanged);
        QSignalSpy blur(&settings, &SettingsStore::backgroundBlurChanged);
        QSignalSpy basemap(&settings, &SettingsStore::basemapIdChanged);
        QSignalSpy stationLayer(&settings, &SettingsStore::showStationLayerChanged);
        QSignalSpy regionParams(&settings, &SettingsStore::sourceEstimationRegionParamsChanged);
        QVERIFY(theme.isValid() && blur.isValid() && basemap.isValid());
        QVERIFY(stationLayer.isValid() && regionParams.isValid());

        settings.setThemeMode(QStringLiteral("dark"));
        QCOMPARE(theme.count(), 1);
        QCOMPARE(blur.count(), 0);
        QCOMPARE(basemap.count(), 0);
        QCOMPARE(stationLayer.count(), 0);
        QCOMPARE(aggregate.count(), 1);

        // 写同样的值必须完全静默（幂等）。
        settings.setThemeMode(QStringLiteral("dark"));
        QCOMPARE(theme.count(), 1);
        QCOMPARE(aggregate.count(), 1);

        settings.setBackgroundBlur(false);
        QCOMPARE(blur.count(), 1);
        QCOMPARE(theme.count(), 1);

        // 区域参数是复合键，四个字段共用一个信号。
        settings.setSourceEstimationTrigger(QStringLiteral("japan"), 3.5);
        QCOMPARE(regionParams.count(), 1);
        settings.setSourceEstimationClusterRadius(QStringLiteral("china"), 300);
        QCOMPARE(regionParams.count(), 2);

        // 区域**可用性**是另一条依赖：快照 / 签名 / sourceEstimationRegionActive 都挂在
        // `sourceEstimationRegionStateChanged` 上，开发者模式一变就必须发，否则 QML 里
        // 「开关点开了，下面滑块还是灰的」会原样复现（绑定不重算）。
        {
            QSignalSpy regionState(&settings, &SettingsStore::sourceEstimationRegionStateChanged);
            QVERIFY(regionState.isValid());
            settings.setDeveloperMode(true);
            QVERIFY2(regionState.count() >= 1, "开发者模式必须惊动区域状态绑定");
            const int afterDevMode = regionState.count();
            settings.setSourceEstimationRegionEnabled(QStringLiteral("china"), true);
            QVERIFY2(regionState.count() > afterDevMode, "逐区开关必须惊动区域状态绑定");
            // 聚合信号与区域信号是两回事：改开发者模式不该让主题绑定重算。
            QCOMPARE(theme.count(), 1);
            QCOMPARE(blur.count(), 1);
            settings.setDeveloperMode(false);
        }

        // 恢复默认：所有逐项信号都要发出去。
        const int beforeTheme = theme.count();
        const int beforeBlur = blur.count();
        const int beforeStations = stationLayer.count();
        settings.resetToDefaults();
        QVERIFY2(theme.count() > beforeTheme, "恢复默认必须发出 themeModeChanged");
        QVERIFY2(blur.count() > beforeBlur, "恢复默认必须发出 backgroundBlurChanged");
        QVERIFY2(stationLayer.count() > beforeStations,
                 "恢复默认必须发出 showStationLayerChanged");
    }

    /// 新增的提醒矩阵 / 声音分轨 / 图层 / 逐源过滤默认值，以及旧通知键的迁移。
    void granularSettingsDefaultsAndMigration() {
        QSettings().clear();
        {
            SettingsStore settings;
            QVERIFY(settings.notifyAlert());
            QVERIFY(!settings.raiseAlert());
            QVERIFY(!settings.notifyEew());
            QVERIFY(!settings.raiseEew());
            QVERIFY(!settings.notifyInfo());
            QVERIFY(!settings.raiseInfo());
            QVERIFY(settings.announceEvent());
            QVERIFY(settings.announceUpdate());
            QVERIFY(settings.announceCountdown());
            QVERIFY(settings.enableAlarmTone());
            QVERIFY(settings.showStationLayer());
            QVERIFY(settings.showWavefront());
            QVERIFY(settings.showProvinceBorders());
            QVERIFY(settings.closeToTray());
            QVERIFY(settings.isStationSourceEnabled(QStringLiteral("kmoni")));
            QCOMPARE(settings.infoMagnitudeFilter(QStringLiteral("wolfx")), 0.0);
            QVERIFY(settings.passesInfoMagnitudeFilter(QStringLiteral("wolfx"), 3.0));

            // 逐源过滤：不接收 / 低于阈值 / 未知震级。
            settings.setInfoMagnitudeFilter(QStringLiteral("wolfx"), -1.0);
            QVERIFY(!settings.passesInfoMagnitudeFilter(QStringLiteral("wolfx"), 6.0));
            settings.setInfoMagnitudeFilter(QStringLiteral("wolfx"), 4.0);
            QVERIFY(!settings.passesInfoMagnitudeFilter(QStringLiteral("wolfx"), 3.9));
            QVERIFY(settings.passesInfoMagnitudeFilter(QStringLiteral("wolfx"), 4.0));
            QVERIFY(settings.passesInfoMagnitudeFilter(QStringLiteral("wolfx"), 0.0));

            // 本地震源推算（Beta）：**默认关闭**、保留 60 s、地图标记开。
            QVERIFY(!settings.sourceEstimationEnabled());
            QCOMPARE(settings.sourceEstimationHoldSeconds(), 60);
            QVERIFY(settings.showEstimatedEpicenter());
            // 区域默认值：中国 CSIS 3.0 / 6 站 / 250 km，日本 JMA 2.5 / 8 站 / 200 km，
            // 韩国 MMI 4.0 / 5 站 / 200 km，台湾 3.0 / 1 站（台网未接入）。
            QCOMPARE(settings.sourceEstimationTriggerFor("china"), 3.0);
            QCOMPARE(settings.sourceEstimationMinStationsFor("china"), 6);
            QCOMPARE(settings.sourceEstimationClusterRadiusFor("china"), 250);
            QCOMPARE(settings.sourceEstimationTriggerFor("japan"), 2.5);
            // 日本区门槛对齐 kanameishi 的 `minInferenceStationCount = 5`。
            QCOMPARE(settings.sourceEstimationMinStationsFor("japan"), 5);
            QCOMPARE(settings.sourceEstimationClusterRadiusFor("japan"), 200);
            QCOMPARE(settings.sourceEstimationTriggerFor("korea"), 4.0);
            QCOMPARE(settings.sourceEstimationMinStationsFor("korea"), 5);
            QCOMPARE(settings.sourceEstimationMinStationsFor("taiwan"), 1);
            QVERIFY(settings.hasSourceEstimationRegion("taiwan"));
            QVERIFY(!settings.hasSourceEstimationRegion("mars"));
            QCOMPARE(settings.sourceEstimationRegionLabel("korea"), QStringLiteral("韩国"));
            // 无参重载 = 中国区（旧调用兼容）。
            QCOMPARE(settings.sourceEstimationTrigger(), 3.0);
            QCOMPARE(settings.sourceEstimationMinStations(), 6);
            QVERIFY(settings.sourceEstimationQuietEvidence());

            settings.setSourceEstimationEnabled(true);
            settings.setSourceEstimationTrigger(4.5);
            settings.setSourceEstimationMinStations(9);
            settings.setSourceEstimationQuietEvidence(false);
            settings.setSourceEstimationHoldSeconds(150);
            settings.setSourceEstimationClusterRadius(600);
            // 日本区单独调一套，验证「一个区的改动不会溢出到另一个区」。
            settings.setSourceEstimationTrigger("japan", 1.5);
            settings.setSourceEstimationMinStations("japan", 12);
            settings.setSourceEstimationClusterRadius("japan", 350);
            settings.setSourceEstimationQuietEvidence("japan", false);
            settings.setShowEstimatedEpicenter(false);

            settings.setStationSourceEnabled(QStringLiteral("kmoni"), false);
            QVERIFY(!settings.isStationSourceEnabled(QStringLiteral("kmoni")));
            settings.setNotifyEew(true);
            settings.setAnnounceCountdown(false);
        }
        SettingsStore reloaded;
        QVERIFY(reloaded.notifyEew());
        QVERIFY(!reloaded.announceCountdown());
        QVERIFY(reloaded.sourceEstimationEnabled());
        QCOMPARE(reloaded.sourceEstimationTrigger(), 4.5);
        QCOMPARE(reloaded.sourceEstimationMinStations(), 9);
        QVERIFY(!reloaded.sourceEstimationQuietEvidence());
        QCOMPARE(reloaded.sourceEstimationHoldSeconds(), 150);
        QCOMPARE(reloaded.sourceEstimationClusterRadius(), 600);
        QCOMPARE(reloaded.sourceEstimationTriggerFor("japan"), 1.5);
        QCOMPARE(reloaded.sourceEstimationMinStationsFor("japan"), 12);
        QCOMPARE(reloaded.sourceEstimationClusterRadiusFor("japan"), 350);
        QVERIFY(!reloaded.sourceEstimationQuietEvidenceFor("japan"));
        // 韩国/台湾没被碰过，仍是各自的默认值。
        QCOMPARE(reloaded.sourceEstimationTriggerFor("korea"), 4.0);
        QCOMPARE(reloaded.sourceEstimationMinStationsFor("taiwan"), 1);
        QVERIFY(!reloaded.showEstimatedEpicenter());
        QVERIFY(!reloaded.isStationSourceEnabled(QStringLiteral("kmoni")));
        QCOMPARE(reloaded.infoMagnitudeFilter(QStringLiteral("wolfx")), 4.0);
        // 恢复默认必须把震源推算一并还原（否则「恢复默认」会留下一个关不掉的旧阈值）。
        reloaded.resetToDefaults();
        QVERIFY(!reloaded.sourceEstimationEnabled());
        QCOMPARE(reloaded.sourceEstimationTrigger(), 3.0);
        QCOMPARE(reloaded.sourceEstimationMinStations(), 6);
        QVERIFY(reloaded.sourceEstimationQuietEvidence());
        QCOMPARE(reloaded.sourceEstimationHoldSeconds(), 60);
        QCOMPARE(reloaded.sourceEstimationClusterRadius(), 250);
        QCOMPARE(reloaded.sourceEstimationTriggerFor("japan"), 2.5);
        QCOMPARE(reloaded.sourceEstimationMinStationsFor("japan"), 5);
        QVERIFY(reloaded.showEstimatedEpicenter());
        // 单区恢复默认：只清该区，其余区域与总开关不受影响。
        reloaded.setSourceEstimationMinStations("japan", 20);
        reloaded.setSourceEstimationMinStations("korea", 11);
        reloaded.resetSourceEstimationRegion("japan");
        QCOMPARE(reloaded.sourceEstimationMinStationsFor("japan"), 5);
        QCOMPARE(reloaded.sourceEstimationMinStationsFor("korea"), 11);
        // 恢复默认同样清空测站源禁用集与逐源过滤（resetToDefaults 的既有语义）。
        QVERIFY(reloaded.isStationSourceEnabled(QStringLiteral("kmoni")));
        QCOMPARE(reloaded.infoMagnitudeFilter(QStringLiteral("wolfx")), 0.0);

        // 区域拆分前的全局键必须迁移进中国区（否则老用户调好的阈值会静默失效）。
        QSettings().clear();
        QSettings().setValue("sourceEstimationTrigger", 5.0);
        QSettings().setValue("sourceEstimationMinStations", 11);
        QSettings().setValue("sourceEstimationClusterRadius", 400);
        QSettings().setValue("sourceEstimationQuietEvidence", false);
        SettingsStore migratedRegion;
        QCOMPARE(migratedRegion.sourceEstimationTriggerFor("china"), 5.0);
        QCOMPARE(migratedRegion.sourceEstimationMinStationsFor("china"), 11);
        QCOMPARE(migratedRegion.sourceEstimationClusterRadiusFor("china"), 400);
        QVERIFY(!migratedRegion.sourceEstimationQuietEvidenceFor("china"));
        // 旧键迁移只写中国区，别的区域保持各自默认。
        QCOMPARE(migratedRegion.sourceEstimationTriggerFor("japan"), 2.5);
        // 迁移后旧键不再残留（否则下次启动会再抄一遍，覆盖用户后来的修改）。
        QVERIFY(!QSettings().contains("sourceEstimationTrigger"));

        // 旧键迁移：新键缺失时沿用旧通知/置顶设置。
        QSettings().clear();
        QSettings().setValue("enableDesktopNotification", false);
        QSettings().setValue("enableWindowRaise", true);
        SettingsStore migrated;
        QVERIFY(!migrated.notifyAlert());
        QVERIFY(migrated.raiseAlert());
    }

    // 需要鉴权的源默认关闭，且未填凭据前不建立连接。
    void credentialSourcesAreOffByDefault() {
        QSettings().clear();
        SettingsStore settings;
        QVERIFY(!settings.isSourceEnabled(QStringLiteral("jian")));
        QVERIFY(settings.isSourceEnabled(QStringLiteral("wolfx")));
        QVERIFY(settings.isSourceEnabled(QStringLiteral("pancakes")));
        QVERIFY(!settings.jianConfigured());
        // 手动开启后仍保持"已配置=假"，直到登录成功写入刷新令牌。
        settings.setSourceEnabled(QStringLiteral("jian"), true);
        QVERIFY(settings.isSourceEnabled(QStringLiteral("jian")));
        QVERIFY(!settings.jianConfigured());
        settings.setJianRefreshToken(QStringLiteral("rt_x"));
        QVERIFY(settings.jianConfigured());
    }

    // Whews 同样需要令牌：默认关闭，粘贴令牌后标记为已配置。
    void whewsTokenIsOptionalAndGatesTheSource() {
        QSettings().clear();
        SettingsStore settings;
        QVERIFY(!settings.isSourceEnabled(QStringLiteral("whews")));
        QVERIFY(!settings.whewsConfigured());
        WhewsSource source;
        QVERIFY(!source.isConfigured());
        // 手动开启但未填令牌：仍视为未配置，上层不会启动本源。
        settings.setSourceEnabled(QStringLiteral("whews"), true);
        QVERIFY(!settings.whewsConfigured());
        QVERIFY(!source.isConfigured());
        settings.setWhewsToken(QStringLiteral("  wat_x  "));
        QVERIFY(settings.whewsConfigured());
        QCOMPARE(settings.whewsToken(), QStringLiteral("wat_x"));   // 去空白后持久化
        source.setToken(settings.whewsToken());
        QVERIFY(source.isConfigured());
    }

    void themeDefaultsAndPersistence() {
        QSettings().clear();
        {
            SettingsStore settings;
            // 全新安装默认跟随系统（与安卓端一致）。
            QCOMPARE(settings.themeMode(), QString("system"));
            settings.setThemeMode("dark");
        }
        {
            SettingsStore reloaded;
            QCOMPARE(reloaded.themeMode(), QString("dark"));
            reloaded.setThemeMode("system");
        }
        SettingsStore settings;
        QCOMPARE(settings.themeMode(), QString("system"));
        settings.setThemeMode("dark");
        settings.resetToDefaults();
        QCOMPARE(settings.themeMode(), QString("system"));
        QVERIFY(!QSettings().contains("themeMode"));
        QVERIFY(!QSettings().contains("darkMode"));
    }

    void themeModeMigratesLegacyDarkMode() {
        // 旧版只有布尔 darkMode：true → dark，false → light。
        QSettings().clear();
        QSettings().setValue("darkMode", true);
        QCOMPARE(SettingsStore().themeMode(), QString("dark"));
        QSettings().clear();
        QSettings().setValue("darkMode", false);
        QCOMPARE(SettingsStore().themeMode(), QString("light"));
        // themeMode 一旦存在即为唯一来源，旧键不再干扰。
        QSettings().setValue("themeMode", "system");
        QCOMPARE(SettingsStore().themeMode(), QString("system"));
        // 非法值回落到跟随系统，不崩也不返回脏值。
        QSettings().setValue("themeMode", "nonsense");
        QCOMPARE(SettingsStore().themeMode(), QString("system"));
    }

    void versionCompare() {
        // 点分数值比较；忽略 v 前缀与预发布后缀。
        QVERIFY(isNewerVersion("1.0.0", "1.0.1"));
        QVERIFY(isNewerVersion("1.0.0", "v1.1.0"));
        QVERIFY(isNewerVersion("1.0.1", "1.1.0-rc2"));
        QVERIFY(isNewerVersion("1.0", "1.0.1"));
        QVERIFY(isNewerVersion("1.0.0", "2.0"));
        QVERIFY(!isNewerVersion("1.0.1", "1.0.1"));
        QVERIFY(!isNewerVersion("1.2.0", "1.1.9"));
        QVERIFY(!isNewerVersion("1.0.1", "1.0.1-rc1")); // 同版本预发布不算更新
    }

    void cityCoordTableResolvesWithFallbacks() {
        const QByteArray asset = R"({
            "provinces": {"四川": [30.65, 104.07], "北京": [39.9, 116.4]},
            "cities": {"四川|广安": [30.45, 106.63]},
            "cities_unique": {"广安": [30.45, 106.63]}
        })";
        QVERIFY(CityCoordTable::instance().loadFromString(asset));
        QVERIFY(CityCoordTable::instance().isLoaded());
        // 省市精确（两侧都归一化后缀）。
        const auto exact = CityCoordTable::instance().resolve(QStringLiteral("四川省"), QStringLiteral("广安市"));
        QVERIFY(exact.has_value());
        QCOMPARE(exact->first, 30.45);
        QCOMPARE(exact->second, 106.63);
        // 市名命中但省份不同 → 唯一市名回退。
        const auto byCity = CityCoordTable::instance().resolve(QStringLiteral("未知省"), QStringLiteral("广安市"));
        QVERIFY(byCity.has_value());
        QCOMPARE(byCity->first, 30.45);
        // 市查不到 → 省级中心回退（直辖市走这条）。
        const auto byProvince = CityCoordTable::instance().resolve(QStringLiteral("北京市"), QStringLiteral("北京市"));
        QVERIFY(byProvince.has_value());
        QCOMPARE(byProvince->first, 39.9);
        // 完全无匹配。
        QVERIFY(!CityCoordTable::instance().resolve(QStringLiteral("不存在省"), QStringLiteral("不存在市")).has_value());
    }

    void ipGeoResponseParsesProvinceAndCity() {
        const auto cn = parseIpGeoResponse(QByteArrayLiteral(
            R"({"ip":"1.2.3.4","location":{"country":"中国","province":"四川省","city":"广安市"}})"));
        QVERIFY(cn.has_value());
        QCOMPARE(cn->province, QStringLiteral("四川省"));
        QCOMPARE(cn->city, QStringLiteral("广安市"));
        // anycast / 非 CN：无 location 字段。
        QVERIFY(!parseIpGeoResponse(QByteArrayLiteral(R"({"ip":"1.1.1.1","asn":{"number":13335}})")).has_value());
        QVERIFY(!parseIpGeoResponse(QByteArrayLiteral("not json")).has_value());
    }

    void disabledSourceIgnoresQueuedMessage() {
        WolfxSource source;
        source.setNowProvider([] { return origin + 2000; });
        // Arm the parser without opening a remote connection or HTTP request.
        source.running_ = true;
        source.socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
        QSignalSpy received(&source, &WolfxSource::eventReceived);
        QSignalSpy info(&source, &WolfxSource::infoChanged);
        QVERIFY(received.isValid());
        const auto text = QString::fromUtf8(QJsonDocument(payload()).toJson());
        source.handleMessage(text);
        QCOMPARE(received.count(), 1); // Positive control: this is a valid live message.
        const auto emitted = qvariant_cast<EarthquakeEvent>(received.at(0).at(0));
        // eventId 带频道前缀（cenc_eew:），供跨聚合商对齐合并键。
        QCOMPARE(emitted.eventId, std::string("cenc_eew:A"));
        QCOMPARE(emitted.sourceProvider, std::string("Wolfx"));
        bool delivered = false;
        QMetaObject::invokeMethod(&source, [&] {
            source.handleMessage(text);
            source.refreshDirectory();
            delivered = true;
        }, Qt::QueuedConnection);
        SettingsStore settings;
        settings.setSourceEnabled("wolfx", true);
        connect(&settings, &SettingsStore::changed, &source, [&] {
            if (!settings.isSourceEnabled("wolfx")) source.stop();
        });
        settings.setSourceEnabled("wolfx", false);
        const int infoAfterStop = info.count();
        QTRY_VERIFY(delivered);
        QCOMPARE(received.count(), 1);
        QCOMPARE(info.count(), infoAfterStop);
        QVERIFY(!source.running_);
        QVERIFY(!source.queryTimer_.isActive());
        QVERIFY(!source.pollTimer_.isActive());
        QVERIFY(!source.reconnectTimer_.isActive());
        QCOMPARE(source.info().status, ConnectionStatus::Disconnected);
        QCOMPARE(source.info().directoryStatus, ConnectionStatus::Disconnected);
    }

    void pancakesRealtimeMapping() {
        auto envelope = [](const QString& source, const QString& action, const QJsonObject& payload) {
            return QJsonObject{{"source", source}, {"type", QStringLiteral("earthquake")},
                               {"action", action}, {"timestampMs", static_cast<double>(origin)},
                               {"payload", payload}};
        };
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("usgs", "update",
                         {{"eventId", "us7000abcd"}, {"placeName", "Somewhere"}, {"latitude", -12.5},
                          {"longitude", 166.25}, {"depth", 35.0}, {"magnitude", 5.8},
                          {"originTimeMs", static_cast<double>(origin)},
                          {"updatedTimeMs", static_cast<double>(origin + 5000)},
                          {"infoType", "Reviewed"}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(parsed.has_value());
            QCOMPARE(parsed->kind, SourceEventKind::Live);
            QCOMPARE(parsed->event.eventId, std::string("usgs:us7000abcd"));
            QCOMPARE(parsed->event.sourceProvider, std::string("Pancakes"));
            QCOMPARE(parsed->event.sourceAgency, std::string("USGS"));
            QVERIFY(parsed->event.isFinal);
            QCOMPARE(parsed->event.reportTime, origin + 5000);
            // USGS 的 placeName 是英文原文，按坐标换成震源区中文名。
            QCOMPARE(QString::fromStdString(parsed->event.location),
                     QStringLiteral("圣克鲁斯群岛附近"));
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("gq", "archived",
                         {{"id", "abc"}, {"latitude", 35.0}, {"longitude", 140.0}, {"depth", 10.0},
                          {"magnitude", 6.5}, {"originTimeMs", static_cast<double>(origin)},
                          {"region", "関東"}, {"revisionId", 3},
                          {"lastUpdateMs", static_cast<double>(origin + 500)}, {"intensity", "VIII"}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(parsed.has_value());
            QVERIFY(parsed->event.isFinal);
            QCOMPARE(parsed->event.reportNum, 4);
            QCOMPARE(QString::fromStdString(parsed->event.maxIntensityText), QStringLiteral("VIII"));
            // GQ 的 region 也换成同一张震源区表的中文名。
            QCOMPARE(QString::fromStdString(parsed->event.location),
                     QStringLiteral("日本本州东岸近海附近"));
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("gq", "cancelled", {{"id", "abc"}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(parsed.has_value());
            QVERIFY(parsed->event.isCanceled);
            QCOMPARE(parsed->event.reportNum, PancakesProtocol::kCancelReportNum);
            // 取消报文没有坐标，不能被 0/0 的震源区名覆盖。
            QCOMPARE(QString::fromStdString(parsed->event.location), QStringLiteral("已取消"));
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("jma_eew", "update",
                         {{"EventID", "20231114221320"}, {"Serial", 4},
                          {"AnnouncedTime", "2023-11-14T22:13:30+09:00"},
                          {"OriginTime", "2023-11-14T22:13:20+09:00"}, {"Hypocenter", "東京湾"},
                          {"Latitude", 35.5}, {"Longitude", 139.8}, {"Magunitude", 6.1},
                          {"Depth", 20.0}, {"MaxIntensity", "5+"}, {"isFinal", false},
                          {"isCancel", false}}),
                std::nullopt, IntensityStandard::Jma, origin);
            QVERIFY(parsed.has_value());
            QCOMPARE(parsed->kind, SourceEventKind::Live);
            QCOMPARE(parsed->event.eventId, std::string("jma_eew:20231114221320"));
            QCOMPARE(parsed->event.reportNum, 4);
            QCOMPARE(parsed->event.maxIntensityRaw, 5.5);
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("jma_eqlist", "update",
                         {{"eventId", "20231114221320"},
                          {"originTime", "2023-11-14T22:13:20+09:00"}, {"placeName", "東京湾"},
                          {"latitude", 35.5}, {"longitude", 139.8}, {"depth", 20.0},
                          {"magnitude", 4.5}, {"maxIntensity", "3"}, {"serial", 1},
                          {"reportTime", "2023-11-14T22:21:00+09:00"}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(parsed.has_value());
            QCOMPARE(parsed->kind, SourceEventKind::Directory);
            QVERIFY(parsed->event.isFinal);
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("cma", "update", {{"id", "x"}, {"latitude", 1.0}, {"longitude", 2.0}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(!parsed.has_value());
        }
    }

    void pancakesListMapping() {
        const QJsonObject item{{"source", "usgs"}, {"eventId", "us7000abcd"}, {"status", "active"},
                               {"revision", static_cast<double>(origin)},
                               {"originTime", "2026-10-05T11:19:29.04Z"}, {"magnitude", 4.9},
                               {"depthKm", 10.0}, {"place", "Somewhere"}, {"latitude", 38.8},
                               {"longitude", -122.8}};
        auto event = PancakesParser::parseListItem(item, std::nullopt, IntensityStandard::Csis, origin);
        QVERIFY(event.has_value());
        QCOMPARE(event->eventId, std::string("usgs:us7000abcd"));
        QCOMPARE(event->sourceAgency, std::string("USGS"));
        QCOMPARE(event->timestamp,
                 QDateTime::fromString(QStringLiteral("2026-10-05T11:19:29.04Z"), Qt::ISODate)
                     .toMSecsSinceEpoch());
        QCOMPARE(event->reportTime, origin);
        QVERIFY(!event->isCanceled);
        // 列表项与实时报文走同一条命名规则。
        QCOMPARE(QString::fromStdString(event->location), QStringLiteral("加利福尼亚州北部附近"));
    }

    // Wolfx 的 JMA 报文时刻是无时区 JST(UTC+9) 墙钟，必须按 JST 解析，才能与 Pancakes 的
    // 同一地震（带偏移 ISO）对齐；两路 jma_eqlist 由此合并为一条，互为备份而不重复。
    void jmaTimesUseJstAndSourcesAgree() {
        const auto expected = QDateTime::fromString(QStringLiteral("2026-10-06T04:47:00Z"),
                                                    Qt::ISODate).toMSecsSinceEpoch();
        const QJsonObject wolfxJma{
            {"Title", "震源・震度情報"}, {"EventID", "20261006134727"},
            {"time", "2026/10/06 13:47"}, {"time_full", "2026/10/06 13:47:00"},
            {"location", "熊本県熊本地方"}, {"magnitude", "2.9"}, {"shindo", "1"},
            {"depth", "10km"}, {"latitude", "32.6"}, {"longitude", "130.7"}, {"info", ""}};
        auto jma = EewParser::parseJmaDirectory(wolfxJma, std::nullopt, IntensityStandard::Csis);
        QVERIFY(jma.has_value());
        QCOMPARE(jma->timestamp, expected);
        QCOMPARE(jma->eventId, std::string("jma_eqlist:20261006134727"));
        // 只有分钟精度（time，无 time_full）也必须按 JST 解析成功。
        QJsonObject minuteOnly = wolfxJma;
        minuteOnly.remove(QStringLiteral("time_full"));
        auto minute = EewParser::parseJmaDirectory(minuteOnly, std::nullopt, IntensityStandard::Csis);
        QVERIFY(minute.has_value());
        QCOMPARE(minute->timestamp, expected);

        // Wolfx jma_eew 的 OriginTime 同样是 JST 墙钟。
        auto eew = EewParser::parse(
            QJsonObject{{"type", "jma_eew"}, {"EventID", "20261005223109"},
                        {"OriginTime", "2026/10/05 22:30:47"}, {"Hypocenter", "石垣島北西沖"},
                        {"Latitude", 25.1}, {"Longitude", 123.3}, {"Magnitude", 4.6},
                        {"Depth", 140}, {"MaxIntensity", "2"}, {"isCancel", false}},
            std::nullopt, IntensityStandard::Jma, QStringLiteral("JMA 紧急地震速报"),
            QStringLiteral("wolfx_"), origin, /*originTimeIsJst=*/true);
        QVERIFY(eew.has_value());
        QCOMPARE(eew->timestamp, QDateTime::fromString(QStringLiteral("2026-10-05T13:30:47Z"),
                                                       Qt::ISODate)
                                    .toMSecsSinceEpoch());

        // Pancakes 的同一条目（eventId 相同、originTime 为带 Z 的 ISO）应与之判为同一地震。
        const QJsonObject pancakesItem{
            {"source", "jma_eqlist"}, {"eventId", "20261006134727"}, {"status", "active"},
            {"revision", static_cast<double>(origin)},
            {"originTime", "2026-10-06T04:47:00Z"}, {"magnitude", 2.9}, {"depthKm", 10.0},
            {"place", "熊本県熊本地方"}, {"maxIntensity", "1"}, {"latitude", 32.6},
            {"longitude", 130.7}};
        auto pan = PancakesParser::parseListItem(pancakesItem, std::nullopt,
                                                 IntensityStandard::Csis, origin);
        QVERIFY(pan.has_value());
        QCOMPARE(pan->timestamp, expected);
        QVERIFY(QuakeCalculator::isSameQuake(jma->timestamp, jma->latitude, jma->longitude,
                                             pan->timestamp, pan->latitude, pan->longitude));
    }

    // QWebSocket 传输失败会同时发 errorOccurred 与 disconnected：一次连接尝试只应调度
    // 一次重连，否则 retryCount_ 被翻倍、退避瞬间顶到 15s 上限。
    void reconnectScheduledOncePerAttempt() {        WolfxSource source;
        source.running_ = true;
        source.generation_ = 1;

        // 顺序一：errorOccurred 先于 disconnected。
        {
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
            socket->disconnected();
            QCOMPARE(source.retryCount_, 1);
            QVERIFY(source.reconnectTimer_.isActive());
            source.reconnectTimer_.stop();
        }

        // 顺序二：disconnected 先于 errorOccurred。
        {
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->disconnected();
            socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
            QCOMPARE(source.retryCount_, 2);
            QVERIFY(source.reconnectTimer_.isActive());
        }

        source.stop();
        QVERIFY(!source.reconnectTimer_.isActive());
    }

    void historyMigratesAndRoundTrips() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("legacy.sqlite");
        const QString connection = "qt_business_legacy_fixture";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE events (id TEXT PRIMARY KEY, source TEXT NOT NULL, "
                           "magnitude REAL NOT NULL, latitude REAL NOT NULL, longitude REAL NOT NULL, "
                           "depth REAL NOT NULL, location TEXT NOT NULL, timestamp INTEGER NOT NULL, "
                           "distance_km REAL NOT NULL, estimated_intensity TEXT NOT NULL, "
                           "raw_intensity REAL NOT NULL, p_wave_arrival INTEGER, s_wave_arrival INTEGER, "
                           "warning_level INTEGER NOT NULL, report_num INTEGER NOT NULL, "
                           "is_final INTEGER NOT NULL, is_canceled INTEGER NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO events VALUES ('legacy','CENC',4,30,103,10,'old',1000,"
                           "-1,'--',0,NULL,NULL,0,1,0,0)"));
        }
        QSqlDatabase::removeDatabase(connection);
        auto a = makeEvent();
        a.maxIntensityRaw = 6.5;
        a.maxIntensityText = "6強";
        a.pWaveArrival = origin + 12000;
        a.sWaveArrival = origin + 23000;
        a.reportNum = 4;
        a.isFinal = true;
        a.isInfo = true;
        a.determination = "reviewed";
        {
            HistoryStore store;
            QVERIFY(store.init(path));
            QVERIFY(store.isReady());
            const auto legacy = store.loadRecent();
            QCOMPARE(legacy.size(), qsizetype(1));
            QCOMPARE(legacy.front().id, std::string("legacy"));
            QCOMPARE(legacy.front().eventId, std::string());
            QCOMPARE(legacy.front().reportTime, 0LL);
            QCOMPARE(legacy.front().maxIntensityRaw, 0.0);
            QVERIFY(legacy.front().maxIntensityText.empty());
            QVERIFY(!legacy.front().pWaveArrival && !legacy.front().sWaveArrival);
            store.upsertAll({a});
            store.saveTombstone(QString::fromStdString(a.identity()), origin + 100);
        }
        {
            HistoryStore store;
            QVERIFY(store.init(path)); // Migration is idempotent, including on reopen.
            const auto tombstones = store.loadTombstones(origin + 1000);
            QCOMPARE(tombstones.value(QString::fromStdString(a.identity())), origin + 100);
            WarningSession restarted;
            for (auto it = tombstones.cbegin(); it != tombstones.cend(); ++it)
                restarted.finish(it.key().toStdString(), it.value());
            QCOMPARE(restarted.accept(a, origin + 1000), WarningSession::Change::Ignored);
            auto rows = store.loadRecent();
            QCOMPARE(rows.size(), qsizetype(2));
            const auto& saved = rows.front();
            QCOMPARE(saved.id, a.id);
            QCOMPARE(saved.eventId, a.eventId);
            QCOMPARE(saved.reportTime, a.reportTime);
            QCOMPARE(saved.identity(), a.identity());
            QCOMPARE(saved.maxIntensityRaw, a.maxIntensityRaw);
            QCOMPARE(saved.maxIntensityText, a.maxIntensityText);
            QCOMPARE(saved.rawIntensity, 0.0); // Source max is not local intensity.
            QCOMPARE(saved.estimatedIntensity, std::string("--"));
            QVERIFY(saved.pWaveArrival == a.pWaveArrival);
            QVERIFY(saved.sWaveArrival == a.sWaveArrival);
            QCOMPARE(saved.reportNum, 4);
            QVERIFY(saved.isFinal);
            // 情报标记与测定状态也要跨重启存活：详情页靠它们标「情报/正式测定」。
            QVERIFY(saved.isInfo);
            QCOMPARE(saved.determination, std::string("reviewed"));
            a.reportNum = 5;
            a.reportTime += 2000;
            a.maxIntensityRaw = 7;
            a.maxIntensityText = "7";
            a.isCanceled = true;
            a.pWaveArrival.reset();
            a.sWaveArrival.reset();
            store.upsertAll({a});
            rows = store.loadRecent();
            QCOMPARE(rows.size(), qsizetype(2)); // Stable ID updates, not duplicates.
            QCOMPARE(rows.front().reportTime, a.reportTime);
            QCOMPARE(rows.front().maxIntensityRaw, 7.0);
            QCOMPARE(rows.front().maxIntensityText, std::string("7"));
            QVERIFY(rows.front().isCanceled);
            QVERIFY(!rows.front().pWaveArrival && !rows.front().sWaveArrival);
            QCOMPARE(store.loadRecent(1).size(), qsizetype(1));
            store.prune(1);
            QCOMPARE(store.loadRecent().size(), qsizetype(1));
            QCOMPARE(store.loadRecent().front().id, a.id);
            store.clear();
            QVERIFY(store.loadRecent().empty());
        }
    }

    /// 报次归档的键与主键语义：同一场地震的预警（`n:2`）与情报（`i:<发布时刻>`）
    /// 各自成行；升级前的老表按 `n:<报数>` 原地搬家，不丢历史。
    void reportArchiveKeysSeparateInfoFromEew() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("reports.sqlite");
        const std::string agency = "CENC";
        const long long origin = 1'800'000'000'000LL;
        const QString key = quakePrimaryGroupKey(agency, origin, 30.0, 103.0);
        QVERIFY(!key.isEmpty());
        {
            HistoryStore store;
            QVERIFY(store.init(path));
            StoredReport eew;
            eew.quakeKey = key;
            eew.reportKey = QStringLiteral("n:2");
            eew.reportNum = 2;
            eew.sourceId = QStringLiteral("wolfx");
            eew.sourceTag = QStringLiteral("Wolfx·CENC");
            eew.receivedAt = origin + 5000;
            eew.rawJson = QStringLiteral(R"({"n":2})");
            eew.timestamp = origin;
            eew.reportTime = origin + 4000;
            eew.magnitude = 5.2;
            eew.latitude = 30.0;
            eew.longitude = 103.0;
            eew.location = QStringLiteral("灾测震中");
            StoredReport info = eew;
            info.reportKey = QStringLiteral("i:%1").arg(origin + 600'000);
            info.reportNum = 1;
            info.isInfo = true;
            info.determination = QStringLiteral("reviewed");
            info.sourceId = QStringLiteral("pancakes");
            info.sourceTag = QStringLiteral("Pancakes·CENC");
            // 同一报数（1）却是不同报次槽：旧主键 (key, report_num, source) 已不足以区分。
            StoredReport eewFirst = eew;
            eewFirst.reportKey = QStringLiteral("n:1");
            eewFirst.reportNum = 1;
            eewFirst.sourceId = QStringLiteral("pancakes");
            eewFirst.sourceTag = QStringLiteral("Pancakes·CENC");
            store.upsertReports({eew, info, eewFirst});
            const auto rows = store.loadReports(key);
            QCOMPARE(rows.size(), qsizetype(3));
            bool sawReviewed = false;
            bool sawEew2 = false;
            for (const StoredReport& r : rows) {
                if (r.isInfo && r.determination == QStringLiteral("reviewed")) sawReviewed = true;
                if (!r.isInfo && r.reportNum == 2 && r.reportKey == QStringLiteral("n:2"))
                    sawEew2 = true;
            }
            QVERIFY(sawReviewed);
            QVERIFY(sawEew2);
        }
        {
            // 幂等：重开不会重复写。
            HistoryStore store;
            QVERIFY(store.init(path));
            QCOMPARE(store.loadReports(key).size(), qsizetype(3));
        }

        // 升级前的归档表（无 report_key，quake_key = 机构|发震时刻毫秒）要能被搬过来。
        const auto legacyPath = dir.filePath("legacy_reports.sqlite");
        const QString legacyKey = QStringLiteral("cenc|") + QString::number(origin);
        {
            const QString connection = "qt_business_report_legacy";
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(legacyPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE event_reports (quake_key TEXT NOT NULL, "
                           "report_num INTEGER NOT NULL, source_id TEXT NOT NULL, "
                           "source_tag TEXT NOT NULL, received_at INTEGER NOT NULL, "
                           "raw_json TEXT NOT NULL DEFAULT '', timestamp INTEGER NOT NULL, "
                           "report_time INTEGER NOT NULL, magnitude REAL NOT NULL, "
                           "depth REAL NOT NULL, latitude REAL NOT NULL, longitude REAL NOT NULL, "
                           "location TEXT NOT NULL, max_intensity_text TEXT NOT NULL DEFAULT '', "
                           "is_final INTEGER NOT NULL, is_canceled INTEGER NOT NULL, "
                           "PRIMARY KEY (quake_key, report_num, source_id))"));
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO event_reports VALUES (?,1,'wolfx','Wolfx·CENC',?,?,?,?,5.0,10.0,"
                "30.0,103.0,'旧报次','',0,0)"));
            insert.addBindValue(legacyKey);
            insert.addBindValue(origin + 1000);
            insert.addBindValue(QStringLiteral(R"({"legacy":true})"));
            insert.addBindValue(origin);
            insert.addBindValue(origin + 900);
            QVERIFY(insert.exec());
        }
        QSqlDatabase::removeDatabase("qt_business_report_legacy");
        {
            HistoryStore store;
            QVERIFY(store.init(legacyPath));
            // 旧键的毫秒被重算成分钟桶 + 震中网格：新键与 quakePrimaryGroupKey 一致。
            const auto rows = store.loadReports(key);
            QCOMPARE(rows.size(), qsizetype(1));
            QCOMPARE(rows.front().reportKey, QStringLiteral("n:1"));
            QCOMPARE(rows.front().location, QStringLiteral("旧报次"));
            QVERIFY(rows.front().rawJson.contains("legacy"));
        }
    }

    // ---- Whews (api.2v8.cn) ----

    // 站点顺序：国内备用站在前，主站在后（需求：优先备用站，不可用才用主站）。
    void whewsPrefersDomesticSiteFirst() {
        const QStringList hosts = WhewsProtocol::wsHosts();
        QCOMPARE(hosts.size(), 2);
        QCOMPARE(hosts.at(0), QStringLiteral("wss://api.2v8.cn"));
        QCOMPARE(hosts.at(1), QStringLiteral("wss://api.beecld.com"));
        QCOMPARE(WhewsProtocol::providerName(), QStringLiteral("Whews"));
    }

    // 时刻是无时区墙钟：非 JMA 频道按 UTC+8、JMA 频道按 UTC+9 解释。
    // 这是接入 Whews 最易错的一处：按本地时区解释会让非 UTC+8 设备的时刻整体偏移。
    void whewsParsesWallClockPerChannelTimeZone() {
        const auto* cenc = whewsChannelFor(QStringLiteral("cenc"));
        QVERIFY(cenc);
        QCOMPARE(cenc->tz, WhewsTimeZone::Utc8);
        const auto cencEvent = WhewsParser::parseRecord(
            *cenc,
            QJsonObject{{"id", QStringLiteral("CD.20260813084717.000")},
                        {"shockTime", QStringLiteral("2026-08-13 08:47:00")},
                        {"latitude", 36.06}, {"longitude", 103.55}, {"depth", 11.0},
                        {"magnitude", 3.2}, {"placeName", QString::fromUtf8("甘肃临夏州永靖县")}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(cencEvent.has_value());
        // 08:47:00 +08:00 == 00:47:00Z
        QCOMPARE(cencEvent->timestamp,
                 QDateTime::fromString(QStringLiteral("2026-08-13T00:47:00Z"), Qt::ISODate)
                     .toMSecsSinceEpoch());
        // 只有 USGS 频道换地名；CENC 自带的行政区名原样保留。
        QCOMPARE(QString::fromStdString(cencEvent->location),
                 QString::fromUtf8("甘肃临夏州永靖县"));

        const auto* jma = whewsChannelFor(QStringLiteral("jma_eew"));
        QVERIFY(jma);
        QCOMPARE(jma->tz, WhewsTimeZone::Jst);
        const auto jmaEvent = WhewsParser::parseRecord(
            *jma,
            QJsonObject{{"id", QStringLiteral("20240101161010")}, {"updates", 10},
                        {"shockTime", QStringLiteral("2024-01-01 16:10:08")},
                        {"latitude", 37.5}, {"longitude", 137.3}, {"depth", 10.0},
                        {"magnitude", 6.2}, {"epiIntensity", QStringLiteral("6+")}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(jmaEvent.has_value());
        // 16:10:08 +09:00 == 07:10:08Z（若误按 UTC+8 会差 1 小时）
        QCOMPARE(jmaEvent->timestamp,
                 QDateTime::fromString(QStringLiteral("2024-01-01T07:10:08Z"), Qt::ISODate)
                     .toMSecsSinceEpoch());
        QCOMPARE(jmaEvent->reportNum, 10);
        QCOMPARE(jmaEvent->maxIntensityRaw, 6.5);   // JMA 震度文本 "6+"
    }

    // USGS 频道的地名是英文原文（「10 km NE of ...」），与 GQ / 震源推算一致，
    // 按坐标查 1°×1° 震源区表补中文名。
    void whewsUsgsUsesRegionNameTable() {
        const auto* usgs = whewsChannelFor(QStringLiteral("usgs"));
        QVERIFY(usgs);
        const auto event = WhewsParser::parseRecord(
            *usgs,
            QJsonObject{{"id", QStringLiteral("us7000abcd")},
                        {"shockTime", QStringLiteral("2026-10-05 11:19:29")},
                        {"latitude", 38.8}, {"longitude", -122.8}, {"depth", 10.0},
                        {"magnitude", 4.9},
                        {"placeName", QStringLiteral("10 km NE of Berkeley, CA")}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(event.has_value());
        QCOMPARE(QString::fromStdString(event->location), QStringLiteral("加利福尼亚州北部附近"));
    }

    // createTime / updateTime 同样按频道时区解析。
    void whewsParsesReportTimePerChannelTimeZone() {
        const auto* cenc = whewsChannelFor(QStringLiteral("cenc"));
        const auto cencEvent = WhewsParser::parseRecord(
            *cenc,
            QJsonObject{{"id", QStringLiteral("CD.1")},
                        {"shockTime", QStringLiteral("2026-08-13 08:47:00")},
                        {"createTime", QStringLiteral("2026-08-13 08:51:51")},
                        {"latitude", 36.06}, {"longitude", 103.55}, {"magnitude", 3.2}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(cencEvent.has_value());
        QCOMPARE(cencEvent->reportTime,
                 QDateTime::fromString(QStringLiteral("2026-08-13T00:51:51Z"), Qt::ISODate)
                     .toMSecsSinceEpoch());

        const auto* jma = whewsChannelFor(QStringLiteral("jma"));
        const auto jmaEvent = WhewsParser::parseRecord(
            *jma,
            QJsonObject{{"id", QStringLiteral("20240101161010")},
                        {"shockTime", QStringLiteral("2024-01-01 16:10:08")},
                        {"createTime", QStringLiteral("2024-01-01 16:12:00")},
                        {"latitude", 37.5}, {"longitude", 137.3}, {"magnitude", 6.2}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(jmaEvent.has_value());
        QCOMPARE(jmaEvent->reportTime,
                 QDateTime::fromString(QStringLiteral("2024-01-01T07:12:00Z"), Qt::ISODate)
                     .toMSecsSinceEpoch());
    }

    // Whews 用 cancel / final（不带 is 前缀），与 Wolfx 的 isCancel / isFinal 不同。
    void whewsHonoursCancelAndFinalFlags() {
        const auto* channel = whewsChannelFor(QStringLiteral("jma_eew"));
        QVERIFY(channel);
        const auto ongoing = WhewsParser::parseRecord(
            *channel,
            QJsonObject{{"id", QStringLiteral("A")}, {"updates", 3},
                        {"shockTime", QStringLiteral("2024-01-01 16:10:08")},
                        {"latitude", 37.5}, {"longitude", 137.3}, {"magnitude", 6.2},
                        {"cancel", false}, {"final", false}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(ongoing->isFinal == false);
        QVERIFY(ongoing->isCanceled == false);
        const auto done = WhewsParser::parseRecord(
            *channel,
            QJsonObject{{"id", QStringLiteral("B")}, {"updates", 9},
                        {"shockTime", QStringLiteral("2024-01-01 16:10:08")},
                        {"latitude", 37.5}, {"longitude", 137.3}, {"magnitude", 6.2},
                        {"cancel", true}, {"final", true}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(done->isFinal);
        QVERIFY(done->isCanceled);
    }

    // cenc 用上游 id 原文（无频道前缀），以便与 Wolfx cenc_eqlist 落进同一合并键。
    void whewsCencUsesUpstreamIdVerbatimForCrossSourceMerge() {
        const auto* cenc = whewsChannelFor(QStringLiteral("cenc"));
        QVERIFY(cenc->eventNs.isEmpty());
        const auto event = WhewsParser::parseRecord(
            *cenc,
            QJsonObject{{"id", QStringLiteral("CD.20260813084717.000_M")},
                        {"shockTime", QStringLiteral("2026-08-13 08:47:00")},
                        {"latitude", 36.06}, {"longitude", 103.55}, {"magnitude", 3.2}},
            std::nullopt, IntensityStandard::Csis);
        QVERIFY(event.has_value());
        // 去掉 _M 后缀即与 Wolfx 的 EventID 对齐。
        QCOMPARE(event->eventId, std::string("CD.20260813084717.000"));
        QCOMPARE(event->identity(), std::string("CENC|CD.20260813084717.000"));

        // EEW 频道带 eventNs 前缀，与 Wolfx / Pancakes 同名频道对齐。
        const auto* cea = whewsChannelFor(QStringLiteral("cea"));
        const auto ceaEvent = WhewsParser::parseRecord(
            *cea,
            QJsonObject{{"id", QStringLiteral("bi9wyea65mayd")}, {"updates", 3},
                        {"shockTime", QStringLiteral("2026-08-13 08:47:00")},
                        {"latitude", 29.43}, {"longitude", 101.09}, {"depth", 8},
                        {"magnitude", 4.0}, {"epiIntensity", 5.5}},
            std::nullopt, IntensityStandard::Csis);
        QCOMPARE(ceaEvent->eventId, std::string("cenc_eew:bi9wyea65mayd"));
        QCOMPARE(ceaEvent->identity(), std::string("CEA|cenc_eew:bi9wyea65mayd"));
        QCOMPARE(ceaEvent->sourceProvider, std::string("Whews"));
    }

    // 文档明示这些端点不进入 /ws/all，不应被当作已接入频道。
    void whewsIgnoresEndpointsExcludedFromAggregate() {
        QSet<QString> seen;
        QVERIFY(!whewsChannelFor(QStringLiteral("cenc_int")));
        QVERIFY(!whewsChannelFor(QStringLiteral("cmt_usgs")));
        QVERIFY(!whewsChannelFor(QStringLiteral("nied")));
        QVERIFY(!whewsChannelFor(QStringLiteral("weatheralarm")));
        QCOMPARE(whewsChannelFor(QStringLiteral("jma_eew"))->kind, SourceEventKind::Live);
        QCOMPARE(whewsChannelFor(QStringLiteral("cenc"))->kind, SourceEventKind::Directory);
        // 频道 id 唯一，避免同一 source 短名重复登记导致先命中者生效。
        for (const auto& channel : whewsChannels()) {
            QVERIFY2(!seen.contains(channel.source), qPrintable(channel.source));
            seen.insert(channel.source);
        }
    }

    // 无令牌 → 不建立任何连接（urlIndex 保持初始值，不会前进）。
    void whewsUnconfiguredSourceNeverConnects() {
        WhewsSource source;
        QVERIFY(!source.isConfigured());
        source.start();
        QVERIFY(source.socket_ == nullptr);
        QCOMPARE(source.urlIndex_, 0);
        QVERIFY(!source.reconnectTimer_.isActive());
        QCOMPARE(source.info().status, ConnectionStatus::Disconnected);
        source.stop();
    }

    // 首连快照为 JSON 数组：逐条解析并按频道区分 LIVE / DIRECTORY。
    void whewsSnapshotArrayIsParsedPerChannel() {
        WhewsSource source;
        source.setNowProvider([] { return origin + 2000; });
        source.running_ = true;
        source.socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
        QSignalSpy received(&source, &WhewsSource::eventReceived);
        QVERIFY(received.isValid());
        // 发震时刻取在 origin 附近的新鲜时刻，否则会被 30 min 新鲜度窗口当作回放丢弃。
        const auto wallClock = [](qint64 epochMs, int offsetHours) {
            return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone(offsetHours * 3600))
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        };
        const QString freshCenc = wallClock(origin, 8);
        const QString freshJma = wallClock(origin, 9);
        QJsonObject cencData;
        cencData.insert(QStringLiteral("id"), QStringLiteral("CD.20260813084717.000"));
        cencData.insert(QStringLiteral("shockTime"), freshCenc);
        cencData.insert(QStringLiteral("latitude"), 36.06);
        cencData.insert(QStringLiteral("longitude"), 103.55);
        cencData.insert(QStringLiteral("magnitude"), 3.2);
        QJsonObject jmaData;
        jmaData.insert(QStringLiteral("id"), QStringLiteral("20240101161010"));
        jmaData.insert(QStringLiteral("shockTime"), freshJma);
        jmaData.insert(QStringLiteral("latitude"), 37.5);
        jmaData.insert(QStringLiteral("longitude"), 137.3);
        jmaData.insert(QStringLiteral("magnitude"), 6.2);
        QJsonObject cencFrame;
        cencFrame.insert(QStringLiteral("source"), QStringLiteral("cenc"));
        cencFrame.insert(QStringLiteral("md5"), QStringLiteral("a"));
        cencFrame.insert(QStringLiteral("Data"), cencData);
        QJsonObject jmaFrame;
        jmaFrame.insert(QStringLiteral("source"), QStringLiteral("jma_eew"));
        jmaFrame.insert(QStringLiteral("md5"), QStringLiteral("b"));
        jmaFrame.insert(QStringLiteral("Data"), jmaData);
        const QJsonArray snapshot{cencFrame, jmaFrame};
        source.handleMessage(QString::fromUtf8(QJsonDocument(snapshot).toJson()));
        QCOMPARE(received.count(), 2);
        const auto cenc = qvariant_cast<EarthquakeEvent>(received.at(0).at(0));
        QCOMPARE(qvariant_cast<SourceEventKind>(received.at(0).at(1)), SourceEventKind::Directory);
        QCOMPARE(cenc.sourceAgency, std::string("CENC"));
        const auto jma = qvariant_cast<EarthquakeEvent>(received.at(1).at(0));
        QCOMPARE(qvariant_cast<SourceEventKind>(received.at(1).at(1)), SourceEventKind::Live);
        QCOMPARE(jma.sourceAgency, std::string("JMA"));
        source.stop();
    }

    // 过期的 EEW 回放不得当作实时预警（上游首连会补发「最近一次」预警）。
    void whewsDropsStaleLiveEewButKeepsDirectory() {
        WhewsSource source;
        const long long now = origin;
        source.setNowProvider([now] { return now; });
        source.running_ = true;
        source.socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
        QSignalSpy received(&source, &WhewsSource::eventReceived);
        // 发震时刻在 30 分钟新鲜度窗口之外（origin 为 2027-01，本次固定取 2024-01）。
        QJsonObject data;
        data.insert(QStringLiteral("id"), QStringLiteral("old"));
        data.insert(QStringLiteral("shockTime"), QStringLiteral("2024-01-01 16:10:08"));
        data.insert(QStringLiteral("latitude"), 37.5);
        data.insert(QStringLiteral("longitude"), 137.3);
        data.insert(QStringLiteral("magnitude"), 6.2);
        QJsonObject eew;
        eew.insert(QStringLiteral("source"), QStringLiteral("jma_eew"));
        eew.insert(QStringLiteral("Data"), data);
        source.handleMessage(QString::fromUtf8(QJsonDocument(eew).toJson()));
        QCOMPARE(received.count(), 0);
        // 同一时刻的情报（目录）仍应保留：只有 EEW 受新鲜度窗口约束。
        QJsonObject info;
        info.insert(QStringLiteral("source"), QStringLiteral("jma"));
        info.insert(QStringLiteral("Data"), data);
        source.handleMessage(QString::fromUtf8(QJsonDocument(info).toJson()));
        QCOMPARE(received.count(), 1);
        QCOMPARE(qvariant_cast<SourceEventKind>(received.at(0).at(1)), SourceEventKind::Directory);
        source.stop();
    }

    // 传输失败才前进站点索引：先国内站，失败后切主站。
    void whewsRotatesSiteOnlyOnTransportFailure() {
        WhewsSource source;
        source.running_ = true;
        source.generation_ = 1;
        QCOMPARE(source.urlIndex_, 0);

        // 正常断开（服务端主动关闭）不轮换：仍优先国内站。
        {
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->disconnected();
            QCOMPARE(source.urlIndex_, 0);
            source.reconnectTimer_.stop();
        }

        // 传输失败才 ++urlIndex_，下一次 connectSocket 指向主站。
        {
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
            QCOMPARE(source.urlIndex_, 1);
            QVERIFY(source.reconnectTimer_.isActive());
            source.reconnectTimer_.stop();
        }

        // 与 Wolfx 同款保证：errorOccurred + disconnected 同时到达只调度一次重连。
        {
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            const int before = source.retryCount_;
            socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
            socket->disconnected();   // 已被 errorOccurred 退休，不会再调度一次
            QCOMPARE(source.retryCount_, before + 1);
            QVERIFY(source.reconnectTimer_.isActive());
        }
        source.stop();
        QVERIFY(!source.reconnectTimer_.isActive());
    }

    // 4401/4403 为服务端明确拒绝：按文档停止重连，否则重连过频会被智能封禁。
    // 这两个码不在 Qt 的 CloseCode 枚举内，无法经 Qt API 注入，故直接验证判定谓词；
    // 「停止重连」分支的行为由 isFatalClose 为真时不再调度重连保证。
    void whewsAuthRejectionIsFatal() {
        QVERIFY(WhewsSource::isFatalClose(
            static_cast<QWebSocketProtocol::CloseCode>(WhewsProtocol::CloseCode::kUnauthorized)));
        QVERIFY(WhewsSource::isFatalClose(
            static_cast<QWebSocketProtocol::CloseCode>(WhewsProtocol::CloseCode::kBanned)));
        // 正常关闭与其它错误码不得误判为鉴权失败。
        QVERIFY(!WhewsSource::isFatalClose(QWebSocketProtocol::CloseCodeNormal));
        QVERIFY(!WhewsSource::isFatalClose(QWebSocketProtocol::CloseCodeGoingAway));
        QVERIFY(!WhewsSource::isFatalClose(QWebSocketProtocol::CloseCodeAbnormalDisconnection));
        QVERIFY(!WhewsSource::isFatalClose(static_cast<QWebSocketProtocol::CloseCode>(4503)));
    }

    // 正常关闭（非致命码）应退避重连，且不前进站点索引（仍优先国内站）。
    void whewsNormalCloseSchedulesReconnectOnSameSite() {
        WhewsSource source;
        source.running_ = true;
        source.generation_ = 1;
        auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
        source.socket_ = socket;
        source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
        socket->disconnected();
        QCOMPARE(source.info().status, ConnectionStatus::Disconnected);
        QVERIFY(!source.info().description.contains(QString::fromUtf8("鉴权")));
        QVERIFY(source.reconnectTimer_.isActive());
        QCOMPARE(source.urlIndex_, 0);
        QCOMPARE(source.retryCount_, 1);
        source.stop();
        QVERIFY(!source.reconnectTimer_.isActive());
    }

    // ── 模拟源（sim-eew/1）──────────────────────────────────────────

    static QJsonObject simFrame(const QString& eventId = QStringLiteral("sim-1-a"),
                                int reportNum = 1, long long originMs = origin,
                                const QString& type = QStringLiteral("report"),
                                bool isFinal = false) {
        QJsonObject f;
        f.insert(QStringLiteral("type"), type);
        f.insert(QStringLiteral("eventId"), eventId);
        f.insert(QStringLiteral("reportNum"), reportNum);
        // 契约要求 epoch 毫秒数字，不能是墙钟串。
        f.insert(QStringLiteral("originTime"), static_cast<double>(originMs));
        f.insert(QStringLiteral("magnitude"), 6.5);
        f.insert(QStringLiteral("latitude"), 20.0);
        f.insert(QStringLiteral("longitude"), 160.0);
        f.insert(QStringLiteral("depth"), 12.0);
        f.insert(QStringLiteral("location"), QStringLiteral("模拟震源"));
        f.insert(QStringLiteral("isFinal"), isFinal);
        return f;
    }

    /// 起一个已连接、时钟固定的模拟源。QSignalSpy 不可默认构造、也不可赋值，
    /// 故连同源一起返回，由调用方就地构造 spy。
    static std::unique_ptr<SimulatedSource> startedSimulated(long long nowMs) {
        auto source = std::make_unique<SimulatedSource>();
        source->setNowProvider([nowMs] { return nowMs; });
        source->setDevMode(true);
        source->setUrl(QStringLiteral("ws://127.0.0.1:8080/ws"));
        source->running_ = true;
        source->generation_ = 1;
        source->socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, source.get());
        return source;
    }

    // 门控：开发者模式 + 地址缺一不可。关闭时上层不启动，源自身也不连接。
    void simulatedIsConfiguredRequiresBothGates() {
        SimulatedSource source;
        QVERIFY(!source.isConfigured());                       // 两者皆空
        source.setDevMode(true);
        QVERIFY(!source.isConfigured());                       // 有模式无地址
        source.setUrl(QStringLiteral("  ws://127.0.0.1:8080/ws  "));
        QVERIFY(source.isConfigured());                        // 齐备
        QCOMPARE(source.url_, QStringLiteral("ws://127.0.0.1:8080/ws"));  // 读时去空白
        source.setDevMode(false);
        QVERIFY(!source.isConfigured());                       // 关模式即刻退回未配置
    }

    // 未配置时 connect() 不得开 socket：这是「开发者模式关 ⇒ 从不连接」的最后一道保证。
    void simulatedUnconfiguredNeverConnects() {
        SimulatedSource source;
        source.setNowProvider([] { return origin; });
        source.setDevMode(false);
        source.setUrl(QStringLiteral("ws://127.0.0.1:8080/ws"));
        source.start();
        QCOMPARE(source.info().status, ConnectionStatus::Disconnected);
        QVERIFY(!source.info().description.isEmpty());
        source.stop();
    }

    // 机构固定 SIM：与真实 CENC 报文不得落进同一合并键，否则告警被静默吞掉。
    void simulatedAgencyNeverCollidesWithRealSources() {
        auto source = startedSimulated(origin);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        source->handleMessage(QString::fromUtf8(QJsonDocument(simFrame()).toJson()));
        QCOMPARE(received.count(), 1);
        const auto event = qvariant_cast<EarthquakeEvent>(received.at(0).at(0));
        QCOMPARE(QString::fromStdString(event.sourceAgency), QStringLiteral("SIM"));
        QCOMPARE(QString::fromStdString(event.sourceProvider), QStringLiteral("Simulated"));
        QCOMPARE(QString::fromStdString(event.id), QStringLiteral("sim_sim-1-a"));
        QVERIFY(QString::fromStdString(event.eventId).startsWith(QStringLiteral("sim-")));

        // 形状相同的真实 CENC 报文：id 相同，但 identity 必须不同。
        auto cenc = event;
        cenc.id = "wolfx_CD.1";
        cenc.eventId = "CD.1";
        cenc.sourceAgency = "CENC";
        cenc.sourceProvider = "Wolfx";
        QVERIFY(cenc.identity() != event.identity());
        // 反向对照：同机构同 id 确实合并（证明差异来自机构而非巧合）。
        cenc.sourceProvider = "Pancakes";
        QCOMPARE(cenc.identity(), QString::fromStdString("CENC|CD.1").toStdString());

        // 两者同入一个 EventGate 必须并存，否则用户看不到第二次预警。
        EventGate gate;
        QCOMPARE(gate.admit(event, origin), EventGateDecision::Pass);
        QCOMPARE(gate.admit(cenc, origin), EventGateDecision::Pass);
        source->stop();
    }

    // 多报次递增、末报 final，各报共用一个 identity（EventGate 靠它归并）。
    void simulatedMultiReportIncrementsAndFinalizes() {
        auto source = startedSimulated(origin);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        const auto send = [&](int n, bool final) {
            source->handleMessage(
                QString::fromUtf8(QJsonDocument(simFrame(QStringLiteral("sim-1-a"), n, origin,
                                                      QStringLiteral("report"), final)).toJson()));
        };
        send(1, false);
        send(2, false);
        send(3, true);
        QCOMPARE(received.count(), 3);
        QList<int> nums, finals;
        QSet<std::string> identities;
        for (int i = 0; i < received.count(); ++i) {
            const auto e = qvariant_cast<EarthquakeEvent>(received.at(i).at(0));
            nums << e.reportNum;
            finals << int(e.isFinal);
            identities.insert(e.identity());
        }
        QCOMPARE(nums, (QList<int>{1, 2, 3}));
        QCOMPARE(finals, (QList<int>{0, 0, 1}));
        QCOMPARE(identities.size(), size_t(1));
        source->stop();
    }

    // type 带内承载 Live/Directory：目录帧必须走 Directory，不得进入告警链路。
    void simulatedTypeMapsToEventKind() {
        auto source = startedSimulated(origin);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        source->handleMessage(QString::fromUtf8(QJsonDocument(simFrame()).toJson()));
        source->handleMessage(QString::fromUtf8(
            QJsonDocument(simFrame(QStringLiteral("sim-2-b"), 1, origin, QStringLiteral("directory"))).toJson()));
        QCOMPARE(received.count(), 2);
        QCOMPARE(qvariant_cast<SourceEventKind>(received.at(0).at(1)), SourceEventKind::Live);
        QCOMPARE(qvariant_cast<SourceEventKind>(received.at(1).at(1)), SourceEventKind::Directory);
        QCOMPARE(QString::fromStdString(qvariant_cast<EarthquakeEvent>(received.at(0).at(0)).source),
                 QStringLiteral("模拟数据源 地震预警"));
        QCOMPARE(QString::fromStdString(qvariant_cast<EarthquakeEvent>(received.at(1).at(0)).source),
                 QStringLiteral("模拟数据源 地震情报"));
        source->stop();
    }

    // 超 30 分钟活跃窗口的实时帧丢弃；同一时刻的目录帧仍保留（只有 Live 受限）。
    void simulatedDropsStaleLiveButKeepsDirectory() {
        const long long now = origin;
        const long long old = origin - 31LL * 60 * 1000;
        auto source = startedSimulated(now);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        source->handleMessage(QString::fromUtf8(
            QJsonDocument(simFrame(QStringLiteral("sim-old"), 1, old)).toJson()));
        QCOMPARE(received.count(), 0);
        source->handleMessage(QString::fromUtf8(
            QJsonDocument(simFrame(QStringLiteral("sim-old"), 1, old, QStringLiteral("directory"))).toJson()));
        QCOMPARE(received.count(), 1);
        source->stop();
    }

    // 未来时刻的帧不得进入链路：它一出生就判过期（与 Android EventLifecycle 同规则）。
    void simulatedDropsFutureOrigin() {
        const long long now = origin;
        auto source = startedSimulated(now);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        source->handleMessage(QString::fromUtf8(
            QJsonDocument(simFrame(QStringLiteral("sim-future"), 1, now + 5 * 60 * 1000)).toJson()));
        QCOMPARE(received.count(), 0);
        // 60s 之内的未来时刻仍放行（服务端钳到 +55s 就是为了留这个余量）。
        source->handleMessage(QString::fromUtf8(
            QJsonDocument(simFrame(QStringLiteral("sim-soon"), 1, now + 55 * 1000)).toJson()));
        QCOMPARE(received.count(), 1);
        source->stop();
    }

    // expired() 的未来时刻守卫：与 Android 端 EventLifecycle 对齐，本次补齐。
    // 注意 expired(nowMs) 的入参是「当前时刻」，不是发震时刻：未来时刻的判据是
    // 「发震时刻比当前时刻超前 60s 以上」，故用发震时刻在前的场景来构造。
    void futureOriginIsExpired() {
        const long long now = origin;
        // 发震时刻恰为当下：不是未来；未知到时给 5 分钟窗口，故仍有效。
        auto a = makeEvent();
        QVERIFY(!a.expired(now));
        // 发震时刻超前 60s 之内的仍在窗口内。
        QVERIFY(!a.expired(a.timestamp - 60'000));
        // 发震时刻超前 60s 以上：判过期（本次补齐的守卫，Android 端同规则）。
        QVERIFY(a.expired(a.timestamp - 60'001));
    }

    // hello / ping 是控制帧：只刷新心跳，不产生事件。
    void simulatedControlFramesProduceNoEvents() {
        auto source = startedSimulated(origin);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        source->handleMessage(QStringLiteral(R"({"type":"hello","server":"sim-eew/1"})"));
        source->handleMessage(QStringLiteral(R"({"type":"ping","epoch":1})"));
        QCOMPARE(received.count(), 0);
        source->stop();
    }

    // 畸形帧忽略而非致命：连接必须保持，一条坏帧不该触发断线重连。
    // 截断帧用普通字符串而非 R"()"：裸串里的 )" 会提前终止原始字符串，
    // moc 的简易解析器随之错位，整个测试类的 vtable 就丢了（链接期才报）。
    void simulatedMalformedFrameKeepsSocket() {
        auto source = startedSimulated(origin);
        QSignalSpy received(source.get(), &SimulatedSource::eventReceived);
        auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, source.get());
        source->socket_ = socket;
        source->attachSocketHandlers(socket, /*generation*/ 1, source->attempt_);
        // 桩 socket 不会真的握手成功，故直接置为已连接：这里要验的是
        // 「坏帧不会把连接打掉」，不是握手流程。
        source->setStatus(ConnectionStatus::Connected);
        source->handleMessage(QStringLiteral("{\"type\":\"report\",\"eventId\":"));  // 截断
        source->handleMessage(QStringLiteral("{\"type\":\"report\"}"));             // 缺必填
        source->handleMessage(QString::fromUtf8(QJsonDocument(simFrame()).toJson()));
        QCOMPARE(received.count(), 1);
        QCOMPARE(source->info().status, ConnectionStatus::Connected);
        source->stop();
    }

    // 烈度文本原文透传：JMA 式写法经 parseMaxIntensity 会被重新格式化而破坏。
    void simulatedPreservesIntensityTextVerbatim() {
        auto frame = simFrame();
        frame.insert(QStringLiteral("maxIntensity"), 5.0);
        frame.insert(QStringLiteral("maxIntensityText"), QStringLiteral("5弱"));
        const auto event = SimulatedParser::parseReport(frame, SourceEventKind::Live, std::nullopt,
                                                        IntensityStandard::Csis, origin);
        QVERIFY(event.has_value());
        QCOMPARE(QString::fromStdString(event->maxIntensityText), QStringLiteral("5弱"));
        QCOMPARE(event->maxIntensityRaw, 5.0);
    }

    // 必填缺失一律拒绝：0/0 会被当成几内亚湾的合法坐标，故不能用默认值蒙混。
    void simulatedParserRejectsMissingRequiredFields() {
        QJsonObject noEventId = simFrame();
        noEventId.remove(QStringLiteral("eventId"));
        QVERIFY(!SimulatedParser::parseReport(noEventId, SourceEventKind::Live, std::nullopt,
                                              IntensityStandard::Csis, origin).has_value());
        QJsonObject noOrigin = simFrame();
        noOrigin.remove(QStringLiteral("originTime"));
        QVERIFY(!SimulatedParser::parseReport(noOrigin, SourceEventKind::Live, std::nullopt,
                                              IntensityStandard::Csis, origin).has_value());
        QJsonObject noEpicenter = simFrame();
        noEpicenter.remove(QStringLiteral("latitude"));
        QVERIFY(!SimulatedParser::parseReport(noEpicenter, SourceEventKind::Live, std::nullopt,
                                              IntensityStandard::Csis, origin).has_value());
    }

    // reportTime 必须置为收帧时刻：目录去重以它决胜，缺省会令条目任意胜出。
    void simulatedParserStampsReportTime() {
        const long long now = origin + 1234;
        const auto event = SimulatedParser::parseReport(simFrame(), SourceEventKind::Directory,
                                                        std::nullopt, IntensityStandard::Csis, now);
        QVERIFY(event.has_value());
        QCOMPARE(event->reportTime, now);
    }

    // 传输失败时 errorOccurred 与 disconnected 都会触发：只许调度一次重连。
    // 与 Wolfx / Whews 同款保证，是本仓库反复踩过的坑。
    void simulatedSchedulesReconnectOnlyOnce() {
        auto source = startedSimulated(origin);
        auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, source.get());
        source->socket_ = socket;
        source->attachSocketHandlers(socket, /*generation*/ 1, ++source->attempt_);
        const int before = source->retryCount_;
        socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
        socket->disconnected();   // 已被 errorOccurred 退休，不会再调度一次
        QCOMPARE(source->retryCount_, before + 1);
        QVERIFY(source->reconnectTimer_.isActive());
        source->stop();
        QVERIFY(!source->reconnectTimer_.isActive());
    }

    // 设置项语义：开发者模式与地址是持久化的，且模拟源刻意不在默认禁用集内
    //（门控交给 isConfigured，否则会形成「开关+地址+禁用集」三重门）。
    void simulatedSettingsArePersistedAndNotDisabledByDefault() {
        QSettings().clear();   // 套件已把 QSettings 重定向到临时目录
        {
            SettingsStore store;
            QVERIFY(!store.developerMode());                 // 默认关闭
            QVERIFY(store.simulatedUrl().isEmpty());         // 默认空：不预填
            store.setDeveloperMode(true);
            store.setSimulatedUrl(QStringLiteral("  ws://10.0.2.2:8080/ws  "));
            QVERIFY(store.developerMode());
            QCOMPARE(store.simulatedUrl(), QStringLiteral("ws://10.0.2.2:8080/ws"));
            // 模拟源刻意不在默认禁用集内：门控交给 isConfigured，否则形成三重门。
            QVERIFY(!SettingsStore::defaultDisabledSources().contains(SourceIds::kSimulated));
            QVERIFY(store.isSourceEnabled(SourceIds::kSimulated));
        }
        SettingsStore reloaded;   // 新实例：验证真的落了盘
        QVERIFY(reloaded.developerMode());
        QCOMPARE(reloaded.simulatedUrl(), QStringLiteral("ws://10.0.2.2:8080/ws"));
        QSettings().clear();
    }

    /// 侧栏筛选全部是**展示层**条件：切筛选立即重算，不改源行。
    /// 「近 24h」必须双向判定 —— 未来时间戳不能混进来（否则会永远钉在最前面）。
    void eventFilterAppliesTimeWindowBothDirections() {
        EventListModel model;
        EventFilterModel filter;
        filter.setSourceModel(&model);

        const long long now = 1'800'000'000'000LL;
        filter.setNowProvider([now] { return now; });

        const auto row = [](const QString& id, long long timestamp, double magnitude,
                            double distance) {
            EventListModel::Row r;
            r.id = id;
            r.timestamp = timestamp;
            r.magnitude = magnitude;
            r.source = QStringLiteral("Wolfx 地震信息");
            r.sourceTag = QStringLiteral("Wolfx·CENC");
            r.sourceTags = QStringList{r.sourceTag};
            r.location = QStringLiteral("四川");
            if (distance >= 0.0) {
                r.hasDistance = true;
                r.distance = distance;
            }
            return r;
        };
        constexpr long long kHour = 3600LL * 1000LL;
        model.setRows({row(QStringLiteral("old"), now - 30 * kHour, 6.0, 100.0),
                       row(QStringLiteral("fresh"), now - 2 * kHour, 3.0, 900.0),
                       row(QStringLiteral("near"), now - kHour, 4.5, 120.0),
                       row(QStringLiteral("future"), now + 10 * 60'000, 7.0, 50.0)});

        filter.setFilter(QStringLiteral("RECENT_24H"), QString());
        // future 是时钟漂移/源侧错误：不属于「近 24h」；old 已过期。
        QCOMPARE(filter.rowCount(), 2);

        filter.setFilter(QStringLiteral("WITHIN_500"), QString());
        // fresh 距离 900 km 被挡；old / near / future 都在 500 km 内。
        QCOMPARE(filter.rowCount(), 3);

        filter.setFilter(QStringLiteral("M4_PLUS"), QString());
        QCOMPARE(filter.rowCount(), 3);   // old + near + future，fresh 3.0 被挡

        filter.setFilter(QStringLiteral("ALL"), QString());
        QCOMPARE(filter.rowCount(), 4);

        filter.setFilter(QStringLiteral("ALL"), QStringLiteral("cenc"));
        QCOMPARE(filter.rowCount(), 4);   // 来源标签命中
    }

    // 五个事件源都必须真的测出链路延迟：`latencyMs` 是设置页「延迟」的唯一数据来源。
    //
    // 这条用例钉住的原缺陷：此前只有 Wolfx 调过 `socket_->ping()`，Whews / Simulated 连了
    // pong 却从不 ping（死代码），Jian / Pancakes 连 pong 都没接 —— `latencyMs` 恒为 -1，
    // 设置页的延迟对四个源永远不显示，用户看到的就是「延迟那栏一直是空的」。
    //
    // 用本机 QWebSocketServer 做对端（它按 RFC 6455 自动回 pong），因此不依赖外网、也不
    // 依赖真实上游是否支持控制帧。回环 RTT 常常量到 0 ms，所以断言「测到了」（>= 0）
    // 而不是一个正数 —— 0 ms 是合法测量值，与「没测过」（-1）是两件事。
    void everyEventSourceMeasuresLinkLatency() {
        QWebSocketServer server(QStringLiteral("komira-latency-probe-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&server]() {
            while (server.hasPendingConnections()) server.nextPendingConnection();
        });
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));

        // 每个源都按它真实的接线方式挂上回调，再接一条本机链路。
        // 注意 `generation_` 要与传进 attachSocketHandlers 的一致：`current()` 守卫三个条件
        // （running / generation / attempt），任何一项对不上，探针的 pong 回调就会被丢弃，
        // 表现成「连上了但测不到延迟」——这本身也值得被这条用例覆盖。
        // Wolfx
        {
            WolfxSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            QSignalSpy changed(&source, &WolfxSource::infoChanged);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            // 只有真的发过 ping 才会拿到 pong，因此 infoChanged 必须被触发过。
            QVERIFY(changed.count() > 0);
            source.stop();
        }
        // Pancakes（此前完全没有 pong 处理）
        {
            PancakesSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            source.stop();
        }
        // Jian（此前完全没有 pong 处理）
        {
            JianSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            source.stop();
        }
        // Whews（此前 pong 处理器是死代码）
        {
            WhewsSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            source.stop();
        }
        // Simulated（同上）
        {
            SimulatedSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            source.stop();
        }

        // 断开后不得留着旧值冒充当前延迟：setStatus 在非 Connected 时把 latencyMs 置 -1，
        // 设置页据此把「延迟」整行收起（而不是继续显示几秒前的数）。
        {
            WhewsSource source;
            source.running_ = true;
            source.generation_ = 1;
            auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, &source);
            source.socket_ = socket;
            source.attachSocketHandlers(socket, /*generation*/ 1, ++source.attempt_);
            socket->open(url);
            QTRY_VERIFY_WITH_TIMEOUT(source.info().latencyMs >= 0, 5000);
            source.stop();
            QCOMPARE(source.info().latencyMs, qint64(-1));
        }
    }

    void sentryServiceDsnAndBreadcrumbs() {
        SentryService::initializeEarly();
        auto* sentry = SentryService::instance();
        QVERIFY(sentry != nullptr);
        QVERIFY(sentry->dsn().contains("48fb324081d84266d8f3f961022bb237"));
        QVERIFY(sentry->dsn().contains("4512246266789888"));

        QString handlerPath = QCoreApplication::applicationDirPath() + QStringLiteral("/crashpad_handler");
#ifdef Q_OS_WIN
        handlerPath += QStringLiteral(".exe");
#endif
        QVERIFY2(QFileInfo::exists(handlerPath), qPrintable(handlerPath));

        sentry->init();
        QVERIFY(sentry->isEnabled()); // SDK 与 Crashpad handler 均已初始化

        sentry->addBreadcrumb("test", "test message 1", "info");
        sentry->addBreadcrumb("test", "test message 2", "warning");

        sentry->recordCrashImmediately("TestSignal", "SIGSEGV (Test crash)", "frame1\nframe2");
        sentry->checkAndSendPendingCrashes();
    }
};

QTEST_GUILESS_MAIN(QtBusinessTest)
#include "qt_business_test.moc"
