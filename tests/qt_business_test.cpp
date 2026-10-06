#include <QtTest>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QSet>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>
#include <cmath>
#include <functional>

#include "core/ip_geo_lookup.h"
#include "core/quake_calculator.h"
#include "core/travel_time_service.h"
#include "core/version_compare.h"
#include "core/warning_session.h"
#include "model/data_source_info.h"
#include "prefs/settings_store.h"
#include "source/eew_parser.h"
#include "source/jian_parser.h"
#include "source/pancakes_parser.h"
#include "source/pancakes_protocol.h"
#include "source/simulated_parser.h"
#include "source/simulated_protocol.h"
#include "source/whews_parser.h"
#include "source/whews_protocol.h"
#include "store/history_store.h"

// No production clock/network seam exists for message injection. Restrict access
// widening to these headers; all of their Qt/STL dependencies are included above.
// 合成一段而非多段：moc 处理本文件时也会看到这些宏，分段会让 vtable 声明失配。
#define private public
#include "source/wolfx_source.h"
#include "source/whews_source.h"
#include "source/simulated_source.h"
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
        }
        {
            auto parsed = PancakesParser::parseRealtime(
                envelope("gq", "cancelled", {{"id", "abc"}}),
                std::nullopt, IntensityStandard::Csis, origin);
            QVERIFY(parsed.has_value());
            QVERIFY(parsed->event.isCanceled);
            QCOMPARE(parsed->event.reportNum, PancakesProtocol::kCancelReportNum);
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
};

QTEST_GUILESS_MAIN(QtBusinessTest)
#include "qt_business_test.moc"
