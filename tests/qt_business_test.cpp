#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>
#include <cmath>
#include <functional>

#include "core/ip_geo_lookup.h"
#include "core/travel_time_service.h"
#include "core/warning_session.h"
#include "model/data_source_info.h"
#include "prefs/settings_store.h"
#include "source/eew_parser.h"
#include "store/history_store.h"

// No production clock/network seam exists for message injection. Restrict access
// widening to this header; all of its Qt/STL dependencies are included above.
#define private public
#include "source/wolfx_source.h"
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
        qRegisterMetaType<WolfxEventKind>();
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
        settings.setEnabledWolfx(false);
        QCOMPARE(changed.count(), 1);
        settings.setEnabledWolfx(false);
        QCOMPARE(changed.count(), 1);
        settings.setIsMuted(true);
        settings.setIntensityStandard(1);
        QCOMPARE(changed.count(), 3);
        SettingsStore reloaded;
        QVERIFY(!reloaded.enabledWolfx());
        QVERIFY(reloaded.isMuted());
        QCOMPARE(reloaded.intensityStandard(), 1);
        settings.resetToDefaults();
        QCOMPARE(changed.count(), 4);
        QVERIFY(settings.enabledWolfx());
        QVERIFY(!settings.isMuted());
        QCOMPARE(settings.intensityStandard(), 0);
    }

    void themeDefaultsAndPersistence() {
        QSettings().clear();
        {
            SettingsStore settings;
            QVERIFY(!settings.darkMode());
            settings.setDarkMode(true);
        }
        {
            SettingsStore reloaded;
            QVERIFY(reloaded.darkMode());
            reloaded.setDarkMode(false);
        }
        SettingsStore settings;
        QVERIFY(!settings.darkMode());
        settings.setDarkMode(true);
        settings.resetToDefaults();
        QVERIFY(!settings.darkMode());
        QVERIFY(!QSettings().contains("darkMode"));
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
        QCOMPARE(emitted.eventId, std::string("A"));
        QCOMPARE(emitted.sourceProvider, std::string("Wolfx"));
        bool delivered = false;
        QMetaObject::invokeMethod(&source, [&] {
            source.handleMessage(text);
            source.refreshDirectory();
            delivered = true;
        }, Qt::QueuedConnection);
        SettingsStore settings;
        settings.setEnabledWolfx(true);
        connect(&settings, &SettingsStore::changed, &source, [&] {
            if (!settings.enabledWolfx()) source.stop();
        });
        settings.setEnabledWolfx(false);
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

    // QWebSocket 传输失败会同时发 errorOccurred 与 disconnected：一次连接尝试只应调度
    // 一次重连，否则 retryCount_ 被翻倍、退避瞬间顶到 15s 上限。
    void reconnectScheduledOncePerAttempt() {
        WolfxSource source;
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
};

QTEST_GUILESS_MAIN(QtBusinessTest)
#include "qt_business_test.moc"
