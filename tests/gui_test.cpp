#include <QtTest>
#include <QImage>
#include <QSet>
#include <QHash>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSettings>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <functional>
#include <cmath>
#include "core/intensity_calculator.h"
#include "core/quake_calculator.h"
#include "core/source_estimator.h"
#include "core/warning_session.h"
#include "prefs/settings_store.h"
#include "theme/native_ui.h"
#include <QQuickStyle>
#include <QSGRendererInterface>
#include <QQmlEngine>
#include <QWebSocketServer>
#include "map/vector_map_item.h"
#include "map/station_map_item.h"
#include "model/station_snapshot.h"
#include "store/history_store.h"
// wolfx_source.h 进 #define private public 段：延迟用例要按真实接线方式挂 socket 回调
// （running_ / generation_ / attempt_ 与 attachSocketHandlers），这是头文件里唯一的
// 放宽访问范围之处，其余生产头一律不改。
#define private public
#include "app/app_controller.h"
#include "service/source_estimation_service.h"
#include "service/alert_announcer.h"
#include "service/tray_controller.h"
#include "source/wolfx_source.h"
#undef private

using namespace komira;

// Repeater delegates and popup content belong to the visual tree, not necessarily
// the window's QObject tree. Hit-test the same items the user can see.
static QList<QQuickItem*> visualItems(QQuickItem* root) {
    QList<QQuickItem*> items{root};
    for (qsizetype index = 0; index < items.size(); ++index)
        items.append(items[index]->childItems());
    return items;
}

static QQuickItem* findVisualItem(QQuickWindow* window, const QString& name) {
    for (auto* item : visualItems(window->contentItem()))
        if (item->objectName() == name) return item;
    return nullptr;
}

// 波前圆的「蓝色命中率」：沿圆右半侧（避开左侧 HUD / 右侧工具条）在半径 ±3 px
// 内找 P 波蓝，返回有蓝的采样格占比。实线接近 1；虚线会出现成片空缺。
// 只断言 ShapePath 的属性抓不到「strokeStyle 写错退化成实线」，必须看像素。
static double blueRingHitRatio(QQuickWindow* window, const QPointF& sceneCenter, double radiusPx) {
    const QImage img = window->grabWindow();
    if (img.isNull() || radiusPx <= 0.0 || window->width() <= 0) return -1.0;
    const double scale = static_cast<double>(img.width()) / window->width();
    const auto isWaveBlue = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return false;
        const QColor c = img.pixelColor(x, y);
        return c.blue() > 140 && c.blue() > c.red() + 25 && c.blue() > c.green() + 5;
    };
    int bins = 0;
    int hits = 0;
    for (double angle = -0.95; angle <= 0.95; angle += 0.005) {
        ++bins;
        bool hit = false;
        for (int dr = -3; dr <= 3 && !hit; ++dr) {
            const int x = qRound((sceneCenter.x() + std::cos(angle) * (radiusPx + dr)) * scale);
            const int y = qRound((sceneCenter.y() + std::sin(angle) * (radiusPx + dr)) * scale);
            hit = isWaveBlue(x, y);
        }
        if (hit) ++hits;
    }
    return bins > 0 ? static_cast<double>(hits) / bins : -1.0;
}

// 按 objectName 点击可见控件（命中判定与用户视角一致：滚动裁剪之外的点不点）。
static bool clickByName(QQuickWindow* window, const QString& name) {
    auto* item = findVisualItem(window, name);
    if (!item || !item->isVisible() || !item->isEnabled()) return false;
    if (item->metaObject()->indexOfSignal("clicked()") < 0) return false;
    const QPointF point = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    const QRectF bounds(0, 0, window->width(), window->height());
    if (!bounds.contains(point)) return false;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
    return true;
}

// 发震时刻必须写全：Text 被 elide 时 contentWidth 会小于 implicitWidth。
// 返回是否找到时刻文本；若被截断则写入 offender 并返回 false。
static bool timesFullyShown(QQuickItem* root, QString* offender) {
    bool saw = false;
    for (auto* item : visualItems(root)) {
        const QString text = item->property("text").toString();
        if (!text.contains("UTC+8")) continue;
        saw = true;
        if (item->property("contentWidth").toDouble() < item->implicitWidth() - 0.5) {
            if (offender) *offender = text;
            return false;
        }
    }
    return saw;
}

static QString evidenceDirectory(QQuickWindow* window) {
    const bool software = window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    const QString backend = software ? "software"
        : qEnvironmentVariable("LIBGL_ALWAYS_SOFTWARE") == "1" ? "rhi-cpu" : "gpu";
    const QString directory = QCoreApplication::applicationDirPath() + "/gui-evidence/" + backend;
    QDir().mkpath(directory);
    return directory;
}

class GuiTest : public QObject {
    Q_OBJECT
private:
    /// 主程序用 qt_add_qml_module 自动注册 QML_ELEMENT 类型；gui_test 走 qt_add_resources +
    /// 直接按 qrc URL 加载 QML，拿不到那段注册代码，故在此显式注册一次。
    /// MapView.qml 有 `import KomiraQuake`，因此注册到该 URI 即可见。
    static void registerQmlTypes() {
        static bool done = false;
        if (done) return;
        done = true;
        qmlRegisterType<komira::VectorMapItem>("KomiraQuake", 1, 0, "VectorMapItem");
        qmlRegisterType<komira::StationMapItem>("KomiraQuake", 1, 0, "StationMapItem");
    }

private slots:
    void initTestCase() {
        registerQmlTypes();
        // qrc 资源没有可靠的 mtime，Qt 的 QML 磁盘缓存会跨重建复用旧字节码，
        // 让本用例静默地跑在旧 QML 上。测试必须始终执行本次构建的 QML。
        qputenv("QML_DISABLE_DISK_CACHE", "1");
    }

    /// 字体必须来自 qrc，而不是系统 fontconfig：换到没装这些字体的机器上也要一致。
    void bundledFontsAreEmbedded() {
        QQmlEngine engine;
        configureNativeUi(engine);
        QCOMPARE(QFontInfo(QFont(QStringLiteral("Source Han Sans SC"))).family(),
                 QStringLiteral("Source Han Sans SC"));
        QCOMPARE(QFontInfo(QFont(QStringLiteral("JetBrains Mono"))).family(),
                 QStringLiteral("JetBrains Mono"));
        QCOMPARE(QFontInfo(QFont(QStringLiteral("Google Sans"))).family(),
                 QStringLiteral("Google Sans"));
    }
    void controllerAndWindow() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("GuiTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(2.0);
        EarthquakeEvent e;
        e.id = e.eventId = "GUI-A";
        // 使用较长的「提供方·机构」标注（Pancakes·USGS），复现来源挤压发震时刻的排版问题。
        e.sourceProvider = "Pancakes"; e.sourceAgency = "USGS"; e.source = "GlobalQuake地震信息";
        e.timestamp = now; e.latitude = 30.6; e.longitude = 104;
        e.magnitude = 3; e.location = "演练事件（仅测试）";
        e.distanceKm = 100; e.rawIntensity = 1.0;
        QVERIFY(!controller.announcer_->eligible(e)); // 烈度低于过滤阈值，被拦截
        e.rawIntensity = 2.5;
        QVERIFY(controller.announcer_->eligible(e)); // 烈度达到过滤阈值，通过
        // 阈值按显示烈度（取整）比较：raw 2.6 显示为Ⅲ度，阈值 3.0 应放行，
        // 否则 HUD 显示Ⅲ度却收不到提醒。raw 2.4 显示为Ⅱ度，仍拦截。
        controller.settings()->setLocalIntensityFilter(3.0);
        e.rawIntensity = 2.6;
        QVERIFY(controller.announcer_->eligible(e));
        e.rawIntensity = 2.4;
        QVERIFY(!controller.announcer_->eligible(e));
        // 过滤跟随设置页所选显示标准。M5.0/30km 实测：CSIS 5 级 vs JMA 1 级
        // （JMA s=1.0986→1 级；CSIS raw=4.6268→5 级）。阈值 5.0 时，
        // CSIS 放行、JMA 拦截——若不跟随标准就会拿 CSIS 阈值去比震度而误判。
        e.magnitude = 5.0; e.distanceKm = 30; e.depth = 10; e.rawIntensity = 4.6268;
        controller.settings()->setLocalIntensityFilter(5.0);
        controller.settings()->setIntensityStandard(0);
        QVERIFY(controller.announcer_->eligible(e));
        controller.settings()->setIntensityStandard(1);
        QVERIFY(!controller.announcer_->eligible(e));
        // JMA「及以上」：1 级在阈值 1.0 下放行。5弱/5强 同为 5 级（M6.0/10km、M6.0/5km）。
        controller.settings()->setLocalIntensityFilter(1.0);
        QVERIFY(controller.announcer_->eligible(e));
        controller.settings()->setLocalIntensityFilter(5.0);
        e.magnitude = 6.0; e.distanceKm = 10;
        QVERIFY(IntensityCalculator::formatJma(6.0, 10.0, 10.0) == "5弱");
        QVERIFY(controller.announcer_->eligible(e));
        e.distanceKm = 5;
        QVERIFY(IntensityCalculator::formatJma(6.0, 5.0, 10.0) == "5强");
        QVERIFY(controller.announcer_->eligible(e));
        controller.settings()->setIntensityStandard(0);
        controller.settings()->setLocalIntensityFilter(2.0);
        e.magnitude = 3; e.depth = 0; e.rawIntensity = 2.5;
        // 总开关关闭：只展示，不提醒。
        controller.settings()->setEnableWarnings(false);
        QVERIFY(!controller.announcer_->eligible(e));
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);
        e.distanceKm = -1;
        // 不做震级过滤：任何实时事件都进入生命周期用于展示。
        controller.handleEvent(e, false, false);
        QVERIFY(controller.hasWarning());
        QVERIFY(controller.warningOverlayVisible());
        QCOMPARE(controller.countdown(), -1);
        controller.setManualLocation(30.9, 104.3, "测试定位");
        QVERIFY(controller.countdown() > 0);
        QSignalSpy camera(&controller, &AppController::centerMapRequested);
        auto corrected = e; corrected.magnitude = 5.7;
        controller.handleEvent(corrected, false, false);
        QCOMPARE(camera.count(), 0);
        QCOMPARE(controller.activeWarning().toMap()["magnitude"].toDouble(), 5.7);
        auto b = e; b.id = b.eventId = "GUI-B"; b.longitude = 104.5;
        controller.handleEvent(b, false, false);
        auto cancel = corrected; cancel.isCanceled = true;
        controller.handleEvent(cancel, false, false);
        QCOMPARE(controller.activeWarnings().size(), 1);
        QCOMPARE(controller.activeWarning_.eventId, b.eventId);
        // A directory response must neither replace nor end the realtime event.
        auto directory = b; directory.isFinal = true;
        controller.handleEvent(directory, false, true);
        QCOMPARE(controller.activeWarning_.eventId, b.eventId);
        QVERIFY(controller.warningOverlayVisible());
        QQmlApplicationEngine engine;
        QQuickStyle::setStyle("Basic");
        // 布局回调若形成绑定环，Qt 会打印 "Detected recursive rearrange"；此处收集以便断言。
        QStringList qmlWarnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) qmlWarnings.append(warning.toString());
        });
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto graphicsApi = window->rendererInterface()->graphicsApi();
        const QString evidence = evidenceDirectory(window);
        auto screenshot = [&](const QString& name) {
            QTest::qWait(150);
            return window->grabWindow().save(evidence + "/" + name + ".png");
        };
        auto clickText = [&](const QString& text) {
            for (auto* item : visualItems(window->contentItem())) {
                // 图标化按钮没有可见 text，回退到 accessibleName 匹配。
                const bool matches = item->property("text").toString() == text
                                  || item->property("accessibleName").toString() == text;
                if (!matches || !item->isVisible() || !item->isEnabled()) continue;
                // Controls expose clicked; their label children do not.
                if (item->metaObject()->indexOfSignal("clicked()") < 0) continue;
                const QPointF p = item->mapToScene(QPointF(item->width()/2, item->height()/2));
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, p.toPoint());
                return true;
            }
            return false;
        };
        QVERIFY(screenshot("01-warning"));
        // 桌面端取消全屏预警：主 HUD 下方是承载本地到时与操作的预警卡。
        auto* warningHud = window->findChild<QQuickItem*>("warningHud");
        QVERIFY(warningHud && warningHud->isVisible());
        // 预警卡按钮：悬停、焦点与禁用态的前景/底色都必须可区分。
        // 必须筛出真正的控件，Text 子项同样有 text 但没有 hovered。
        QQuickItem* alertButton = nullptr;
        // 卡片收窄后按钮文字压成「停止」，完整措辞放在 accessibleName（同 clickText 的匹配口径）。
        for (auto* item : warningHud->findChildren<QQuickItem*>())
            if (item->property("accessibleName").toString() == "停止本次提醒"
                && item->property("hovered").isValid()) alertButton = item;
        QVERIFY(alertButton);
        window->requestActivate();
        for (bool dark : {false, true}) {
            controller.settings()->setThemeMode(dark ? "dark" : "light");
            const QPoint center = alertButton->mapToScene(QPointF(alertButton->width()/2, alertButton->height()/2)).toPoint();
            QTest::mouseMove(window, QPoint(0, 0));
            QTest::qWait(30);
            QTest::mouseMove(window, center);
            QTRY_VERIFY_WITH_TIMEOUT(alertButton->property("hovered").toBool(), 1000);
            QVERIFY(alertButton->property("foregroundColor").value<QColor>() != alertButton->property("backgroundColor").value<QColor>());
            alertButton->forceActiveFocus(Qt::TabFocusReason);
            QVERIFY(alertButton->hasActiveFocus());
            alertButton->setEnabled(false);
            QVERIFY(alertButton->property("foregroundColor").value<QColor>() != alertButton->property("backgroundColor").value<QColor>());
            alertButton->setEnabled(true);
        }
        QVERIFY(controller.hasWarning());
        auto* hud = window->findChild<QQuickItem*>("hudCard");
        QVERIFY(hud && hud->isVisible());
        {
            QString offender;
            QVERIFY2(timesFullyShown(hud, &offender),
                     qPrintable("HUD 发震时刻被截断: " + offender));
        }
        QVERIFY(screenshot("02-hud"));
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map);
        // S 波径向渐变填充（QtQuick.Shapes）须已实例化，否则说明该 QML 模块加载失败。
        QVERIFY(window->findChild<QQuickItem*>("sWaveFill"));
        const double oldZoom = map->property("zoom").toDouble();
        QVERIFY(QMetaObject::invokeMethod(map, "zoomAt", Q_ARG(QVariant, 500), Q_ARG(QVariant, 400), Q_ARG(QVariant, 1.0)));
        QVERIFY(!map->property("following").toBool());
        // 缩放现在是补间动画：等它到达目标值后再采样 centerLon，否则拿到的是中间值。
        QTRY_COMPARE_WITH_TIMEOUT(map->property("zoom").toDouble(), oldZoom + 1.0, 3000);
        const double manualLon = map->property("centerLon").toDouble();
        b.reportNum++;
        controller.handleEvent(b, false, false);
        QTest::qWait(50);
        QCOMPARE(map->property("centerLon").toDouble(), manualLon);
        // 桌面端没有全屏预警可"重新弹出"；重复报仍须保留预警卡与倒计时。
        QVERIFY(warningHud->isVisible());
        // 拖动必须 1:1 跟手：位移量应对应指针位移，而不是从 (0,0) 起算的跳变
        // （曾经按下时访问不存在的 root.<id> 抛异常，跳过一次 lastX/lastY 初始化）。
        {
            const double worldSize = 256.0 * std::pow(2.0, map->property("zoom").toDouble());
            const double lonBefore = map->property("centerLon").toDouble();
            const double dx = 60.0, dy = 40.0;
            const QPointF origin = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, origin.toPoint());
            for (int step = 1; step <= 3; ++step) {
                QTest::mouseMove(window, (origin + QPointF(dx * step / 3.0, dy * step / 3.0)).toPoint());
                QTest::qWait(5);
            }
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, (origin + QPointF(dx, dy)).toPoint());
            const double expected = -dx / worldSize * 360.0;
            const double actual = map->property("centerLon").toDouble() - lonBefore;
            QVERIFY2(std::abs(actual - expected) < std::abs(expected) * 0.5 + 0.05,
                     qPrintable(QStringLiteral("拖拽位移 expected %1 got %2").arg(expected).arg(actual)));
        }
        // HUD 不再提供按钮，直接驱动地图重新取景以验证跟随行为。
        QVERIFY(QMetaObject::invokeMethod(map, "frameEvent", Q_ARG(QVariant, true)));
        QVERIFY(map->property("following").toBool());
        controller.settings()->setBasemapId("osm");
        QTest::qWait(50);
        QVERIFY(!map->property("gcjDatum").toBool());
        controller.settings()->setBasemapId("amap_vector");
        QVERIFY(map->property("gcjDatum").toBool());
        // 全屏预警已取消，静音/停止改由预警卡承载。
        QVERIFY(clickText("静音本次"));
        QVERIFY(controller.sessions_.active.at(b.identity()).muted);
        QVERIFY(controller.hasWarning());
        QVERIFY(clickText("停止本次提醒"));
        QVERIFY(!controller.hasWarning());
        b.reportNum++;
        controller.handleEvent(b, false, false);
        QVERIFY(!controller.hasWarning());
        QVERIFY(screenshot("04-stopped"));
        // Expiry is controller-owned even when the warning switch is disabled.
        controller.settings()->setEnableWarnings(false);
        auto c = e; c.id = c.eventId = "GUI-C";
        controller.handleEvent(c, false, false);
        QVERIFY(controller.countdownTimer_.isActive());
        QVERIFY(!controller.warningOverlayVisible());
        QTest::qWait(50);
        QVERIFY(!warningHud->isVisible());
        now = *controller.activeWarning_.sWaveArrival + 60000;
        controller.onTick();
        QVERIFY(!controller.hasWarning());
        QVERIFY(!controller.countdownTimer_.isActive());
        // 多个活动地震：HUD 底部分页器应保持紧凑，不占用过多纵向空间。
        {
            auto p1 = e;
            p1.id = p1.eventId = "GUI-P1"; p1.location = "第一起事件（仅测试）";
            p1.pWaveArrival.reset(); p1.sWaveArrival.reset();
            p1.magnitude = 4.1; p1.longitude = 104.0;
            auto p2 = p1;
            p2.id = p2.eventId = "GUI-P2"; p2.location = "第二起事件（仅测试）"; p2.longitude = 105.2;
            now += 1000; p1.timestamp = now; p2.timestamp = now;
            controller.handleEvent(p1, false, false);
            controller.handleEvent(p2, false, false);
            QTRY_COMPARE(controller.hudCount(), 2);
            auto* pager = window->findChild<QQuickItem*>("hudPager");
            QVERIFY(pager && pager->isVisible());
            // 紧凑：分页器高度不得超过一个小图标按钮（24px）+ 少量余量。
            QVERIFY2(pager->height() <= 28,
                     qPrintable(QStringLiteral("HUD 分页器过高: %1px").arg(pager->height())));
            QVERIFY(screenshot("03-hud-pager"));
            // 翻页按钮可用：2 页时下一页按 (i+1)%2 前进，上一页回到原页。
            const int start = controller.hudIndex();
            QVERIFY(clickText("下一个事件"));
            QCOMPARE(controller.hudIndex(), (start + 1) % 2);
            QVERIFY(clickText("上一个事件"));
            QCOMPARE(controller.hudIndex(), start);
        }
        // 空闲自动归位：无活动预警时回到「我的位置」；有活动预警时回到预警震中并重新跟随。
        {
            map->setProperty("idleResetMs", 250);
            const auto dragBy = [&](double dx) {
                const QPointF origin = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, origin.toPoint());
                QTest::mouseMove(window, (origin + QPointF(dx, 0)).toPoint());
                QTest::qWait(10);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, (origin + QPointF(dx, 0)).toPoint());
            };
            // (A) 仍有活动预警（上面添加的 p1/p2）：拖走后空闲 → 重新跟随预警震中。
            {
                QVERIFY(QMetaObject::invokeMethod(map, "frameEvent", Q_ARG(QVariant, true)));
                QTest::qWait(600);
                QVERIFY(map->property("following").toBool());
                const double followLon = map->property("centerLon").toDouble();
                dragBy(140);
                QVERIFY(!map->property("following").toBool());
                QVERIFY2(std::abs(map->property("centerLon").toDouble() - followLon) > 0.5,
                         "拖动后镜头应离开震中");
                QTRY_VERIFY_WITH_TIMEOUT(map->property("following").toBool(), 3000);
                QTRY_VERIFY_WITH_TIMEOUT(std::abs(map->property("centerLon").toDouble() - followLon) < 1.0, 3000);
            }
            // (B) 停止全部预警后：拖走后空闲 → 回到我的位置。
            while (controller.hasWarning()) controller.stopWarning();
            QTest::qWait(50);
            QVERIFY(QMetaObject::invokeMethod(map, "focusUser"));
            QTest::qWait(500);
            const QVariantMap user = map->property("shiftedUser").toMap();
            const double userLon = user.value("lon").toDouble();
            dragBy(140);
            QVERIFY(!map->property("following").toBool());
            QVERIFY2(std::abs(map->property("centerLon").toDouble() - userLon) > 0.05,
                     "拖动后镜头应离开我的位置");
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(map->property("centerLon").toDouble() - userLon) < 0.01, 3000);
        }
        QVERIFY(clickText("设置"));
        QVERIFY(window->property("showSettings").toBool());
        QVERIFY(screenshot("05-settings"));
        QVERIFY(clickText("返回地图"));
        QVERIFY(!window->property("showSettings").toBool());
        qInfo() << "GUI platform" << QGuiApplication::platformName() << "renderer" << graphicsApi << "evidence" << evidence;

        // Theme changes preserve material ownership and readable primary controls.
        for (bool dark : {false, true}) {
            controller.settings()->setThemeMode(dark ? "dark" : "light");
            controller.settings()->setBackgroundBlur(false);
            QTest::qWait(80);
            QVERIFY(!hud->property("blurActive").toBool());
            QVERIFY(screenshot(dark ? "06-map-dark-blur-off" : "06-map-light-blur-off"));
            controller.settings()->setBackgroundBlur(true);
            QTest::qWait(80);
            QCOMPARE(hud->property("blurActive").toBool(), graphicsApi != QSGRendererInterface::Software);
            QVERIFY(screenshot(dark ? "07-map-dark-blur-on" : "07-map-light-blur-on"));
            QVERIFY(clickText("设置"));
            QVERIFY(screenshot(dark ? "08-settings-dark" : "08-settings-light"));
            QVERIFY(clickText("定位"));
            QVERIFY(screenshot(dark ? "08-location-dark" : "08-location-light"));
            QVERIFY(clickText("外观"));
            QVERIFY(screenshot(dark ? "09-appearance-dark" : "09-appearance-light"));
            QVERIFY(clickText("返回地图"));
        }
        // 展开事件列表，触发 EventTile 布局；布局绑定环会打印 "recursive rearrange"（见末尾断言）。
        window->setProperty("showList", true);
        QTest::qWait(250);
        {
            auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
            QVERIFY(sidebar);
            QString offender;
            QVERIFY2(timesFullyShown(sidebar, &offender),
                     qPrintable("列表发震时刻被截断: " + offender));
        }
        QVERIFY(screenshot("12-event-list"));
        window->resize(390, 720);
        QTest::qWait(200);
        QCOMPARE(map->width(), 390.0);
        window->setProperty("showSettings", true);
        QVERIFY(screenshot("10-settings-narrow"));
        auto* settingsPage = window->findChild<QQuickItem*>("settingsPage");
        QVERIFY(settingsPage);
        for (int section = 0; section < 9; ++section) {
            settingsPage->setProperty("currentSection", section);
            QTest::qWait(30);
            for (auto* item : visualItems(settingsPage)) {
                if (!item->isVisible() || item->width() == 0 || item->height() == 0) continue;
                if (item->metaObject()->indexOfSignal("clicked()") < 0 && item->objectName() != "manualLatitude" && item->objectName() != "manualLongitude") continue;
                const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                QVERIFY2(rect.left() >= 0 && rect.right() <= window->width() + 1, qPrintable(item->property("text").toString()));
            }
            // 数据源是唯一靠内部分栏的版面：窄屏必须退回「下拉框 + 单栏详情」，
            // 而不是把两栏硬压进 390 px。留一张图作为证据。
            if (section == 6) {
                auto* navCard = window->findChild<QQuickItem*>("sourceNavCard");
                QVERIFY(navCard);
                QVERIFY(!navCard->isVisible());
                auto* selector = window->findChild<QQuickItem*>("sourceSelectorCombo");
                QVERIFY(selector);
                QVERIFY(selector->isVisible());
                QVERIFY(screenshot("10b-settings-source-narrow"));
            }
        }
        settingsPage->setProperty("currentSection", 0);
        QVERIFY(screenshot("11-appearance-narrow"));
        // Every combo has exactly one indicator, and controls remain native/focusable.
        QVERIFY(!window->findChildren<QQuickItem*>("comboIndicator").isEmpty());
        // 任何 QML 布局回调形成绑定环都会打印 "Detected recursive rearrange"；不得出现。
        for (const auto& warning : qmlWarnings)
            QVERIFY2(!warning.contains("recursive rearrange"), qPrintable(warning));
    }

    // 启动瞬间必须可交互：冷启动路径也要像托盘恢复那样把焦点拿过来。
    //
    // 缺陷原状：窗口由 QML 的 `visible: !app.startHidden` 自己显示，Qt 不会替我们
    // requestActivate()。在 click-to-focus 的 WM 下，点一个未聚焦窗口的第一次点击
    // 会被 WM 吃掉用于抬起窗口，应用收不到，表现为「刚出来的一瞬间点不动」。
    // main.cpp 的 presentWindow（托盘/预警）一直是 show+raise+requestActivate，
    // 冷启动这条一直漏着。
    //
    // 这条用例锁两件事：
    //  1. Main.qml 里确实存在启动期的 raise + requestActivate（按源码断言，因为
    //     offscreen 平台会自动激活窗口，行为断言测不出差别 —— 见下一条注释）。
    //  2. 「等就绪再显示窗口」这条错误修法不会被误引入：从未 show 过的窗口
    //     contentItem 恒为 0x0，Qt 不给隐藏窗口派发布局，那样会永久等待。
    void startupClaimsFocusAndWindowMustBeShownToLayOut() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("StartupFocusTest");
        AppController controller(nullptr, false);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // 1) 启动期拿焦点的那两下必须写在 QML 里，且真的被 Component.onCompleted 调用。
        //    只查「字符串存在」是不够的：函数定义本身也含 claimFocus()，会把
        //    「定义了但没人调」当成通过（这条断言自己就踩过，见提交记录）。
        QFile mainQml(QStringLiteral(KOMIRA_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY2(mainQml.open(QIODevice::ReadOnly), "读不到 qml/Main.qml");
        const QString source = QString::fromUtf8(mainQml.readAll());
        QVERIFY2(source.contains(QStringLiteral("requestActivate()")),
                 "Main.qml 缺少启动期的 requestActivate()：未聚焦窗口的第一次点击"
                 "会被窗口管理器吃掉，表现为启动瞬间点不动");
        QVERIFY2(source.contains(QStringLiteral("window.raise()")),
                 "Main.qml 缺少启动期的 raise()");
        // 取 Component.onCompleted 的**函数体**，要求 claimFocus() 出现在这段花括号里。
        // 必须带冒号：正文注释里也写着「在 Component.onCompleted 里 resize …」，
        // 只按 `Component.onCompleted` 找会命中那句注释，然后取到相邻函数的函数体，
        // 断言就永远为假（这条断言自己踩过这个坑）。
        const int completedAt = source.indexOf(QStringLiteral("Component.onCompleted:"));
        QVERIFY(completedAt >= 0);
        const int bodyOpen = source.indexOf(QLatin1Char('{'), completedAt);
        QVERIFY(bodyOpen > completedAt);
        int depth = 0, bodyEnd = -1;
        for (int i = bodyOpen; i < source.size(); ++i) {
            if (source.at(i) == QLatin1Char('{')) ++depth;
            else if (source.at(i) == QLatin1Char('}') && --depth == 0) { bodyEnd = i; break; }
        }
        QVERIFY(bodyEnd > bodyOpen);
        const QString completedBody = source.mid(bodyOpen, bodyEnd - bodyOpen);
        QVERIFY2(completedBody.contains(QStringLiteral("claimFocus()")),
                 "claimFocus() 定义了但 Component.onCompleted 没有调用它 —— "
                 "冷启动仍然拿不到焦点");

        // 2) 反证：从未 show 过的窗口不会布局，所以不能改成「等就绪再 show」。
        QQuickWindow neverShown;
        neverShown.setColor(Qt::transparent);
        neverShown.resize(400, 300);
        QTest::qWait(80);
        QVERIFY2(!neverShown.isVisible(), "前提失效：窗口不该是可见的");
        QCOMPARE(neverShown.contentItem()->width(), 0.0);
        QCOMPARE(neverShown.contentItem()->height(), 0.0);
    }

    // 托盘闪烁必须与通知 / 音效 / HUD 同门槛：未达本地烈度过滤的预警级事件
    // 只展示、不闪托盘（曾只按 levelTag 判断，导致全球任意 M4.5+ 都闪红）。
    void trayBlinkRespectsLocalIntensityFilter() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("TrayBlinkTest");
        AppController controller(nullptr, false);
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(3.0);
        controller.setManualLocation(30.9, 104.3, "测试定位");
        TrayController tray(&controller);
        QVERIFY(!tray.blinkTimer_.isActive());

        // 远源M5.0：震级判WARNING，但本地烈度远低于阈值。
        EarthquakeEvent e;
        e.id = e.eventId = "TRAY-1";
        e.sourceProvider = "Pancakes"; e.sourceAgency = "USGS";
        e.source = "GlobalQuake地震信息";
        e.timestamp = controller.nowMs();
        e.latitude = 35.0; e.longitude = 139.0; e.depth = 20;
        e.magnitude = 5.0; e.location = "日本东京附近";
        controller.handleEvent(e, false, false);
        QCOMPARE(controller.activeWarning().toMap()["levelTag"].toString(), QStringLiteral("WARNING"));
        QVERIFY(!controller.alertEligible());
        QVERIFY2(!tray.blinkTimer_.isActive(), "未达烈度过滤阈值时托盘不应闪烁");

        // 调低阈值至放行，同一事件应立即开始闪烁（设置变更经recompute → syncWarning 生效）。
        controller.settings()->setLocalIntensityFilter(0.0);
        QVERIFY(controller.alertEligible());
        QVERIFY2(tray.blinkTimer_.isActive(), "通过烈度过滤后托盘应闪烁");

        // 总开关关闭同样不该闪。
        controller.settings()->setEnableWarnings(false);
        QVERIFY(!tray.blinkTimer_.isActive());
    }

    /// 矢量底图：必须真的不再请求栅格瓦片。
    ///
    /// 回归点：basemapId="vector" 时若 tileUrl/syncTiles 不早退，会沿用默认的
    /// 高德模板，用户看到「选了矢量却还是高德」。
    ///
    /// 断言走 QML 侧暴露的 tileCount：ListModel 只有 id，C++ 侧 property() 取不到它。
    /// 注意 Main.qml 用 Loader { active: window.visible }，故必须先 show() 让 Loader
    /// 激活、syncTiles 跑完，再取属性 —— 否则读到的是尚未完成 Component.onCompleted 的实例。
    void vectorBasemapSuppressesRasterTiles() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorBasemapTest");
        AppController controller(nullptr, false);
        // 通过 SettingsStore 自身的 API 设置：预置 QSettings 的 org/app 名可能与
        // QSettings 默认构造不一致，会写到别的文件，让断言建立在假前提上。
        controller.settings()->setBasemapId("vector");
        controller.settings()->setVectorOverlay(false);
        QCOMPARE(controller.settings()->basemapId(), QStringLiteral("vector"));

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(400);

        auto* mapView = window->findChild<QQuickItem*>("mapView");
        QVERIFY(mapView);
        QCOMPARE(mapView->property("basemap").toString(), QStringLiteral("vector"));
        // 矢量底图下不应残留任何栅格瓦片（否则用户看到的就是高德/OSM）。
        QCOMPARE(mapView->property("tileCount").toInt(), 0);
        QCOMPARE(mapView->property("vectorOnly").toBool(), true);
        // 矢量层必须是 mapRenderLayer 的后代：它既是渲染内容，也是模糊的
        // backdropSource（BackdropBlur 用它采样）。若被提到 root 之下，模糊就
        // 捕获不到地图内容 → 用户看到「组件的模糊没了」。
        auto* renderLayer = window->findChild<QQuickItem*>("mapRenderLayer");
        QVERIFY2(renderLayer, "mapRenderLayer");
        for (const char* name : {"vectorWorldLayer", "vectorProvinceLayer", "vectorDistrictLayer"}) {
            auto* layer = window->findChild<QQuickItem*>(name);
            QVERIFY2(layer, name);
            QVERIFY2(layer->parentItem() == renderLayer,
                     "vector layer must be a child of mapRenderLayer (blur backdrop)");
        }
        auto* district = window->findChild<QQuickItem*>("vectorDistrictLayer");
        auto* pWave = window->findChild<QQuickItem*>("pWaveRing");
        auto* sFill = window->findChild<QQuickItem*>("sWaveFill");
        auto* sWave = window->findChild<QQuickItem*>("sWaveRing");
        auto* hypocenter = window->findChild<QQuickItem*>("hypocenterMarker");
        QVERIFY(district && pWave && sFill && sWave && hypocenter);
        QVERIFY2(district->z() < pWave->z(), "P wave must be above district intensity shading");
        QVERIFY2(district->z() < sFill->z(), "S wave fill must be above district intensity shading");
        QVERIFY2(district->z() < sWave->z(), "S wave must be above district intensity shading");
        // 十字已提到 mapRenderLayer **之外**（否则整片盖不过测站层，见
        // epicenterAndUserMarkerStayAboveStationLayer）。跨子树后 z 不能直接比：
        // 判据改成「所在标记层是 mapRenderLayer 的兄弟，且 z 更高」——
        // 那正是「压在区县着色与波前之上」在场景图里的等价说法。
        auto* markerLayer = window->findChild<QQuickItem*>("mapMarkerLayer");
        QVERIFY2(markerLayer, "mapMarkerLayer");
        QVERIFY2(hypocenter->parentItem() == markerLayer,
                 "hypocenter must live in mapMarkerLayer");
        QVERIFY2(markerLayer->parentItem() == renderLayer->parentItem()
                     && markerLayer->z() > renderLayer->z(),
                 "震中标记层必须是 mapRenderLayer 的兄弟且压在其上");
        // 世界/区县层始终参与渲染；省级分界线只在拉近后画（见下个断言）。
        QVERIFY(window->findChild<QQuickItem*>("vectorWorldLayer")->property("visible").toBool());
        QVERIFY(window->findChild<QQuickItem*>("vectorDistrictLayer")->property("visible").toBool());
        // 全国概览（z4）时省界隐藏 —— 35 条线会糊成噪声；拉近到 z5 才出现。
        auto* province = window->findChild<QQuickItem*>("vectorProvinceLayer");
        QVERIFY(province);
        QVERIFY2(!province->property("visible").toBool(),
                 "province borders must be hidden at national overview zoom");
        mapView->setProperty("zoom", 5.0);
        QTRY_VERIFY_WITH_TIMEOUT(province->property("visible").toBool(), 1000);
        mapView->setProperty("zoom", 4.0);
        QTRY_VERIFY_WITH_TIMEOUT(!province->property("visible").toBool(), 1000);
        // 且矢量数据确实加载成功（3.7 MiB 的 .kvb）。
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY2(world->property("ready").toBool(), "vector data (.kvb) failed to load");
        QCOMPARE(world->property("featureCount").toInt(), 1285);

        // 端到端护栏：矢量图必须真的画出东西。
        // 曾经因视口裁剪把纬度写反 —— 属性全部正确、画面却一片空白，
        // 只断言属性抓不到，必须看像素。
        {
            // 采样地图区域左侧（避开右侧控件栏与左下角徽章）的不同颜色数。
            auto mapColours = [&]() {
                const QImage img = window->grabWindow();
                QSet<QRgb> colors;
                for (int y = img.height() / 4; y < img.height() * 3 / 4; y += 7) {
                    for (int x = img.width() / 10; x < img.width() / 2; x += 7) {
                        colors.insert(img.pixel(x, y));
                    }
                }
                return colors.size();
            };
            // VectorMapItem 首次 paint 才建离屏缓存/三角化网格（~90 ms/三层），且绘制
            // 结果要下一帧才进场景图。用 QTRY 反复抓帧直到出现内容。
            //
            // 判据不能是「不同颜色数 > 5」：矢量层是低饱和度灰白配色（填充 #EAEFF0、
            // 省界 #5A6A6C），采样下来只有 3 种颜色，画得完全正确时该断言也会失败。
            // 改为统计「明显暗于背景的像素比例」—— 省界与海岸线是深色细线。
            auto inkRatio = [&]() -> double {
                const QImage img = window->grabWindow();
                int ink = 0, total = 0;
                for (int y = img.height() / 4; y < img.height() * 3 / 4; y += 3) {
                    for (int x = img.width() / 10; x < img.width() / 2; x += 3) {
                        ++total;
                        if (qGray(img.pixel(x, y)) < 150) ++ink;
                    }
                }
                return total > 0 ? double(ink) / total : 0.0;
            };
            Q_UNUSED(mapColours);
            // 采样前先拉到 z6：z4 时省界未启用（provinceBorderMinZoom = 5），画面只剩
            // 浅色的世界陆地填充 + 同色系描边，深色像素几乎为 0 —— 那时「画得对」也
            // 测不出来。z6 有省界与区县边界，深色细线占比约 1.7%。
            mapView->setProperty("centerLat", 35.0);
            mapView->setProperty("centerLon", 105.0);
            mapView->setProperty("zoom", 6.0);
            QTRY_VERIFY_WITH_TIMEOUT(inkRatio() > 0.002, 12000);
        }

        // 截图存证：属性对不代表画得出来。设 KOMIRA_EVIDENCE_DIR 才落盘。
        // 这一步曾经抓到「视口裁剪把纬度写反导致整图空白」的 bug。
        const QString evidence = qEnvironmentVariable("KOMIRA_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(evidence + "/vector-basemap.png"));
        }
    }

    /// 栅格瓦片预取必须有界：单层只预取一圈，跨层过渡最多保留当前层 + 一层底衬。
    /// 这条护栏锁住的是 delegate 数量，而不是网络是否可用。
    void rasterTilePrefetchStaysBounded() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("RasterTilePrefetchTest");

        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("osm");
        controller.settings()->setVectorOverlay(false);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        QVERIFY(mapView);

        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("tileCount").toInt() > 0, 1000);
        QCOMPARE(mapView->property("prefetchMargin").toInt(), 1);
        QVERIFY2(mapView->property("tileCount").toInt() <= 80,
                 "single raster layer should stay within one-prefetch-ring budget");

        mapView->setProperty("zoom", 7.0);
        QTest::qWait(100);
        QVERIFY2(mapView->property("tileCount").toInt() <= 160,
                 "zoom transition should retain at most two bounded tile layers");
    }

    /// 复现/回归：跨 zoom 层级时不得让旧层瓦片在屏幕上「淡出」。
    /// 旧的实现把两个层级的瓦片都做成 opacity 0→1 的淡入，且底衬层自己不淡出：
    /// 跨层瞬间旧层新 delegate 从透明开始，用户看到整屏旧地图慢慢浮出/消失。
    /// 这里用本地彩色瓦片（file URL），在跨层后连抓多帧，断言：
    ///   1) 每一帧旧层瓦片都不以半透明形式存在（opacity 恒为 1）；
    ///   2) 抓帧像素里不出现「旧层颜色与新层颜色各占一半」的过渡色。
    void zoomTransitionKeepsTileLayersOpaque() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("TileOpacityTest");
        qputenv("QML_DISABLE_DISK_CACHE", "1");

        // 本地生成纯色瓦片：只需覆盖镜头附近的瓦片窗口（全量 z2..8 是 7 万多张，
        // 会把测试拖到两分钟以上）。颜色按 tz 区分，同一层所有瓦片同色，
        // 便于用像素判断「当前显示的是哪一层」。文件名 {z}/{x}/{y}.png，
        // 配合 customBasemapUrl 的 file:// 模板。
        const QString tileDir = isolated.path() + "/tiles";
        auto levelColor = [](int z) {
            // 每层一个高饱和主色，避免与背景/矢量层混淆。
            static const QColor colors[] = {
                QColor(0xE6, 0x39, 0x46), QColor(0xF7, 0x67, 0x06),
                QColor(0xE9, 0xAB, 0x00), QColor(0x2E, 0x8B, 0x57),
                QColor(0x1D, 0x7F, 0xD6), QColor(0x6E, 0x56, 0xCF),
                QColor(0xC0, 0x36, 0x9B), QColor(0x8D, 0x6E, 0x63),
                QColor(0x0F, 0x76, 0x6E),
            };
            return colors[z % 9];
        };
        auto tileXAt = [](double lon, int z) {
            return int(std::floor((lon + 180.0) / 360.0 * (1 << z)));
        };
        auto tileYAt = [](double lat, int z) {
            const double rad = lat * M_PI / 180.0;
            const double y = (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / M_PI) / 2.0;
            return int(std::floor(y * (1 << z)));
        };
        QImage img(256, 256, QImage::Format_RGB32);
        for (int z = 2; z <= 8; ++z) {
            const int n = 1 << z;
            const int cx = tileXAt(105.0, z);
            const int cy = tileYAt(30.0, z);
            QVERIFY(QDir().mkpath(tileDir + "/" + QString::number(z)));
            for (int x = cx - 7; x <= cx + 7; ++x) {
                if (x < 0 || x >= n) continue;
                const QString dir = tileDir + "/" + QString::number(z) + "/" + QString::number(x);
                QVERIFY(QDir().mkpath(dir));
                for (int y = cy - 5; y <= cy + 6; ++y) {
                    if (y < 0 || y >= n) continue;
                    img.fill(levelColor(z));
                    QVERIFY2(img.save(dir + "/" + QString::number(y) + ".png"),
                             "failed to write local tile");
                }
            }
        }

        AppController controller(nullptr, false);
        controller.settings()->setCustomBasemapUrl("file://" + tileDir + "/{z}/{x}/{y}.png");
        controller.settings()->setCustomBasemapDatum(0);
        controller.settings()->setBasemapId("custom");
        controller.settings()->setVectorOverlay(false);
        controller.settings()->setShowStationLayer(false);
        controller.settings()->setShowWavefront(false);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        QVERIFY(mapView);

        // 镜头放到固定位置、整数层级，等瓦片全部 Ready。
        mapView->setProperty("centerLat", 30.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 5.0);
        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("tileModelZoom").toInt() == 5, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("topLayerTileCount").toInt() > 0, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("topLayerPendingTiles").toInt() == 0, 5000);
        window->grabWindow();

        // 场景图里的 Image delegate 按 model 行序排列；找出 z=5 的瓦片节点，
        // 其 opacity 必须是 1（旧层不得从 0 淡入）。
        auto tileImages = [&](int tz) {
            QList<QQuickItem*> out;
            auto* renderLayer = window->findChild<QQuickItem*>("mapRenderLayer");
            if (!renderLayer) return out;
            for (auto* item : visualItems(renderLayer)) {
                if (item->inherits("QQuickImage") && item->parentItem()
                        && item->parentItem()->property("tz").toInt() == tz
                        && item->parentItem()->property("tz").isValid()) {
                    out.append(item);
                }
            }
            return out;
        };
        // 自检：确实找到了瓦片节点，否则下面的断言会空转通过。
        QTRY_VERIFY_WITH_TIMEOUT(!tileImages(5).isEmpty(), 2000);

        // 跨层：z=5 → z=6。旧层立即成为底衬。
        mapView->setProperty("zoom", 6.0);
        // 换层是同步通知的：此刻新层瓦片还一张都没加载，底衬必须已经就位。
        // （不能 QTRY 等——本地瓦片可能在下一帧就全部就绪，底衬随即被正常撤除。）
        QCOMPARE(mapView->property("underlayZ").toInt(), 5);
        // 确定性检查：新层集合建立时，父层兜底必须全部关闭。
        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("tileModelZoom").toInt() == 6, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(mapView->property("topLayerTileCount").toInt() > 0, 1000);
        for (auto* item : visualItems(window->contentItem())) {
            if (!item->inherits("QQuickImage")) continue;
            auto* cell = item->parentItem();
            if (!cell || !cell->property("parentLevel").isValid()) continue;
            QVERIFY2(cell->property("parentLevel").toInt() < 0
                     || !cell->property("active").toBool(),
                     "tz-2 parent fallback is active while an underlay is present — "
                     "users see a coarser zoom flash");
        }

        // 连续采样一个淡入窗口（180ms）加余量；同时容忍底衬在过渡中被撤除
        // （本地瓦片加载极快时，整个过渡可能只持续几帧）。
        const QColor newColor = levelColor(6);
        const QColor oldColor = levelColor(5);
        const QColor mixColor((oldColor.red() + newColor.red()) / 2,
                              (oldColor.green() + newColor.green()) / 2,
                              (oldColor.blue() + newColor.blue()) / 2);
        auto near = [](const QColor& a, const QColor& b) {
            return std::abs(a.red() - b.red()) < 20
                && std::abs(a.green() - b.green()) < 20
                && std::abs(a.blue() - b.blue()) < 20;
        };
        // 地图区域采样点：避开左上 HUD、右侧控件栏、左下角徽章。
        auto sampleMapPixels = [&](const QImage& shot, auto&& fn) {
            for (int y = shot.height() / 3; y < shot.height() * 2 / 3; y += 7)
                for (int x = shot.width() / 4; x < shot.width() / 2; x += 7)
                    fn(shot.pixelColor(x, y));
        };
        int oldSamples = 0, blendedFrames = 0, parentBlinkFrames = 0;
        for (int frame = 0; frame < 12; ++frame) {
            QTest::qWait(20);
            const QImage shot = window->grabWindow();
            for (auto* img : tileImages(5)) {
                oldSamples++;
                QVERIFY2(img->opacity() > 0.999,
                         qPrintable(QStringLiteral("underlay tile at opacity %1 — "
                                                   "old zoom tiles visibly fade")
                                    .arg(img->opacity())));
            }
            // 当前层瓦片一旦出现在屏幕上，也不能是半透明的（淡入会让新旧层混色）。
            for (auto* img : tileImages(6)) {
                if (img->opacity() > 0 && img->opacity() < 0.999)
                    QFAIL("top-layer tile is rendering half-transparent (cross-fade between zooms)");
            }
            // 父层兜底（tz-2）在跨层过渡期绝不能被画出来：它比底衬更糊，
            // 一旦显示就是「整屏闪一下更粗的 zoom」。底衬存在时必须始终不可见。
            bool parentVisible = false;
            for (auto* item : visualItems(window->contentItem())) {
                if (!item->inherits("QQuickImage")) continue;
                auto* parent = item->parentItem();
                if (!parent || !parent->property("parentLevel").isValid()) continue;
                if (parent->property("parentLevel").toInt() >= 0 && item->isVisible()
                        && item->opacity() > 0.01 && item->width() > 0 && item->height() > 0) {
                    parentVisible = true;
                }
            }
            if (parentVisible) parentBlinkFrames++;
            // 像素层：过渡帧里不应出现「旧新各半」的混合色。
            int blended = 0;
            sampleMapPixels(shot, [&](const QColor& c) {
                if (near(c, mixColor)) blended++;
            });
            if (blended > 2) blendedFrames++;
        }
        QVERIFY2(oldSamples > 0 || mapView->property("underlayZ").toInt() < 0,
                 "no underlay tiles were sampled while the underlay was still alive");
        QVERIFY2(parentBlinkFrames == 0,
                 qPrintable(QStringLiteral("%1 transition frames drew the tz-2 parent fallback — "
                                           "users see a coarser zoom flash")
                                .arg(parentBlinkFrames)));
        QVERIFY2(blendedFrames == 0,
                 qPrintable(QStringLiteral("%1 transition frames showed blended old/new tile "
                                           "colours — the two zoom levels cross-fade")
                                .arg(blendedFrames)));

        // 过渡结束后：只剩新层，旧层必须已撤除。
        QTest::qWait(400);
        const QImage shot = window->grabWindow();
        int solidOld = 0, solidNew = 0;
        sampleMapPixels(shot, [&](const QColor& c) {
            if (near(c, oldColor)) solidOld++;
            else if (near(c, newColor)) solidNew++;
        });
        QVERIFY2(solidNew > 0, "new zoom level did not render after transition");
        QVERIFY2(solidOld == 0, "old zoom tiles still visible long after transition");
    }

    void districtShadingClearsOnCanceledReport() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("DistrictCancelTest");

        AppController controller(nullptr, false);
        // 区县着色跟随矢量图层：开启叠加才会计算。
        controller.settings()->setVectorOverlay(true);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };

        EarthquakeEvent event;
        event.id = event.eventId = "DISTRICT-CANCEL";
        event.timestamp = now;
        event.reportNum = 1;
        event.latitude = 30.6;
        event.longitude = 104.0;
        event.depth = 10.0;
        event.magnitude = 6.5;
        event.location = "测试事件";
        controller.handleEvent(event, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.districtIntensity_.isEmpty(), 1500);

        auto canceled = event;
        canceled.reportNum = 2;
        canceled.isCanceled = true;
        controller.handleEvent(canceled, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.districtIntensity_.isEmpty(), 1500);
    }

    /// 跨日界线时矢量底图两半都要有内容。
    ///
    /// 回归点：相机经度被 QML 夹在 ±180，但视口可越界；数据只含 [-180,180] 一份几何。
    /// 若渲染不按 ±360 环绕，从中国向东平移到美国途中，视口一跨 ±180° 就有半幅是空的
    /// （用户看到的「地图突然消失一半」）。
    void vectorBasemapWrapsAcrossAntimeridian() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorAntimeridianTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        controller.settings()->setVectorOverlay(false);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY(mapView && world);
        QVERIFY2(world->property("ready").toBool(), "vector data (.kvb) failed to load");
        QTest::qWait(500);

        // 贴近日界线、位于中国与北美之间：视口同时跨过 ±180°。
        // 左半应见堪察加半岛（东半球），右半应见阿拉斯加/阿留申（西半球的 +360° 副本）。
        mapView->setProperty("centerLat", 45.0);
        mapView->setProperty("centerLon", 179.0);
        mapView->setProperty("zoom", 4.0);
        QTest::qWait(200);

        // 采样左右两半的地图区域（避开右侧工具条与底部徽章）的陆地像素数：
        // 陆地填充明显比背景暗，修复前半幅为 0。
        auto landCounts = [&](int& left, int& right) {
            const QImage img = window->grabWindow();
            left = right = 0;
            for (int y = 60; y < img.height() - 140; ++y) {
                for (int x = 8; x < img.width() - 100; ++x) {
                    const QRgb c = img.pixel(x, y);
                    if (qRed(c) >= 248 && qGreen(c) >= 250) continue;   // 背景/空白
                    if (x < img.width() / 2) ++left;
                    else ++right;
                }
            }
        };
        int left = 0, right = 0;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { landCounts(left, right); return left > 150 && right > 150; })(),
                                 5000);
        qInfo() << "PROBE antimeridian land pixels: left" << left << "right" << right;

        const QString evidence = qEnvironmentVariable("KOMIRA_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QTest::qWait(200);
            QVERIFY(window->grabWindow().save(evidence + "/vector-antimeridian.png"));
        }
    }

    /// 缩放时矢量层必须与震中/定位标记同源。
    ///
    /// 回归点：离屏位图缓存曾只做平移、不随 scale 重渲 —— 缩放时矢量层停在旧比例上，
    /// 与标记逐渐错位，并在平移量超出缓存 margin 触发重渲时整屏跳变（即用户看到的
    /// “缩放抖动、震中和我的位置定不住”）。这里在分数 zoom 下比对「矢量层实际绘制的
    /// 投影」(VectorMapItem::screenPosForTest) 与「标记所用的 QML 投影」(screenX/screenY)。
    void vectorZoomStaysAlignedWithMarkers() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorZoomAlignTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        controller.settings()->setVectorOverlay(false);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY(mapView && world);
        QVERIFY2(world->property("ready").toBool(), "vector data (.kvb) failed to load");
        QTest::qWait(900);   // 等初始自动取景补间结束，避免动画中途改相机

        // 先在整数层级画一帧，让离屏缓存在 scale=1 的比例下建好。
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 6.0);
        window->grabWindow();

        // 再做一次很小的缩放变化：旧实现里它的平移量落在缓存 margin 内（≈165px < 192），
        // 会复用旧比例的位图而不重渲，于是矢量层与标记立刻错位。
        mapView->setProperty("zoom", 6.02);
        window->grabWindow();

        // screenX/screenY 带类型标注（`function screenX(lon: real): real`），
        // 编译后的元调用签名是 double(double)，不能再按 QVariant 传参。
        auto qmlNum = [&](const char* fn, double arg) -> double {
            double ret = qQNaN();
            const bool ok = QMetaObject::invokeMethod(mapView, fn, Q_RETURN_ARG(double, ret),
                                                      Q_ARG(double, arg));
            return ok ? ret : qQNaN();
        };
        auto aligned = [&]() -> bool {
            for (const QPointF& p : {QPointF(116.4, 39.9), QPointF(121.5, 31.2),
                                     QPointF(104.1, 30.6)}) {
                QPointF got;
                QMetaObject::invokeMethod(world, "screenPosForTest", Q_RETURN_ARG(QPointF, got),
                                          Q_ARG(double, p.y()), Q_ARG(double, p.x()));
                const double expX = qmlNum("screenX", p.x());
                const double expY = qmlNum("screenY", p.y());
                if (!(std::hypot(got.x() - expX, got.y() - expY) < 1.0)) {
                    qWarning() << "misaligned" << p << "got" << got << "exp" << expX << expY;
                    return false;
                }
            }
            return true;
        };
        // 抓帧要等渲染线程把新的 paint 结果落地，故重试若干帧。
        bool ok = false;
        for (int i = 0; i < 50 && !ok; ++i) {
            QTest::qWait(20);
            window->grabWindow();
            ok = aligned();
        }
        QVERIFY2(ok, "矢量层在缩放后与震中/定位标记错位");
    }

    /// LOD 分档与缓存记忆：低缩放必须用粗档、拉近必须回到全精度，且隐藏时释放位图。
    ///
    /// 回归点（2026-10 矢量层重构）：全精度重绘在缓存尺寸下要 13–57 ms/次，LOD 是把它
    /// 压到一帧预算内的关键手段。若以后有人把档位算错（例如忘了乘 dpr、或档位方向反了），
    /// 属性断言仍是全绿、只有手感变差 —— 所以这里直接断言绘制实际采用的容差档。
    void vectorLodTiersAndCacheRelease() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorLodTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* district = window->findChild<QQuickItem*>("vectorDistrictLayer");
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY(mapView && district && world);

        // 几何与首个档位都在 worker 线程构建：等到本层真的画出东西为止。
        auto lodTol = [&](QQuickItem* layer) {
            double tol = 0.0;
            QMetaObject::invokeMethod(layer, "lodToleranceForTest", Q_RETURN_ARG(double, tol));
            return tol;
        };
        auto cacheBytes = [&](QQuickItem* layer) {
            qint64 bytes = 0;
            QMetaObject::invokeMethod(layer, "cacheBytesForTest", Q_RETURN_ARG(qint64, bytes));
            return bytes;
        };
        // GPU 路径（非软件后端）不再建离屏位图，故「有内容」的判据按后端区分。
        const bool softwareBackend =
            window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        auto triangles = [&](QQuickItem* layer) {
            qint64 v = -1;
            QMetaObject::invokeMethod(layer, "submittedTrianglesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };
        auto hasContent = [&](QQuickItem* layer) {
            return softwareBackend ? cacheBytes(layer) > 0 : triangles(layer) > 0;
        };
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 5.0);
        QVERIFY2(QTest::qWaitFor([&] { window->grabWindow(); return hasContent(world); }, 15000),
                 "world layer never produced drawable content");

        // 全国概览（z5）：容差必须 > 0（说明真的在简化），且是 0.5 设备像素对应的档。
        const double coarse = lodTol(world);
        QVERIFY2(coarse > 0.0, qPrintable(QStringLiteral("z5 未启用 LOD，tol=%1").arg(coarse)));
        // 期望 tolRef ≈ 0.5 / 2^(zoom-kRefZoom)，档位量化到 2 的幂：允许 1 个档的偏差。
        const double ideal = 0.5 / std::pow(2.0, 5.0 - 10.0);
        QVERIFY2(coarse <= ideal * 2.5 && coarse >= ideal / 2.5,
                 qPrintable(QStringLiteral("z5 LOD 档位偏离预期：tol=%1 ideal=%2")
                                .arg(coarse).arg(ideal)));

        // 拉近到 z 12：容差必须比 z5 细得多（LOD 随缩放宽严）。
        //
        // 这里刻意**不**断言「回到全精度（tol == 0）」：0.5 设备像素在 z12 对应
        // 0.125 参考层级像素，量化后是 tier=-3，仍然处于简化档 —— 那是正确行为。
        // 早期断言能通过只是因为 CPU 路径的 drawnTolRef_ 跟着「上一次缓存重建」走，
        // 而 z12 没有触发重建，读到的是旧档位的残值；GPU 路径每次同步最新档位，
        // 于是暴露出这个断言本身与实现对不上。改为断言「显著变细」。
        const double coarseAtZ5 = coarse;
        mapView->setProperty("zoom", 12.0);
        // 拉近后容差必须显著变细。
        //
        // 判据用 `fine == 0 || fine < coarse/2`：0 表示已切到**全精度**——z12 时
        // 0.5 设备像素的预算细于 1 参考层级像素，按 wantedTier 的语义就该走全精度
        //（tier >= kMaxTier），而不是继续留在简化档。早期实现把 tier 夹到 kMaxTier，
        // 会让拉近后的线条明显发钝；这里同时守住这两种合法结果。
        //
        // CPU 路径的档位在 worker 线程构建，换档前会沿用上一档（「不闪空白」的设计），
        // 所以要给足时间。
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&] {
                window->grabWindow();
                const double fine = lodTol(world);
                return fine == 0.0 || fine < coarseAtZ5 / 2.0;
            })(),
            20000);

        if (softwareBackend) {
            // 以下三条只对 CPU 光栅化路径成立（GPU 路径没有离屏位图）。
            // 隐藏即释放：矢量叠加关闭后不应再占着十几 MB 位图。
            QVERIFY(cacheBytes(world) > 0);
            world->setProperty("visible", false);
            QTRY_VERIFY_WITH_TIMEOUT(cacheBytes(world) == 0, 3000);

            // 单张位图必须落在预算内（高分屏降档逻辑）。16 MiB / 4 B/px => 420 万像素。
            world->setProperty("visible", true);
            mapView->setProperty("zoom", 5.0);
            QTRY_VERIFY_WITH_TIMEOUT(cacheBytes(world) > 0, 5000);
            QVERIFY2(cacheBytes(world) <= 16LL * 1024 * 1024,
                     qPrintable(QStringLiteral("缓存位图超预算：%1 B").arg(cacheBytes(world))));

            // 区县层平时全透明（无震度）：必须一个面都不画，缓存保持为 0。
            QVERIFY2(cacheBytes(district) == 0,
                     "无震度时区县层不该建位图（1011 面全描边实测 24 ms）");
        } else {
            // GPU 路径：网格常驻显存但不应失控；区县层无震度时不提交三角形。
            qint64 meshBytes = 0;
            QMetaObject::invokeMethod(world, "meshBytesForTest", Q_RETURN_ARG(qint64, meshBytes));
            QVERIFY2(meshBytes > 0, "GPU 路径未持有网格");
            QVERIFY2(meshBytes < 512LL * 1024 * 1024,
                     qPrintable(QStringLiteral("网格内存超预算：%1 MB").arg(meshBytes / 1048576.0)));
            QVERIFY2(triangles(district) == 0, "无震度时区县层不该提交三角形");
            // 隐藏后 QQuickItem 不再走 updatePaintNode，故提交计数会**停在旧值**上，
            // 不能拿它断言「已释放」。正确判据是「item 已不可见，且网格仍可用」：
            // 网格常驻是 GPU 路径的设计（复用比释放更划算，只有几十 MB 上限）。
            world->setProperty("visible", false);
            QTRY_VERIFY_WITH_TIMEOUT(!world->isVisible(), 3000);
            world->setProperty("visible", true);
            QTRY_VERIFY_WITH_TIMEOUT(world->isVisible(), 3000);
        }
    }

    /// GPU 矢量路径：有 GPU 时必须走三角化网格，且**不**再光栅化整层位图。
    ///
    /// 回归点（2026-10 GPU 化）：CPU 路径每移出缓存余量就要重新光栅化 13–57 ms；
    /// GPU 路径把几何一次性建成三角形网格，平移/缩放只改变换、按分块做视口裁剪。
    /// 这里断言四件事：
    ///   ① 非软件后端下 `gpuPathActive` 为真，且离屏位图缓存为 0（没有再走 QPainter）；
    ///   ② 提交的三角形数随缩放下降（LOD 生效），且远小于整层总量（视口裁剪生效）；
    ///   ③ 网格字节数落在预算内（分块把 float32 精度问题解决，不能靠放大分块来省内存）；
    ///   ④ 软件后端必须回退到 CPU 位图路径（否则 CI 上整层空白）。
    void vectorGpuPathUsesTiledMesh() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorGpuPathTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY(mapView && world);

        const bool software =
            window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        auto gpuActive = [&] {
            bool v = false;
            QMetaObject::invokeMethod(world, "gpuPathActiveForTest", Q_RETURN_ARG(bool, v));
            return v;
        };
        auto triangles = [&] {
            qint64 v = -1;
            QMetaObject::invokeMethod(world, "submittedTrianglesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };
        auto tilesDrawn = [&] {
            int v = -1;
            QMetaObject::invokeMethod(world, "drawnTilesForTest", Q_RETURN_ARG(int, v));
            return v;
        };
        auto meshBytes = [&] {
            qint64 v = -1;
            QMetaObject::invokeMethod(world, "meshBytesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };
        auto cacheBytes = [&] {
            qint64 v = -1;
            QMetaObject::invokeMethod(world, "cacheBytesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };

        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 6.0);
        // 等首帧真的画出内容。两个后端的「有内容」判据不同：GPU 路径提交三角形，
        // CPU 路径建离屏位图（且 submittedTriangles 恒为 0，不能拿它判断）。
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         window->grabWindow();
                         return software ? cacheBytes() > 0 : triangles() > 0;
                     },
                     15000),
                 "矢量层未在超时内画出内容");

        if (software) {
            // 软件后端没有可用的自定义材质支持：必须回退到 CPU 位图路径，
            // 否则整层空白（CI 与远程桌面都是这个后端）。
            QVERIFY2(!gpuActive(), "软件后端不应走 GPU 三角化路径");
            QVERIFY2(cacheBytes() > 0, "软件后端必须回退到离屏位图");
            qInfo() << "PROBE gpu-path: 软件后端，按预期回退到 CPU 光栅化";
            return;
        }

        QVERIFY2(gpuActive(), "非软件后端应启用 GPU 三角化路径");
        QVERIFY2(cacheBytes() == 0, "GPU 路径不应再保留整层离屏位图");

        const qint64 trisAtZ6 = triangles();
        const int tilesAtZ6 = tilesDrawn();
        qInfo() << "PROBE gpu-path: z6 tiles=" << tilesAtZ6 << "tris=" << trisAtZ6
                << "meshMB=" << meshBytes() / 1048576.0;
        QVERIFY2(trisAtZ6 > 0, "GPU 路径未提交任何三角形");
        // 视口裁剪：z6 下世界层只应提交少量分块（实测 9 块量级），而不是全部 700+ 块。
        QVERIFY2(tilesAtZ6 > 0 && tilesAtZ6 < 80,
                 qPrintable(QStringLiteral("视口裁剪失效：z6 竟然画了 %1 个分块").arg(tilesAtZ6)));
        // 整层总量（world 层 z10 全精度约 100 万三角形）：视口裁剪后必须低一个数量级。
        QVERIFY2(trisAtZ6 < 300000,
                 qPrintable(QStringLiteral("提交三角形过多（视口裁剪/LOD 失效）：%1").arg(trisAtZ6)));

        // 缩到全国概览：LOD 档位变粗（容差按 2^(zoom-10) 放大），**每单位面积的
        // 几何精度下降**。注意不能断言「提交三角形总数减少」—— z2 的视口覆盖的世界
        // 范围是 z6 的 16 倍，块数变多，总数未必下降。正确的判据是 LOD 容差变粗。
        auto lodTol = [&] {
            double v = -1.0;
            QMetaObject::invokeMethod(world, "lodToleranceForTest", Q_RETURN_ARG(double, v));
            return v;
        };
        const double tolAtZ6 = lodTol();
        mapView->setProperty("zoom", 2.0);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { window->grabWindow(); return lodTol() > tolAtZ6; })(), 15000);
        qInfo() << "PROBE gpu-path: z2 tiles=" << tilesDrawn() << "tris=" << triangles()
                << "tolRef" << lodTol();

        // 网格内存预算：world+district 常驻不应失控（分块而非整层 float32）。
        QVERIFY2(meshBytes() > 0, "GPU 路径未持有网格");
        QVERIFY2(meshBytes() < 512LL * 1024 * 1024,
                 qPrintable(QStringLiteral("网格内存超预算：%1 MB").arg(meshBytes() / 1048576.0)));

        // 端到端护栏：GPU 路径必须真的画出东西（属性对不代表画得出来）。
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 5.0);
        auto mapColours = [&]() {
            const QImage img = window->grabWindow();
            QSet<QRgb> colors;
            for (int y = img.height() / 4; y < img.height() * 3 / 4; y += 7) {
                for (int x = img.width() / 10; x < img.width() / 2; x += 7) {
                    colors.insert(img.pixel(x, y));
                }
            }
            return colors.size();
        };
        // 先抓帧存证再断言：断言失败时 QTest 会直接返回，拿不到画面。
        QTest::qWait(1200);
        const QString evidence = qEnvironmentVariable("KOMIRA_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            window->grabWindow().save(evidence + "/vector-gpu-path.png");
        }

        // 端到端护栏：GPU 路径必须真的画出东西。
        //
        // 不能沿用 CPU 用例的「不同颜色数 > 5」判据：矢量层是低饱和度的灰白配色
        //（填充 #EAEFF0、省界 #5A6A6C），采样下来本来就只有 3 种颜色 —— 那条断言在
        // 画得完全正确时也会失败。改为统计「明显暗于背景的像素比例」：省界与海岸线
        // 是深色细线，空白区域则不是。
        auto inkRatio = [&]() -> double {
            const QImage img = window->grabWindow();
            int ink = 0, total = 0;
            for (int y = img.height() / 4; y < img.height() * 3 / 4; y += 3) {
                for (int x = img.width() / 10; x < img.width() / 2; x += 3) {
                    const QRgb c = img.pixel(x, y);
                    ++total;
                    // 背景（#EAEFF0 及更亮）远亮于省界（#5A6A6C）
                    if (qGray(c) < 150) ++ink;
                }
            }
            return total > 0 ? double(ink) / total : 0.0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(inkRatio() > 0.002, 8000);
        qInfo() << "PROBE gpu-path: ink ratio" << inkRatio();
    }

    /// 几何上传次数：相机移动**不应**重传顶点/索引缓冲。
    ///
    /// 回归点（2026-10-20 用户实测「开省界很卡」）：GPU 路径初版每帧都删掉全部
    /// QSGGeometryNode、重建 QSGGeometry 并把顶点索引重新 memcpy —— 相当于每帧向
    /// GPU 重新上传几 MB，比原来的 CPU 光栅化还慢。正确形态是「几何传一次，
    /// 之后只改变换矩阵」。这里断言连续拖动 30 帧期间上传次数不增长。
    void vectorGpuGeometryUploadsOncePerMesh() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorUploadTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        controller.settings()->setShowProvinceBorders(true);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        auto* province = window->findChild<QQuickItem*>("vectorProvinceLayer");
        QVERIFY(mapView && world && province);

        const bool software =
            window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        if (software) {
            qInfo() << "PROBE uploads: 软件后端走 CPU 光栅化，跳过";
            return;
        }

        auto uploads = [&](QQuickItem* layer) {
            qint64 v = -1;
            QMetaObject::invokeMethod(layer, "geometryUploadsForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };
        auto triangles = [&](QQuickItem* layer) {
            qint64 v = -1;
            QMetaObject::invokeMethod(layer, "submittedTrianglesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };

        // 省界在 z >= 5 才可见 —— 用户报的场景正是这个缩放区间。
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 116.4);
        mapView->setProperty("zoom", 6.0);
        QVERIFY2(QTest::qWaitFor([&] { window->grabWindow(); return triangles(province) > 0; },
                                 30000),
                 qPrintable(QStringLiteral("省界层未在超时内画出内容（world tris=%1 province "
                                           "visible=%2 zoom=%3）")
                                .arg(triangles(world))
                                .arg(province->isVisible())
                                .arg(mapView->property("zoom").toDouble())));

        // world 层的网格比省界层大得多（块数多一个数量级），构建也更慢，单独等它。
        QVERIFY2(QTest::qWaitFor([&] { window->grabWindow(); return triangles(world) > 0; }, 30000),
                 "world 层未在超时内画出内容");
        const qint64 worldBefore = uploads(world);
        const qint64 provinceBefore = uploads(province);
        QVERIFY2(worldBefore > 0 && provinceBefore > 0,
                 qPrintable(QStringLiteral("首帧应至少上传过一次几何（world=%1 province=%2）")
                                .arg(worldBefore).arg(provinceBefore)));

        // 连续平移：块集合会跨过边界若干次（每次允许一次重建），但绝大多数帧
        // 只应更新变换矩阵。
        const int frames = 40;
        for (int i = 0; i < frames; ++i) {
            mapView->setProperty("centerLon", 116.4 - i * 0.15);
            window->grabWindow();
        }
        const qint64 worldUploads = uploads(world) - worldBefore;
        const qint64 provinceUploads = uploads(province) - provinceBefore;
        qInfo() << "PROBE uploads:" << frames << "帧拖动后 world +" << worldUploads
                << "province +" << provinceUploads;

        // 块集合变化（跨块边界）时才重建，最多几次；绝不是每帧一次。
        QVERIFY2(worldUploads <= 5,
                 qPrintable(QStringLiteral("world 层 40 帧上传了 %1 次几何（应只在换块/换档时重建）")
                                .arg(worldUploads)));
        QVERIFY2(provinceUploads <= 5,
                 qPrintable(QStringLiteral("省界层 40 帧上传了 %1 次几何（用户报卡顿的正是它）")
                                .arg(provinceUploads)));

        // 缩放跨 LOD 档时**允许**重建（描边宽度烘焙在顶点里），但也不能每帧一次。
        const qint64 beforeZoom = uploads(province);
        for (int i = 0; i < 20; ++i) {
            mapView->setProperty("zoom", 6.0 + i * 0.1);
            window->grabWindow();
        }
        const qint64 zoomUploads = uploads(province) - beforeZoom;
        qInfo() << "PROBE uploads: 20 帧缩放后 province +" << zoomUploads;
        QVERIFY2(zoomUploads <= 10,
                 qPrintable(QStringLiteral("缩放 20 帧上传了 %1 次几何").arg(zoomUploads)));
    }

    /// 拖动性能基准：模拟连续平移，测量每帧渲染耗时。
    /// 只记录不硬性断言（阈值随机器/后端差异大），用于定位卡顿来源。
    void vectorDragPerformance() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("VectorDragPerf");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        QVERIFY(mapView);
        QTest::qWait(300);

        // 几何与首个 LOD 档位在 worker 线程构建，必须先等它落地再计时，
        // 否则测到的是「还没画出来」的 0 ms（曾经的探针就是这样测不到东西的）。
        auto* world = window->findChild<QQuickItem*>("vectorWorldLayer");
        QVERIFY(world);
        auto cacheBytes = [&] {
            qint64 bytes = 0;
            QMetaObject::invokeMethod(world, "cacheBytesForTest", Q_RETURN_ARG(qint64, bytes));
            return bytes;
        };
        const bool softwareBackend =
            window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        auto triangles = [&] {
            qint64 v = -1;
            QMetaObject::invokeMethod(world, "submittedTrianglesForTest", Q_RETURN_ARG(qint64, v));
            return v;
        };
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 116.4);
        mapView->setProperty("zoom", 6.0);
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         window->grabWindow();
                         return softwareBackend ? cacheBytes() > 0 : triangles() > 0;
                     },
                     15000),
                 "矢量层未在超时内完成首帧");
        window->grabWindow();   // 预热：让 LOD 档位与纹理都就位

        qint64 worst = 0, total = 0;
        const int frames = 40;
        QElapsedTimer t;
        auto rebuildsNow = [&] {
            qint64 count = 0;
            QMetaObject::invokeMethod(world, "cacheRebuildsForTest", Q_RETURN_ARG(qint64, count));
            return count;
        };
        const qint64 before = rebuildsNow();
        for (int i = 0; i < frames; ++i) {
            t.restart();
            // zoom 6 下 0.5° ≈ 23 px：40 帧共 ~900 px，必然跨过 160+128 px 的缓存余量，
            // 逼出多次真实重建 —— 这正是「拖动卡顿」的来源，必须测到它。
            mapView->setProperty("centerLon", 116.4 - i * 0.5);
            window->grabWindow();
            const qint64 ms = t.elapsed();
            worst = qMax(worst, ms);
            total += ms;
        }
        const qint64 rebuilds = rebuildsNow() - before;
        qInfo() << "PROBE drag:" << frames << "frames, avg" << (total / frames) << "ms, worst"
                << worst << "ms, cache rebuilds" << rebuilds;
        // 拖动 40 帧必须真的产生过重绘，否则这个探针又退化成「测不到东西」。
        //
        // 只对 CPU 路径断言：GPU 路径拖动时**不重建几何**（只改变换），这正是它的
        // 收益所在，`cacheRebuilds` 为 0 是预期行为而不是探针失效。
        if (softwareBackend) {
            QVERIFY2(rebuilds > 0, "40 帧拖动没有触发任何缓存重建，探针无效");
        } else {
            QVERIFY2(triangles() > 0, "GPU 路径拖动期间没有任何几何提交");
        }
    }

    /// 回归：目录整表重发时，未变化的事件不应重复写库；新条目合并成一次事务。
    /// 逐条 upsert + prune 实测 200 条约 400 ms，启动/复显时会把 GUI 线程卡住 1–2 s。
    void historyWritesAreCoalesced() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("HistoryBatchTest");
        AppController controller(nullptr, false);
        QVERIFY(controller.historyStore_);
        QVERIFY(controller.historyStore_->isReady());

        EarthquakeEvent event;
        event.id = "batch-1";
        event.eventId = "batch-1";
        event.source = "cenc";
        event.sourceAgency = "CENC";
        event.location = "测试";
        event.magnitude = 5.0;
        event.timestamp = 1700000000000LL;

        QVERIFY(controller.upsertHistory(event));
        controller.queueHistoryWrite(event);
        QCOMPARE(int(controller.pendingHistoryWrites_.size()), 1);
        QTest::qWait(250);   // 80 ms 合并窗口
        QVERIFY(controller.pendingHistoryWrites_.empty());
        QCOMPARE(controller.historyStore_->loadRecent(10).size(), 1);

        // 目录重发同样内容：upsertHistory 返回 false，上层不会产生写库任务。
        QVERIFY(!controller.upsertHistory(event));

        // 内容变化：按最新值落库。
        event.magnitude = 9.9;
        QVERIFY(controller.upsertHistory(event));
        controller.queueHistoryWrite(event);
        controller.flushHistoryWrites();
        QCOMPARE(controller.historyStore_->loadRecent(10).size(), 1);
        QCOMPARE(controller.historyStore_->loadRecent(10)[0].magnitude, 9.9);
    }

    /// 右上角测站卡：收起时**堆叠**、悬停展开成一排、离开收回，且前排优先级正确。
    ///
    /// 用 StationStackScene.qml 夹具直接喂数据（不联网）：
    ///  - 收起态：只露前 3 张（peekCount），后卡沿**左下**各错开 peekX / peekY，
    ///    z 由槽位决定，第 4 张起 opacity = 0；
    ///  - 展开态：卡间距 = 卡宽 + gap，整排宽度 = n·卡宽 + (n-1)·gap，且前排仍贴右缘；
    ///  - 优先级：优先「读数变过」的源，同级按跨制式折算后的读数从高到低，
    ///    都没有变化、也都没有读数时回落到数据源注册顺序（NIED 最前）；
    ///  - reduceMotion 打开时没有过渡动画（几何直接到位）。
    void stationStackExpandsOnHoverAndOrdersFrontByChangeAndValue() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        AppController controller(nullptr, false);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl::fromLocalFile(QStringLiteral(KOMIRA_SOURCE_DIR "/tests/StationStackScene.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto* stack = window->findChild<QQuickItem*>("fixtureStack");
        QVERIFY(stack);
        auto* map = window->findChild<QQuickItem*>("fixtureMap");
        QVERIFY(map);
        // 把容器尺寸/位置从 QML 的 Item 坐标换算成场景坐标用。
        auto sceneCenterOf = [&](QQuickItem* item) {
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };
        auto cardOf = [&](const QString& id) {
            return findVisualItem(window, "stationCard-" + id);
        };
        auto feed = [&](const std::function<void(QVariantList&)>& mutate) {
            QVariantList list = stack->property("sources").toList();
            mutate(list);
            stack->setProperty("sources", list);
            // sync() 在 sourcesChanged 里同步跑，但排序动画/布局要等一次事件循环。
            QCoreApplication::processEvents();
        };

        const QStringList ids{ "kmoni", "snet", "kma_station", "pancakes_seis", "wolfx_seis" };
        QCOMPARE(stack->property("count").toInt(), ids.size());
        for (const QString& id : ids) QVERIFY2(cardOf(id), qPrintable("缺少卡片 " + id));

        // 1. 默认：五路都无数据 → 前排回落注册顺序第一个（NIED），unexpanded。
        QCOMPARE(stack->property("frontId").toString(), QStringLiteral("kmoni"));
        QVERIFY(!stack->property("expanded").toBool());
        const qreal cardWidth = stack->property("cardWidth").toReal();
        // 卡高要等 delegate 量过之后才是真实值（首帧回落到默认 74），这里取稳定值。
        QTRY_VERIFY(stack->property("cardHeight").toReal() >= 40);
        const qreal cardHeight = stack->property("cardHeight").toReal();
        QVERIFY(cardWidth >= 80);

        // 2. 收起态几何：**左下堆叠**（每张卡向左、向下各错开一小步），且只露前 3 张。
        {
            QCOMPARE(stack->property("peekCount").toInt(), 3);
            QCOMPARE(stack->property("collapsedCount").toInt(), 3);
            const qreal peekX = stack->property("peekX").toReal();
            const qreal peekY = stack->property("peekY").toReal();
            QVERIFY(peekX > 1 && peekY > 1);
            // 尺寸只覆盖 3 张纸边：不是 5 张全错开。
            QVERIFY2(std::abs(stack->width() - (cardWidth + peekX * 2)) < 0.5,
                     qPrintable(QStringLiteral("收起态应只露 3 张: %1").arg(stack->width())));
            QVERIFY2(std::abs(stack->height() - (cardHeight + peekY * 2)) < 0.5,
                     qPrintable(QStringLiteral("收起态应只露 3 张的高度: %1").arg(stack->height())));
            bool sawSecond = false;
            for (int slot = 0; slot < 3; ++slot) {
                auto* card = cardOf(ids.at(stack->property("order").toList().at(slot).toInt()));
                QVERIFY(card);
                QVERIFY2(card->isVisible() && card->opacity() > 0.99,
                         "前 3 张必须完整可见");
                QVERIFY2(std::abs(card->x() - (stack->width() - cardWidth - slot * peekX)) < 0.5,
                         qPrintable(QStringLiteral("槽位 %1 的卡没有向左错开").arg(slot)));
                QVERIFY2(std::abs(card->y() - slot * peekY) < 0.5,
                         qPrintable(QStringLiteral("槽位 %1 的卡没有向下错开").arg(slot)));
                if (slot == 1) sawSecond = true;
            }
            QVERIFY(sawSecond);
            // 「看得出是叠着的」靠**一条发丝边 + 一点点压暗**：靠后的几张纸边比前排深一档，
            // 但底下的磨砂玻璃仍然透得出来（用户要求「底下的两张也要有模糊，不能是纯色」）。
            // 所以：**不要**盖不透明纸、**不要**压白，也**不要**加深描边或投影 ——
            // 那几版要么把模糊糊成平色，要么在浅色地图上很脏（用户原话「好丑」）。
            double lastShade = 0;
            for (int slot = 0; slot < 3; ++slot) {
                auto* card = cardOf(ids.at(stack->property("order").toList().at(slot).toInt()));
                QVERIFY(card);
                // z == 50 是发丝边框（必须 **color 透明**，否则会把模糊盖成平色）；
                // z == 51 是「越深越暗」的那层极薄压暗。
                double paper = -1, shade = 0;
                bool frameTransparent = false;
                for (auto* item : visualItems(card)) {
                    if (item->parentItem() != card) continue;
                    const qreal z = item->property("z").toReal();
                    if (z >= 50 && z < 51) {
                        paper = item->opacity();
                        frameTransparent = item->property("color").value<QColor>().alpha() == 0;
                    }
                    if (z >= 51 && z < 100) shade = item->opacity();
                }
                if (slot == 0) {
                    QVERIFY2(paper <= 0.001, "前排不该有纸边层（纸边要最亮）");
                } else {
                    QVERIFY2(paper > 0.99, "靠后的卡没有纸边层，纸边读不出层次");
                    QVERIFY2(frameTransparent,
                             "纸边层必须 color 透明：盖了实色纸/压白会把模糊糊成平色");
                }
                // 靠后的卡必须把玻璃底衬调薄：底衬太厚时，64 px 的模糊只剩一片均值，
                // 9 px 的纸边看上去就是纯色（用户：「底下的两张没有模糊」）。
                // 浅色主题才调薄底衬；深色主题的底衬本来是深的，调薄会变灰板。
                auto* testTheme = stack->property("theme").value<QObject*>();
                QVERIFY(testTheme);
                const bool dark = testTheme->property("dark").toBool();
                if (slot == 0 || dark) {
                    QCOMPARE(card->property("tintOpacity").toReal(), 1.0);
                } else {
                    QVERIFY2(card->property("tintOpacity").toReal() < 0.9,
                             "靠后的卡没有把玻璃底衬调薄，模糊透不出来");
                }
                // 压暗必须逐级变重，否则后两张会糊成同一块。
                if (slot == 0) {
                    QVERIFY2(shade <= 0.001, "前排不该压暗");
                } else {
                    QVERIFY2(shade > 0.001, "靠后的卡没有压暗，纸边与卡面分不开");
                    QVERIFY2(shade > lastShade, "纸边压暗没有随深度递进");
                }
                lastShade = shade;
            }
            // 第 4、5 张在收起态必须完全隐藏（用户只要「看得出是叠着的」）。
            for (int slot = 3; slot < ids.size(); ++slot) {
                auto* card = cardOf(ids.at(stack->property("order").toList().at(slot).toInt()));
                QVERIFY(card);
                QVERIFY2(!card->isVisible() || card->opacity() < 0.01,
                         qPrintable(QStringLiteral("第 %1 张在收起态不该露出").arg(slot + 1)));
            }
            // 前排压在后面那些卡的上面。
            auto* first = cardOf(ids.at(stack->property("order").toList().at(0).toInt()));
            auto* second = cardOf(ids.at(stack->property("order").toList().at(1).toInt()));
            QVERIFY2(first->z() > second->z(), "前排必须压在后面的卡上面");
        }

        // 「热」是按时间窗口算的；用例把窗口压到 250 ms，否则要真等 20 秒。
        stack->setProperty("hotWindowMs", 250);

        // 3. 读数高者优先（都冷下来之后）：跨制式折算 —— CSIS 6.0 > JMA 4.6（≈5.9）
        //    > KMA 3（=2）> 无数据。Pancakes 应排前排。
        feed([](QVariantList& list) {
            for (QVariant& entry : list) {
                QVariantMap m = entry.toMap();
                if (m["standard"].toString() == "jma") {
                    // 日本两路保持缺测：前排应由有读数的那几路里最高的占。
                    m["hasData"] = false; m["raw"] = -3.0; m["text"] = "--";
                } else if (m["standard"].toString() == "kma") {
                    m["hasData"] = true; m["raw"] = 3.0; m["text"] = "3";
                } else {
                    m["hasData"] = true; m["raw"] = 2.0; m["text"] = "2";
                }
                entry = m;
            }
            QVariantMap pancakes = list.at(3).toMap();
            pancakes["raw"] = 6.0; pancakes["text"] = "6";
            list[3] = pancakes;
        });
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("pancakes_seis"), 3000);
        QCOMPARE(cardOf("pancakes_seis")->z(), ids.size());   // 槽位 0 → z 最大
        // priority() 是函数而非属性：读属性拿不到值（防把它改成两边不同步的缓存）。
        QVERIFY(!stack->property("priority").isValid());

        // 4. 迟滞：新读数只高 0.1 度时前排不让位（否则读数抖动会每帧换卡）。
        //    NIED 4.6（JMA）≈ CSIS 5.9，比 Pancakes 6.0 低一点。
        auto setSource = [&](int index, double raw, const QString& text) {
            feed([&](QVariantList& list) {
                QVariantMap m = list.at(index).toMap();
                m["hasData"] = true; m["raw"] = raw; m["text"] = text;
                list[index] = m;
            });
        };
        // 先等 Pancakes 也冷下来：两路都在「热」窗口内时比的是读数，不是变化。
        QTest::qWait(1200);
        setSource(0, 4.6, QStringLiteral("5弱"));
        // 刚变过 → 立刻浮到前排（此时 Pancakes 已冷）。
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("kmoni"), 1000);
        // 冷下来后：Pancakes 6.0 只高 0.1，落在 0.3 的迟滞带内 → NIED 继续占前排。
        QTest::qWait(1200);
        QCOMPARE(stack->property("frontId").toString(), QStringLiteral("kmoni"));
        // 明显更高（0.4 ≥ 0.3）才换人。
        setSource(3, 6.4, "6");
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("pancakes_seis"), 3000);

        // 5. 「读数变过的优先」：读数最低的那一路刚刚变过，也要浮到前排；冷却后按读数沉回去。
        QTest::qWait(1200);
        QCOMPARE(stack->property("frontId").toString(), QStringLiteral("pancakes_seis"));
        setSource(2, 2.5, "3");   // KMA：变化 0.5 > changeEpsilon，读数却是最低的
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("kma_station"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("pancakes_seis"), 4000);

        // 5b. 全部静稳（有数据但没有读数）：前排必须回落到 NIED（注册顺序第一个）。
        //     守的是「安静时别让恒报基线的源霸占前排」与「都静稳就显示 nied」两条口径。
        feed([](QVariantList& list) {
            for (QVariant& entry : list) {
                QVariantMap m = entry.toMap();
                if (m["standard"].toString() == "jma") {
                    m["hasData"] = true; m["raw"] = 0.0; m["text"] = "0";
                } else if (m["standard"].toString() == "kma") {
                    m["hasData"] = true; m["raw"] = 1.0; m["text"] = "1";
                } else {
                    m["hasData"] = true; m["raw"] = 0.0; m["text"] = "0";
                }
                entry = m;
            }
        });
        QTRY_COMPARE_WITH_TIMEOUT(stack->property("frontId").toString(),
                                  QStringLiteral("kmoni"), 3000);

        // 6. 悬停展开：卡间距变成 卡宽 + gap，整排变宽，前排仍贴右缘；离开后收回。
        //    过渡期间卡片的玻璃底必须切到 live 逐帧重采样，否则采样纹理不随卡片移动，
        //    观感是「模糊被卡片拖着走、落定那一刻跳回去」（用户实测反馈的问题）。
        QCOMPARE(stack->property("cardBackdropLive").toBool(), false);
        QCOMPARE(stack->property("cardBackdropLive").toBool(), stack->property("backdropLive").toBool());
        // 把动画拉慢到 1.2 s：`grabWindow` 本身要十几毫秒，220 ms 的动画常常
        // 「一抓就已经落定」，取证图便抓不到真正的过渡帧（也就无法肉眼核对模糊）。
        stack->setProperty("durationOverride", 1200);
        QTest::mouseMove(window, QPoint(0, 0));
        QTest::qWait(30);
        QTest::mouseMove(window, sceneCenterOf(stack));
        QTRY_VERIFY_WITH_TIMEOUT(stack->property("expanded").toBool(), 1500);
        // 动画一起步就必须 live；终点落定 180 ms 后才关回去（关的那一帧纹理是旧的，
        // 立刻关会在终点闪一下）。
        QTRY_VERIFY_WITH_TIMEOUT(stack->property("animating").toBool(), 1000);
        QVERIFY(stack->property("cardBackdropLive").toBool());
        // 动画中途抓一帧留证：背景是竖条纹，模糊若被卡片「拖走」或中途闪断，
        // 取证图上一眼可见。动画已被拉慢到 1.2 s，抓图本身（十几毫秒）不会让它落定。
        {
            QTRY_VERIFY_WITH_TIMEOUT(
                stack->property("progress").toReal() > 0.25
                && stack->property("progress").toReal() < 0.95, 2000);
            window->grabWindow().save(evidenceDirectory(window)
                                      + "/59f-station-stack-mid.png");
        }
        // 断言要一路落到**卡片自己**的模糊采样源上：root 的派生属性对，
        // 不代表 GlassCard::backdropLive 透传下去、更不代表 ShaderEffectSource 真的 live。
        {
            auto* card = cardOf(stack->property("frontId").toString());
            QVERIFY(card);
            QVERIFY2(card->property("backdropLive").toBool(),
                     "展开动画期间卡片的玻璃底没有切到 live 逐帧重采样");
            const bool software = window->rendererInterface()->graphicsApi()
                                  == QSGRendererInterface::Software;
            if (!software) {
                QQuickItem* sample = nullptr;
                for (auto* item : visualItems(card)) {
                    if (item->objectName() == QLatin1String("backdropSample")) sample = item;
                }
                QVERIFY(sample);
                QVERIFY2(sample->property("live").toBool(),
                         "展开动画期间采样纹理没有逐帧重采样（模糊会跟在卡片后面拖影）");
            }
        }
        const qreal gap = stack->property("gap").toReal();
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(stack->property("progress").toReal() - 1.0) < 0.001, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("animating").toBool(), 3000);
        QCOMPARE(stack->property("cardBackdropLive").toBool(),
                 stack->property("backdropLive").toBool());
        stack->setProperty("durationOverride", -1);   // 之后按真实时长继续验收回
        // progress 读数是浮点：容 0.5 px，不从像素级对比几何。
        QVERIFY2(std::abs(stack->property("width").toReal()
                          - (cardWidth * ids.size() + gap * (ids.size() - 1))) < 0.5,
                 qPrintable(QStringLiteral("展开宽度不对：%1")
                                .arg(stack->property("width").toReal())));
        {
            auto* front = cardOf(stack->property("frontId").toString());
            QVERIFY(front);
            QVERIFY(std::abs(front->x() - (stack->width() - cardWidth)) < 0.5);
            QSet<int> seen;
            for (const QString& id : ids) {
                auto* card = cardOf(id);
                QVERIFY(card);
                const int slot = card->property("slot").toInt();
                seen.insert(slot);
                QVERIFY(std::abs(card->y()) < 0.5);
                // 槽位间隔 = 卡宽 + gap：展开后每张卡都完整可读、互不压叠。
                QVERIFY2(std::abs(card->x() - (stack->width() - cardWidth
                                               - slot * (cardWidth + gap))) < 0.5,
                         qPrintable(QStringLiteral("%1 展开后位置不对：%2")
                                        .arg(id).arg(card->x())));
            }
            QCOMPARE(seen.size(), ids.size());   // 槽位唯一，没有两张卡挤在同一格
            // 展开态 5 张全部可见（收起态被隐藏的第 4、5 张必须淡回来）。
            for (const QString& id : ids) {
                auto* card = cardOf(id);
                QVERIFY2(card->isVisible() && card->opacity() > 0.99,
                         qPrintable(QStringLiteral("%1 展开后没有淡入").arg(id)));
            }
        }
        window->grabWindow().save(evidenceDirectory(window) + "/59-station-stack-expanded.png");

        // 7. 收回：指针移开后回到堆叠（320 ms 收起延迟 + 220 ms 动画）。
        QTest::mouseMove(window, QPoint(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("expanded").toBool(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(stack->property("progress").toReal()) < 0.001, 2000);
        // 收回后仍然只露 3 张：宽度 = 卡宽 + 2 个纸条边，第 4、5 张重新淡出。
        QVERIFY(std::abs(stack->property("width").toReal()
                         - (cardWidth + stack->property("peekX").toReal() * 2)) < 0.5);
        QTRY_VERIFY(cardOf(ids.at(3)) && cardOf(ids.at(3))->opacity() < 0.01);
        {
            auto* front = cardOf(stack->property("frontId").toString());
            QVERIFY(front);
            QVERIFY(std::abs(front->x() - (stack->width() - cardWidth)) < 0.5);
        }
        window->grabWindow().save(evidenceDirectory(window) + "/59b-station-stack-collapsed.png");

        // 8. 关动画：reduceMotion 打开后展开/收回没有过渡（一次几何直接到位）。
        //    而且「无动画」这条路径也必须把 animating 收回去 —— 否则 5 张卡会永远
        //    逐帧重采样背景（每帧重绘整棵地图子树）。
        window->setProperty("testReduceMotion", true);
        QTRY_COMPARE(stack->property("expandDuration").toInt(), 0);
        QTest::mouseMove(window, sceneCenterOf(stack));
        QTRY_VERIFY_WITH_TIMEOUT(stack->property("expanded").toBool(), 1500);
        QCOMPARE(stack->property("progress").toReal(), 1.0);
        QTest::mouseMove(window, QPoint(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("expanded").toBool(), 2000);
        QCOMPARE(stack->property("progress").toReal(), 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("animating").toBool(), 1500);

        // 9. 窄窗口放不下一整排时退回**竖向**展开：不裁切、不顶出地图左缘。
        window->resize(500, 480);
        QTest::qWait(200);
        QVERIFY2(stack->property("expandVertical").toBool(), "窄窗口应改为竖向展开");
        QTest::mouseMove(window, sceneCenterOf(stack));
        QTRY_VERIFY_WITH_TIMEOUT(stack->property("expanded").toBool(), 1500);
        QTRY_VERIFY_WITH_TIMEOUT(
            std::abs(stack->property("progress").toReal() - 1.0) < 0.001, 1500);
        const qreal stackCardHeight = stack->property("cardHeight").toReal();
        QVERIFY(std::abs(stack->property("width").toReal() - cardWidth) < 0.5);
        for (const QString& id : ids) {
            auto* card = cardOf(id);
            QVERIFY(card);
            const int slot = card->property("slot").toInt();
            QVERIFY(std::abs(card->x()) < 0.5);
            QVERIFY2(std::abs(card->y() - slot * (stackCardHeight + gap)) < 0.5,
                     qPrintable(QStringLiteral("%1 竖向展开位置不对：%2").arg(id).arg(card->y())));
            // 右缘不得顶出容器（容器本身贴着地图右缘 16 px 边距）。
            QVERIFY(card->x() + card->width() <= stack->width() + 0.5);
        }
        window->grabWindow().save(evidenceDirectory(window) + "/59c-station-stack-narrow.png");
        window->resize(900, 480);
        QTest::qWait(200);
        QTest::mouseMove(window, QPoint(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("expanded").toBool(), 2000);

        // 10. 关掉测站图层 / 没有源时整块不许占位。
        stack->setProperty("sources", QVariantList());
        QTRY_COMPARE(stack->width(), 0.0);
        QTRY_COMPARE(stack->height(), 0.0);
    }

    /// 回归：地图上的 UI 浮层必须盖住测站层（测站层 z=2）。
    /// 曾经右侧工具条没有写 z（默认 0），测站直接画在按钮上面。
    void mapOverlaysStayAboveStationLayer() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("MapOverlayZTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* station = window->findChild<QQuickItem*>("stationIntensityLayer");
        auto* toolbar = window->findChild<QQuickItem*>("mapToolbar");
        QVERIFY(station && toolbar);
        QVERIFY2(toolbar->z() > station->z(), "右侧工具条被测站层盖住");

        // 真实主窗口里也要能悬停展开：地图的 MouseArea 铺满整个视口，
        // 若它抢走 hover，右上角那摞就永远展不开（夹具用例测不到这一层）。
        auto* stack = window->findChild<QQuickItem*>("stationStack");
        QVERIFY(stack);
        QVERIFY2(stack->z() > station->z(), "测站卡被测站层盖住");
        QCOMPARE(stack->property("count").toInt(), 5);
        QCOMPARE(stack->property("frontId").toString(), QStringLiteral("kmoni"));
        const qreal collapsedWidth = stack->width();
        const QPoint stackCenter =
            stack->mapToScene(QPointF(stack->width() / 2, stack->height() / 2)).toPoint();
        QTest::mouseMove(window, QPoint(0, 0));
        QTest::qWait(30);
        QTest::mouseMove(window, stackCenter);
        QTRY_VERIFY_WITH_TIMEOUT(stack->property("expanded").toBool(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(stack->width() > collapsedWidth + 100, 2000);
        QTest::qWait(150);
        window->grabWindow().save(evidenceDirectory(window) + "/59d-map-stack-expanded.png");
        // 指针离开测站卡区域（移到地图中部）→ 收回。
        QTest::mouseMove(window, QPoint(window->width() / 2, window->height() * 3 / 4));
        QTRY_VERIFY_WITH_TIMEOUT(!stack->property("expanded").toBool(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(stack->width() - collapsedWidth) < 0.5, 2000);
        QTest::qWait(150);
        window->grabWindow().save(evidenceDirectory(window) + "/59e-map-stack-collapsed.png");
    }

    /// 回归：地图上的「震源中心」X 十字与「我的位置」圆点必须压在测站层**之上**。
    ///
    /// 用户报告：日本区震中附近测站密，红色十字被整块测站方标盖住，看不出震源中心；
    /// 中国/韩国同样会中招，只是台网密度低、恰好压在震中的概率小。
    ///
    /// 根因不是「z 写小了」，而是 QML 的 z 只在与**同一个父项的兄弟**之间比较：
    /// 十字与用户点原先都住在 `mapRenderLayer` 里（z:3），而 `StationMapItem` 是该
    /// 容器的**兄弟**（z:2）。容器内的 z 再大也压不过后绘制的兄弟 —— 上千个测站整片
    /// 盖在十字上面。给十字加 z、或给测站层减 z 都没有用，只能把它们提出容器。
    void epicenterAndUserMarkerStayAboveStationLayer() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EpicenterAboveStationsTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setBasemapId("vector");
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);
        controller.settings()->setShowStationLayer(true);
        // 偏开震中 1° 左右：既在取景范围内，又不与 40 px 的十字、38 px 的用户点叠在一起。
        controller.setManualLocation(31.5, 105.5, "测试定位");

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        auto* layer = window->findChild<komira::StationMapItem*>("stationIntensityLayer");
        QVERIFY(map && layer);

        EarthquakeEvent e;
        e.id = e.eventId = "OCCLUDE-A";
        e.sourceProvider = "Wolfx"; e.sourceAgency = "CENC"; e.source = "中国地震台网";
        e.timestamp = now - 20'000;
        e.latitude = 30.0; e.longitude = 104.0;
        e.depth = 10.0; e.magnitude = 6.0; e.location = "四川雅安";
        controller.handleEvent(e, false, false);
        QTRY_VERIFY(map->property("hasEvent").toBool());
        QTest::qWait(900);   // 等入场取景补间结束

        // 相机钉死到整数层级、进入手动模式：波前逐帧跟随会把镜头带走，像素判据就失效了。
        QVERIFY(QMetaObject::invokeMethod(map, "moveCamera",
                                          Q_ARG(QVariant, QVariant(30.6)), Q_ARG(QVariant, QVariant(104.6)),
                                          Q_ARG(QVariant, QVariant(6.0)), Q_ARG(QVariant, QVariant(0)),
                                          Q_ARG(QVariant, QVariant())));
        map->setProperty("following", false);
        QTest::qWait(200);

        // 震中与我的位置各压一个 CSIS 方标（烈度 4 = 亮蓝 #3FAFFF）：方标是 16–24 px 的
        // 实心块，恰好整块盖住 40 px 十字的中心与用户点的圆芯 —— 与日本区 NIED 密网同观感。
        komira::StationSnapshot snapshot;
        auto stationAt = [](double lat, double lon) {
            komira::StationSnapshot::Entry s;
            s.lat = lat; s.lon = lon; s.csis = true; s.raw = 4.0;
            return s;
        };
        snapshot.setEntries({stationAt(30.0, 104.0), stationAt(31.5, 105.5)});
        layer->setProperty("snapshot", QVariant::fromValue(static_cast<QObject*>(&snapshot)));
        QTest::qWait(150);

        const QImage img = window->grabWindow();
        QVERIFY(!img.isNull());
        const double scale = window->width() > 0
            ? static_cast<double>(img.width()) / window->width() : 1.0;
        auto pixel = [&](const QPointF& scenePoint) {
            const int x = qRound(scenePoint.x() * scale);
            const int y = qRound(scenePoint.y() * scale);
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return QColor();
            return img.pixelColor(x, y);
        };
        // 十字的红色描边（#E21D1D）与用户点的实心圆（theme.severity("NORMAL")，浅色 #006874）。
        auto isCrossRed = [](const QColor& c) {
            return c.isValid() && c.red() > 170 && c.green() < 90 && c.blue() < 90;
        };
        auto isUserTeal = [](const QColor& c) {
            return c.isValid() && c.red() < 90 && c.green() > 70 && c.blue() > 80;
        };
        auto isStationBlue = [](const QColor& c) {
            return c.isValid() && c.blue() > 200 && c.red() < 130;
        };

        const double hx = map->property("hypocenterX").toDouble();
        const double hy = map->property("hypocenterY").toDouble();
        double ux = 0.0, uy = 0.0;
        QVERIFY(QMetaObject::invokeMethod(map, "screenX", Q_RETURN_ARG(double, ux),
                                          Q_ARG(double, 105.5)));
        QVERIFY(QMetaObject::invokeMethod(map, "screenY", Q_RETURN_ARG(double, uy),
                                          Q_ARG(double, 31.5)));
        const QPointF epicenter = map->mapToScene(QPointF(hx, hy));
        const QPointF userPoint = map->mapToScene(QPointF(ux, uy));
        // 自检：两个标记都要在窗口内，否则下面的采样会取到窗口外（恒失败但不说明问题）。
        const QRectF viewport(0, 0, window->width(), window->height());
        QVERIFY2(viewport.contains(epicenter), "震中不在视口内，用例前提不成立");
        QVERIFY2(viewport.contains(userPoint), "我的位置不在视口内，用例前提不成立");

        // 十字的两条对角线在中心 ±7 px 处仍是 6 px 红描边；被方标盖住时这里会变成蓝色。
        // 先自检「确实有测站被画出来」：把方标挪走后该处不能是蓝的（否则判据形同虚设）。
        const QList<QPointF> probes{
            epicenter + QPointF(-7, -7), epicenter + QPointF(7, 7),
            epicenter + QPointF(-7, 7), epicenter + QPointF(7, -7)};
        int redProbes = 0;
        int blueProbes = 0;
        for (const QPointF& p : probes) {
            const QColor c = pixel(p);
            if (isCrossRed(c)) ++redProbes;
            if (isStationBlue(c)) ++blueProbes;
        }
        QVERIFY2(redProbes == probes.size(),
                 qPrintable(QStringLiteral("震中十字被测站盖住：%1/%2 个采样点是红色（蓝 %3）")
                                .arg(redProbes).arg(probes.size()).arg(blueProbes)));
        // 用户点同理：圆心必须还是主题自己的颜色，而不是压在它上面的测站方标。
        QVERIFY2(isUserTeal(pixel(userPoint)),
                 qPrintable(QStringLiteral("我的位置被测站盖住：圆心像素 %1")
                                .arg(pixel(userPoint).name())));

        window->grabWindow().save(evidenceDirectory(window) + "/61-epicenter-above-stations.png");

        // 对照：关掉测站层后同一批采样点仍应是红色/主题色 —— 证明判据测的是「谁画在上面」，
        // 而不是「恰好那里是红的」。
        controller.settings()->setShowStationLayer(false);
        QTest::qWait(150);
        const QImage noStations = window->grabWindow();
        const double scale2 = window->width() > 0
            ? static_cast<double>(noStations.width()) / window->width() : 1.0;
        auto pixel2 = [&](const QPointF& scenePoint) {
            const int x = qRound(scenePoint.x() * scale2);
            const int y = qRound(scenePoint.y() * scale2);
            if (x < 0 || y < 0 || x >= noStations.width() || y >= noStations.height()) return QColor();
            return noStations.pixelColor(x, y);
        };
        for (const QPointF& p : probes) {
            QVERIFY2(isCrossRed(pixel2(p)),
                     qPrintable(QStringLiteral("无测站时震中十字也不红：%1").arg(pixel2(p).name())));
        }
        QVERIFY2(isUserTeal(pixel2(userPoint)),
                 qPrintable(QStringLiteral("无测站时我的位置也不可见：%1").arg(pixel2(userPoint).name())));

        // 用户报的是「日本区域」，中国只是同源缺陷的另一处现场：再跑一遍日本震中，
        // 证明修复与经纬度无关（日本恰好在 GCJ-02 纠偏 bbox 之外，坐标不做平移）。
        controller.settings()->setShowStationLayer(true);
        EarthquakeEvent jp;
        jp.id = jp.eventId = "OCCLUDE-B";
        jp.sourceProvider = "Wolfx"; jp.sourceAgency = "JMA"; jp.source = "NIED";
        jp.timestamp = now + 60'000;
        jp.latitude = 35.68; jp.longitude = 139.76;   // 東京湾北部
        jp.depth = 80.0; jp.magnitude = 5.4; jp.location = "東京都千代田区";
        controller.handleEvent(jp, false, false);
        QTRY_VERIFY(map->property("hasEvent").toBool());
        QTest::qWait(900);
        QVERIFY(QMetaObject::invokeMethod(map, "moveCamera",
                                          Q_ARG(QVariant, QVariant(35.9)), Q_ARG(QVariant, QVariant(139.9)),
                                          Q_ARG(QVariant, QVariant(7.0)), Q_ARG(QVariant, QVariant(0)),
                                          Q_ARG(QVariant, QVariant())));
        map->setProperty("following", false);
        QTest::qWait(200);

        komira::StationSnapshot jpSnapshot;
        // 复刻日本现场：NIED 圆点本身只有 5 px，单独一个盖不满十字；日本区的问题是
        // **密网**——0.05° 间距（z7 约 4.5 px）的 K-NET/KiK-net 阵列铺满整片，圆点
        // 互相叠成实心色块，十字整根消失。用 9×9 阵列而不是单站，判据才对得上用户看到的现象。
        std::vector<komira::StationSnapshot::Entry> jpEntries;
        for (int i = -4; i <= 4; ++i) {
            for (int j = -4; j <= 4; ++j) {
                komira::StationSnapshot::Entry s;
                s.lat = 35.68 + i * 0.05;
                s.lon = 139.76 + j * 0.05;
                s.csis = false;   // NIED 连续震度 → 色阶圆点
                s.raw = 4.5;
                jpEntries.push_back(s);
            }
        }
        jpSnapshot.setEntries(std::move(jpEntries));
        layer->setProperty("snapshot", QVariant::fromValue(static_cast<QObject*>(&jpSnapshot)));
        QTest::qWait(150);

        const QImage jpImage = window->grabWindow();
        const double jpScale = window->width() > 0
            ? static_cast<double>(jpImage.width()) / window->width() : 1.0;
        auto jpPixel = [&](const QPointF& scenePoint) {
            const int x = qRound(scenePoint.x() * jpScale);
            const int y = qRound(scenePoint.y() * jpScale);
            if (x < 0 || y < 0 || x >= jpImage.width() || y >= jpImage.height()) return QColor();
            return jpImage.pixelColor(x, y);
        };
        const QPointF jpEpicenter = map->mapToScene(
            QPointF(map->property("hypocenterX").toDouble(),
                    map->property("hypocenterY").toDouble()));
        QVERIFY2(viewport.contains(jpEpicenter), "日本震中不在视口内，用例前提不成立");
        int jpRed = 0;
        for (const QPointF& p : probes) {
            if (isCrossRed(jpPixel(jpEpicenter + (p - epicenter)))) ++jpRed;
        }
        QVERIFY2(jpRed == probes.size(),
                 qPrintable(QStringLiteral("日本区震中十字被测站盖住：%1/%2 个采样点是红色")
                                .arg(jpRed).arg(probes.size())));
        window->grabWindow().save(evidenceDirectory(window) + "/61b-epicenter-japan-above-stations.png");
    }

    /// 回归：测站图层必须真的画出内容，且与 QML 的 screenX/screenY 同源。
    /// 该图层从 QML Canvas 改成 C++ 离屏缓存 + 仿射贴图（2026-10）：Canvas 版每次相机
    /// 变化都在 JS 里整层重画，600 个 CSIS 方标实测约 60 ms/帧。缓存复用分支在矢量层
    /// 曾经造成过缩放错位与整屏空白，这里用同一组断言守住这两类回归。
    void stationLayerDrawsAndStaysAligned() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("StationLayerTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* layer = window->findChild<QQuickItem*>("stationIntensityLayer");
        QVERIFY(mapView && layer);
        QTest::qWait(400);
        window->grabWindow();

        komira::StationSnapshot snapshot;
        layer->setProperty("snapshot", QVariant::fromValue(static_cast<QObject*>(&snapshot)));

        auto makeEntries = [](int nied, int csis, int kma = 0) {
            std::vector<komira::StationSnapshot::Entry> entries;
            entries.reserve(nied + csis + kma);
            for (int i = 0; i < nied; ++i) {
                komira::StationSnapshot::Entry e;
                e.lat = 20.0 + (i % 400) * 0.08;
                e.lon = 95.0 + (i / 400) * 0.5 + (i % 37) * 0.05;
                e.raw = double(i % 70) / 10.0 - 3.0;
                e.csis = false;
                entries.push_back(e);
            }
            for (int i = 0; i < csis; ++i) {
                komira::StationSnapshot::Entry e;
                e.lat = 25.0 + (i % 300) * 0.06;
                e.lon = 100.0 + (i / 300) * 0.4 + (i % 23) * 0.05;
                e.raw = double(i % 13);
                e.csis = true;
                entries.push_back(e);
            }
            // KMA：韩国 MMI（0–10，缺测 -3），走独立的圆点/方标图集。
            for (int i = 0; i < kma; ++i) {
                komira::StationSnapshot::Entry e;
                e.lat = 34.5 + (i % 20) * 0.12;
                e.lon = 126.2 + (i / 20) * 0.15;
                e.raw = (i % 14 == 0) ? -3.0 : double(i % 11);
                e.kma = true;
                entries.push_back(e);
            }
            return entries;
        };

        // 非空绘制护栏：同一相机下，有/无测站两帧应出现成片差异像素。
        mapView->setProperty("centerLat", 35.0);
        mapView->setProperty("centerLon", 105.0);
        mapView->setProperty("zoom", 6.0);
        snapshot.setEntries({});
        QTest::qWait(100);
        const QImage empty = window->grabWindow();
        snapshot.setEntries(makeEntries(1749, 600, 263));
        QTest::qWait(100);
        const QImage drawn = window->grabWindow();
        int changed = 0;
        for (int y = 0; y < drawn.height(); ++y) {
            const QRgb* a = reinterpret_cast<const QRgb*>(empty.constScanLine(y));
            const QRgb* b = reinterpret_cast<const QRgb*>(drawn.constScanLine(y));
            for (int x = 0; x < drawn.width(); ++x) {
                if (qAbs(qRed(a[x]) - qRed(b[x])) + qAbs(qGreen(a[x]) - qGreen(b[x])) +
                        qAbs(qBlue(a[x]) - qBlue(b[x])) >
                    24) {
                    ++changed;
                }
            }
        }
        QVERIFY2(changed > 500, "测站图层没有画出内容");

        // screenX/screenY 带类型标注（`function screenX(lon: real): real`），
        // 编译后的元调用签名是 double(double)，不能再按 QVariant 传参。
        auto qmlNum = [&](const char* fn, double arg) -> double {
            double ret = qQNaN();
            const bool ok = QMetaObject::invokeMethod(mapView, fn, Q_RETURN_ARG(double, ret),
                                                      Q_ARG(double, arg));
            return ok ? ret : qQNaN();
        };
        auto aligned = [&]() -> bool {
            for (const QPointF& p : {QPointF(116.4, 39.9), QPointF(121.5, 31.2),
                                     QPointF(104.1, 30.6)}) {
                QPointF got;
                QMetaObject::invokeMethod(layer, "screenPosForTest", Q_RETURN_ARG(QPointF, got),
                                          Q_ARG(double, p.y()), Q_ARG(double, p.x()));
                const double expX = qmlNum("screenX", p.x());
                const double expY = qmlNum("screenY", p.y());
                if (!(std::hypot(got.x() - expX, got.y() - expY) < 1.0)) {
                    qWarning() << "station layer misaligned" << p << "got" << got << "expected"
                               << expX << expY;
                    return false;
                }
            }
            return true;
        };
        auto waitAligned = [&]() -> bool {
            for (int i = 0; i < 50; ++i) {
                QTest::qWait(20);
                window->grabWindow();
                if (aligned()) return true;
            }
            return false;
        };
        QVERIFY2(waitAligned(), "整数层级下测站层与 QML 投影错位");
        // 轻微缩放会走「缓存位图按比例贴回」分支；旧实现在这里会停在旧比例上。
        mapView->setProperty("zoom", 6.02);
        window->grabWindow();
        QVERIFY2(waitAligned(), "缓存缩放贴图后测站层与 QML 投影错位");
        mapView->setProperty("zoom", 7.3);
        mapView->setProperty("centerLon", 118.0);
        window->grabWindow();
        QVERIFY2(waitAligned(), "平移+缩放后测站层与 QML 投影错位");
    }

    /// 回归：测站悬停浮层必须真的能命中、显示站名，并且不拖慢渲染。
    ///
    /// 设计约束（改实现前先看这里）：
    /// - 上千个测站由 C++ 批量绘制，没有逐站 Item，所以浮层只能靠 stationAt() 命中；
    /// - 命中是 O(n) 纯算术（缓存的投影 Y），不重建任何东西，也不该触发 update()；
    /// - 浮层只有一个实例，相机一动就收起（否则锚点会与测站错位）。
    void stationHoverTooltipShowsDetails() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("StationTooltipTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        controller.settings()->setShowStationLayer(true);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* mapView = window->findChild<QQuickItem*>("mapView");
        auto* layer = window->findChild<QQuickItem*>("stationIntensityLayer");
        QVERIFY(mapView && layer);
        mapView->setProperty("centerLat", 30.0);
        mapView->setProperty("centerLon", 104.0);
        mapView->setProperty("zoom", 7.0);
        QTest::qWait(300);
        window->grabWindow();

        komira::StationSnapshot snapshot;
        layer->setProperty("snapshot", QVariant::fromValue(static_cast<QObject*>(&snapshot)));

        // 一眼能看到的三个站：成都（CSIS 方标）、日本（NIED 圆点）、韩国（KMA 圆点）。
        auto makeEntries = []() {
            std::vector<komira::StationSnapshot::Entry> entries;
            std::vector<komira::StationSnapshot::StationMeta> metas;
            auto add = [&](double lat, double lon, double raw, const char* name, const char* area,
                           const char* code, const char* type, const char* source, bool csis,
                           bool kma) {
                komira::StationSnapshot::Entry e;
                e.lat = lat; e.lon = lon; e.raw = raw; e.csis = csis; e.kma = kma;
                e.meta = static_cast<int>(entries.size());
                entries.push_back(e);
                komira::StationSnapshot::StationMeta m;
                m.name = QString::fromUtf8(name);
                m.area = QString::fromUtf8(area);
                m.code = QString::fromUtf8(code);
                m.type = QString::fromUtf8(type);
                m.source = QString::fromUtf8(source);
                metas.push_back(m);
            };
            add(30.67, 104.06, 6.0, "成都高新", "四川省成都市", "SC.CD01", "CSIS", "PancakesSeis",
                true, false);
            add(35.68, 139.76, 4.6, "東京", "東京都", "TKY01", "KiK-net", "NIED Kmoni", false, false);
            add(37.56, 126.97, 3.0, "", "", "", "", "KMA PEWS", false, true);
            return std::make_pair(entries, metas);
        };
        auto [entries, metas] = makeEntries();
        snapshot.setEntries(entries, metas);
        QTest::qWait(120);
        window->grabWindow();

        // 1. 命中：把相机里成都的位置交给 stationAt，必须命中并带出站名/代码/来源。
        QPointF chengdu;
        QVERIFY(QMetaObject::invokeMethod(layer, "stationScreenPos", Q_RETURN_ARG(QPointF, chengdu),
                                          Q_ARG(int, 0)));
        QVERIFY2(chengdu.x() > 0 && chengdu.x() < layer->width(), "测试站不在视野内");
        QVariantMap hit;
        QVERIFY(QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, hit),
                                          Q_ARG(double, chengdu.x()), Q_ARG(double, chengdu.y())));
        QVERIFY2(hit.value("found").toBool(), "指针落在测站上却没有命中");
        QCOMPARE(hit.value("index").toInt(), 0);
        QCOMPARE(hit.value("name").toString(), QStringLiteral("成都高新"));
        QCOMPARE(hit.value("area").toString(), QStringLiteral("四川省成都市"));
        QCOMPARE(hit.value("code").toString(), QStringLiteral("SC.CD01"));
        QCOMPARE(hit.value("source").toString(), QStringLiteral("PancakesSeis"));
        QCOMPARE(hit.value("standard").toString(), QStringLiteral("csis"));
        QCOMPARE(hit.value("intensity").toString(), QStringLiteral("6"));

        // 2. 未命中：离站点足够远处必须 found=false（不能把浮层挂在半空）。
        QVariantMap miss;
        QVERIFY(QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, miss),
                                          Q_ARG(double, chengdu.x() + 80),
                                          Q_ARG(double, chengdu.y() + 80)));
        QVERIFY(!miss.value("found").toBool());

        // 3. 各制式的显示文本：NIED 用 JMA 震度阶，KMA 取整 MMI。
        QVariantMap tokyo;
        QVERIFY(QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, tokyo),
                                          Q_ARG(double, chengdu.x() + 400),
                                          Q_ARG(double, chengdu.y())));
        QVERIFY(!tokyo.value("found").toBool() || tokyo.value("index").toInt() != 0);
        QPointF tokyoPos;
        QVERIFY(QMetaObject::invokeMethod(layer, "stationScreenPos", Q_RETURN_ARG(QPointF, tokyoPos),
                                          Q_ARG(int, 1)));
        QVERIFY(QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, tokyo),
                                          Q_ARG(double, tokyoPos.x()), Q_ARG(double, tokyoPos.y())));
        QVERIFY(tokyo.value("found").toBool());
        QCOMPARE(tokyo.value("standard").toString(), QStringLiteral("jma"));
        QCOMPARE(tokyo.value("intensity").toString(), QStringLiteral("5弱"));

        // 4. 命中查询不得触发整层重绘（这是「注意性能」的硬约束）。
        QSignalSpy frameSwapped(window, &QQuickWindow::frameSwapped);
        for (int i = 0; i < 500; ++i) {
            QVariantMap probe;
            QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, probe),
                                      Q_ARG(double, chengdu.x()), Q_ARG(double, chengdu.y()));
        }
        QTest::qWait(150);
        QVERIFY2(frameSwapped.count() <= 2,
                 qPrintable(QStringLiteral("悬停命中触发了持续重绘：%1 帧/150 ms")
                                .arg(frameSwapped.count())));

        // 5. 真实指针路径：停在地图上 → 90 ms 去抖后浮层出现并显示站名。
        auto* tooltip = findVisualItem(window, "stationTooltip");
        QVERIFY(tooltip);
        const QPoint scenePos =
            mapView->mapToScene(chengdu + QPointF(0, 0)).toPoint();
        QTest::mouseMove(window, QPoint(0, 0));
        QTest::qWait(30);
        QTest::mouseMove(window, scenePos);
        QTRY_VERIFY_WITH_TIMEOUT(tooltip->isVisible(), 1500);
        bool sawName = false;
        for (auto* item : visualItems(tooltip)) {
            if (item->property("text").toString() == QStringLiteral("成都高新")) sawName = true;
        }
        QVERIFY2(sawName, "浮层里没有站名");
        // 浮层与其他地图玻璃卡同一套材质：GPU 后端下必须真的挂上 BackdropBlur，
        // 且模糊源的 live 必须为 false（live 会让 SceneGraph 永不空闲）。
        // 软件后端按设计降级成不透明底衬，只断言没有实例化模糊。
        const bool software = window->rendererInterface()->graphicsApi()
                              == QSGRendererInterface::Software;
        // 只认**浮层自己**那棵子树里的模糊（tooltip 是 popup/visual 树之外的普通子项，
        // 但 visualItems(tooltip) 仍可能带上 Loader 的实例）。用 parent 链回溯到 tooltip。
        auto insideTooltip = [&](QQuickItem* item) {
            for (auto* p = item; p; p = p->parentItem()) {
                if (p == tooltip) return true;
            }
            return false;
        };
        QQuickItem* blurEffect = nullptr;
        for (auto* item : visualItems(tooltip)) {
            if (!insideTooltip(item)) continue;
            if (item->objectName() == QLatin1String("backdropEffect")) blurEffect = item;
        }
        QQuickItem* backdrop = nullptr;
        for (auto* item : visualItems(tooltip)) {
            if (item->objectName() == QLatin1String("backdropBlur")) backdrop = item;
        }
        if (software) {
            QVERIFY2(!blurEffect && !backdrop, "软件后端不应实例化模糊");
        } else {
            QVERIFY2(blurEffect && backdrop, "浮层缺少背景模糊");
            // 看 BackdropBlur.live（本组件传给模糊源的那个属性）。不能看 ShaderEffectSource
            // 自己的 live：MultiEffect 接管 source 后会把它改回自己的默认值 true，误判。
            QVERIFY2(!backdrop->property("live").toBool(),
                     "浮层的模糊源必须 live=false，否则会持续重绘地图");
            // 真实成本判据：浮层静止、相机静止时场景图必须能空闲（只允许 500 ms 低频补帧）。
            QSignalSpy idleFrames(window, &QQuickWindow::frameSwapped);
            QTest::qWait(1200);
            QVERIFY2(idleFrames.count() <= 8,
                     qPrintable(QStringLiteral("悬停浮层让场景图持续重绘：1200 ms 内 %1 帧")
                                    .arg(idleFrames.count())));
        }
        QTest::qWait(150);
        window->grabWindow().save(evidenceDirectory(window) + "/13-station-tooltip.png");

        // 5b. 悬停期间数据更新：浮层数值跟随快照刷新（震度 100 ms 级更新，
        //     停在旧值上会误导）。只做一次 O(n) 命中，不重建图层。
        {
            auto live = entries;
            live[0].raw = 9.0;
            snapshot.setEntries(live, metas);
            auto tooltipText = [&]() {
                QStringList texts;
                for (auto* item : visualItems(tooltip)) {
                    const QString t = item->property("text").toString();
                    if (!t.isEmpty()) texts << t;
                }
                return texts;
            };
            auto hasText = [&](const QString& want) {
                for (const QString& t : tooltipText()) if (t == want) return true;
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(hasText(QStringLiteral("9")), 1000);
        }

        // 5c. 关闭测站图层：浮层立刻收起（地图上已没有可指的点）。
        controller.settings()->setShowStationLayer(false);
        QTRY_VERIFY_WITH_TIMEOUT(!tooltip->isVisible(), 500);
        controller.settings()->setShowStationLayer(true);

        // 5d. 规模与预算：2500 站时一次命中必须远低于一帧（16 ms）。这是「注意性能」
        //     的硬指标，避免以后把命中改成重建投影或逐站 QObject。
        {
            std::vector<komira::StationSnapshot::Entry> many;
            std::vector<komira::StationSnapshot::StationMeta> manyMeta;
            many.reserve(2500);
            manyMeta.reserve(2500);
            for (int i = 0; i < 2500; ++i) {
                komira::StationSnapshot::Entry e;
                e.lat = 24.0 + (i % 50) * 0.25;
                e.lon = 100.0 + (i / 50) * 0.5;
                e.raw = double(i % 70) / 10.0 - 3.0;
                e.meta = i;
                many.push_back(e);
                komira::StationSnapshot::StationMeta m;
                m.name = QStringLiteral("站%1").arg(i);
                m.source = QStringLiteral("NIED Kmoni");
                manyMeta.push_back(m);
            }
            snapshot.setEntries(many, manyMeta);
            QTest::qWait(120);
            window->grabWindow();
            QElapsedTimer timer;
            timer.start();
            int hits = 0;
            for (int i = 0; i < 200; ++i) {
                QVariantMap probe;
                QMetaObject::invokeMethod(layer, "stationAt", Q_RETURN_ARG(QVariantMap, probe),
                                          Q_ARG(double, 300.0 + (i % 20) * 15.0),
                                          Q_ARG(double, 200.0 + (i % 13) * 20.0));
                if (probe.value("found").toBool()) ++hits;
            }
            const qint64 elapsedMs = timer.elapsed();
            qInfo() << "[station] 2500 站 × 200 次命中:" << elapsedMs << "ms, hits" << hits;
            QVERIFY2(elapsedMs < 200,
                     qPrintable(QStringLiteral("命中查询过慢：2500 站 × 200 次 = %1 ms")
                                    .arg(elapsedMs)));
        }

        // 6. 相机一动就收起：浮层锚在屏幕坐标上，跟着镜头漂会与测站错位。
        mapView->setProperty("centerLon", 103.0);
        QTRY_VERIFY_WITH_TIMEOUT(!tooltip->isVisible(), 500);
    }

    /// 本地震源推算（Beta）的可见性契约：
    /// - 卡片独占一张、挂在左列 HUD 之下（不与预警卡/测站卡共用）；
    /// - 倒计时卡在主 HUD **右侧**，且比主 HUD 窄（用户明确要求「弄小点」）；
    /// - 地图上的推定震中只在有解且开关打开时出现；
    /// - 关闭总开关后卡片与标记一起消失。
    void sourceEstimateCardLayoutAndMarker() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("SourceEstimateLayoutTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setBasemapId("vector");
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);
        // 震源推算默认关闭（Beta），本用例专门验证它，显式打开。
        // 逐区域开关同样默认关闭：本用例要验证「中国 + 日本同屏」，两个区都打开。
        // 中国区默认只对开发者模式开放（只有日本区在真实台网上标定过），故先开开发者模式。
        controller.settings()->setSourceEstimationEnabled(true);
        controller.settings()->setDeveloperMode(true);
        controller.settings()->setSourceEstimationRegionEnabled("china", true);
        controller.settings()->setSourceEstimationRegionEnabled("japan", true);
        controller.setManualLocation(35.0, 139.0, "测试定位");
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        const QString evidence = evidenceDirectory(window);
        auto screenshot = [&](const QString& name) {
            QTest::qWait(150);
            return window->grabWindow().save(evidence + "/" + name + ".png");
        };

        // 1. 无解时卡片完全不占位（否则地图上会凭空多一块空白玻璃）。
        auto* card = window->findChild<QQuickItem*>("sourceEstimateCard");
        auto* hud = window->findChild<QQuickItem*>("hudCard");
        auto* warningHud = window->findChild<QQuickItem*>("warningHud");
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(card && hud && warningHud && map);
        // 1b. 默认关闭：设置项默认 false，关闭时即使有解也不显示。
        QVERIFY(controller.settings()->sourceEstimationEnabled());

        // 2. 注入一条达到门槛的预警：倒计时卡必须出现在主 HUD 右侧且更窄。
        EarthquakeEvent alert;
        alert.id = alert.eventId = "EST-LAYOUT";
        alert.sourceProvider = "Wolfx"; alert.sourceAgency = "CENC"; alert.source = "四川地震预警";
        alert.timestamp = now; alert.latitude = 35.6; alert.longitude = 139.4;
        alert.depth = 10.0; alert.magnitude = 6.0; alert.location = "测试震中";
        alert.rawIntensity = 4.0;
        controller.handleEvent(alert, false, false);
        QTRY_VERIFY(hud->isVisible());
        QTRY_VERIFY(warningHud->isVisible());
        QVERIFY2(warningHud->x() >= hud->x() + hud->width() - 0.5,
                 qPrintable(QStringLiteral("倒计时卡应在主 HUD 右侧: hud 右缘 %1, 倒计时左缘 %2")
                                .arg(hud->x() + hud->width()).arg(warningHud->x())));
        QVERIFY2(warningHud->width() < hud->width(),
                 qPrintable(QStringLiteral("倒计时卡应比主 HUD 窄: %1 vs %2")
                                .arg(warningHud->width()).arg(hud->width())));
        QVERIFY(std::abs(warningHud->y() - hud->y()) < 1.0);   // 与 HUD 顶对齐
        QVERIFY(screenshot("40-countdown-beside-hud"));

        // 2b. 窄窗口放不下右侧位时退回 HUD 下方，且不得超出窗口右缘（宁可换位也不裁切）。
        window->resize(520, 760);
        QTest::qWait(200);
        QTRY_VERIFY2(!window->property("countdownBesideHud").toBool(), "窄窗口应退回左列");
        QTRY_VERIFY(std::abs(warningHud->x() - hud->x()) < 1.0);
        QVERIFY(warningHud->y() >= hud->y() + hud->height() - 0.5);
        QVERIFY(warningHud->x() + warningHud->width() <= window->width() - 8);
        QVERIFY(screenshot("44-countdown-narrow-fallback"));
        window->resize(1280, 800);
        QTest::qWait(200);
        QTRY_VERIFY(window->property("countdownBesideHud").toBool());

        // 3. 投喂一帧合成强度场：卡片出现在左列 HUD 之下，地图出现推定震中。
        std::vector<StationSnapshot::Entry> frame;
        std::vector<StationSnapshot::StationMeta> metas;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.csis = true;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                e.meta = static_cast<int>(frame.size());
                frame.push_back(e);
                StationSnapshot::StationMeta m;
                m.source = QStringLiteral("PancakesSeis");
                metas.push_back(m);
            }
        }
        controller.stationSnapshot_->setEntries(frame, metas);
        controller.sourceEstimation_->ingestObservations(frame);
        controller.sourceEstimation_->runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(controller.sourceEstimation_->regionState(SourceRegion::China).estimate.valid, 5000);
        if (controller.sourceEstimation_->watcher_.isRunning())
            controller.sourceEstimation_->watcher_.waitForFinished();
        QCoreApplication::processEvents();
        QTRY_VERIFY(card->isVisible());
        QVERIFY2(card->y() >= hud->y() + hud->height() - 0.5,
                 "震源推算卡必须挂在主 HUD 之下");
        QVERIFY2(std::abs(card->x() - hud->x()) < 1.0, "震源推算卡与 HUD 左对齐");
        QVERIFY2(std::abs(card->width() - hud->width()) < 1.0, "震源推算卡与 HUD 同宽");
        // 与倒计时卡不得重叠。倒计时卡贴在主 HUD 右侧、通常比 HUD 高，所以它不会压到
        // 下方的推算卡；两个方向都判一次，将来换布局也不会漏。
        {
            const QRectF cardRect(card->x(), card->y(), card->width(), card->height());
            const QRectF countdownRect(warningHud->x(), warningHud->y(),
                                       warningHud->width(), warningHud->height());
            QVERIFY2(!cardRect.intersects(countdownRect),
                     qPrintable(QStringLiteral("震源推算卡与倒计时卡重叠: 卡 %1,%2 %3x%4 / 倒计时 %5,%6 %7x%8")
                                    .arg(cardRect.x()).arg(cardRect.y())
                                    .arg(cardRect.width()).arg(cardRect.height())
                                    .arg(countdownRect.x()).arg(countdownRect.y())
                                    .arg(countdownRect.width()).arg(countdownRect.height())));
        }
        auto* marker = window->findChild<QQuickItem*>("sourceEstimateMarker");
        QVERIFY(marker);
        QVERIFY(marker->isVisible());
        // 卡片内容契约（改版后与普通地震 HUD 同构）：
        //   · 大号标题是震源区名（参考实现 getFEName 的 1° 网格：四川 → 「中国四川附近」）；
        //   · 顶行右侧是「第 N 报」，首报为 1；
        //   · 右下角写「精度 S/A/B/C/D」（取代普通 HUD 的数据源标注）；
        //   · 原来的收集提示、置信度百分比、±km、脚注一律不再出现。
        QTest::qWait(120);
        auto textOf = [&](const QString& name) -> QString {
            auto* item = window->findChild<QQuickItem*>(name);
            return item ? item->property("text").toString() : QString();
        };
        QCOMPARE(textOf("sourceEstimateLocation"), QStringLiteral("中国四川附近"));
        QCOMPARE(textOf("sourceEstimateReportNum"), QStringLiteral("第 1 报"));
        QVERIFY2(textOf("sourceEstimatePrecision").startsWith(QStringLiteral("精度 ")),
                 qPrintable(QStringLiteral("右下角精度文案异常: %1")
                                .arg(textOf("sourceEstimatePrecision"))));
        QVERIFY2(textOf("sourceEstimateMagnitude").startsWith(QStringLiteral("M ")),
                 qPrintable(QStringLiteral("震级/深度行异常: %1")
                                .arg(textOf("sourceEstimateMagnitude"))));
        // 反推的发震时刻必须写成完整时刻（与普通 HUD 同格式），不能只有 HH:mm:ss。
        QVERIFY2(textOf("sourceEstimateOrigin").contains(QStringLiteral("UTC+8"))
                     && textOf("sourceEstimateOrigin").length() >= 19,
                 qPrintable(QStringLiteral("推算卡发震时刻不完整: %1")
                                .arg(textOf("sourceEstimateOrigin"))));
        {
            QString offender;
            QVERIFY2(timesFullyShown(card, &offender),
                     qPrintable("推算卡发震时刻被截断: " + offender));
        }
        // 原有提示必须全部消失：遍历卡片下所有文本，不允许出现这些措辞。
        {
            const QStringList banned = {
                QStringLiteral("站已触发"), QStringLiteral("正在反推震源"),
                QStringLiteral("负证据"), QStringLiteral("合并半径"),
                QStringLiteral("单站粗解"), QStringLiteral("置信度"),
                QStringLiteral("另有"), QStringLiteral("s 前更新"),
            };
            for (auto* item : visualItems(card)) {
                const QString text = item->property("text").toString();
                if (text.isEmpty()) continue;
                for (const QString& word : banned) {
                    QVERIFY2(!text.contains(word),
                             qPrintable(QStringLiteral("推算卡仍残留旧提示「%1」: %2")
                                            .arg(word, text)));
                }
                QVERIFY2(!text.contains(QChar(0x00B1)),   // ±
                         qPrintable(QStringLiteral("推算卡仍显示不确定度: ") + text));
                QVERIFY2(!text.contains(QLatin1Char('%')),
                         qPrintable(QStringLiteral("推算卡仍显示置信度百分比: ") + text));
            }
        }
        // 只有中国区出解时没有分页器（分页是为多区域准备的）。
        {
            auto* pager = findVisualItem(window, "sourceEstimatePager");
            QVERIFY2(pager, "缺少推算卡分页器（多区域时用它翻页）");
            QVERIFY2(!pager->isVisible(), "只有一区出解时不应显示分页器");
        }
        // 另外三个区域的标记必须存在、但此刻没有解 → 不显示（逐区一个标记，互不覆盖）。
        for (const QString& regionId :
             {QStringLiteral("japan"), QStringLiteral("korea"), QStringLiteral("taiwan")}) {
            auto* other = window->findChild<QQuickItem*>("sourceEstimateMarker-" + regionId);
            QVERIFY2(other, qPrintable(QStringLiteral("缺少区域标记: ") + regionId));
            QVERIFY(!other->isVisible());
        }
        // 再投喂一片日本台网（JMA）：中国区的解必须保留，日本区另外多出一个标记。
        {
            std::vector<StationSnapshot::Entry> japanFrame;
            for (double lat = 34.9; lat <= 36.5 + 1e-9; lat += 0.2) {
                for (double lon = 138.9; lon <= 140.5 + 1e-9; lon += 0.2) {
                    StationSnapshot::Entry e;
                    e.lat = lat;
                    e.lon = lon;
                    e.raw = SourceEstimator::predictedIntensity(
                        IntensityFamily::Jma, 6.6,
                        QuakeCalculator::haversineDistance(35.7, 139.7, lat, lon), 10.0);
                    japanFrame.push_back(e);
                }
            }
            std::vector<StationSnapshot::Entry> merged = frame;
            merged.insert(merged.end(), japanFrame.begin(), japanFrame.end());
            // 起算有 1.2 s 的最小间隔：时钟是注入的常量，必须手动往前拨，否则会被节流掉。
            now += 3000;
            controller.sourceEstimation_->ingestObservations(merged);
            controller.sourceEstimation_->runEstimation();
            QTRY_VERIFY_WITH_TIMEOUT(
                controller.sourceEstimation_->regionState(SourceRegion::Japan).estimate.valid,
                5000);
            if (controller.sourceEstimation_->watcher_.isRunning())
                controller.sourceEstimation_->watcher_.waitForFinished();
            QCoreApplication::processEvents();
        }
        auto* japanMarker = window->findChild<QQuickItem*>("sourceEstimateMarker-japan");
        QVERIFY(japanMarker);
        QTRY_VERIFY(japanMarker->isVisible());
        // 日本区走到时反演：载荷必须带出 P/S 相位数与到时残差（卡片不再显示它们，
        // 但服务契约保留，供将来诊断面板使用）。
        {
            const QVariantList regions =
                controller.sourceEstimateInfo().value("regions").toList();
            QCOMPARE(regions.size(), 4);
            const QVariantMap japanRow = regions[1].toMap();
            QCOMPARE(japanRow.value("id").toString(), QStringLiteral("japan"));
            QCOMPARE(japanRow.value("usesArrivalTimes").toBool(), true);
            QVERIFY(!japanRow.value("originUtc8").toString().isEmpty());
            QVERIFY(japanRow.value("pPhases").toInt() > 0);
            QVERIFY(std::abs(japanRow.value("arrivalRmsSeconds").toDouble()) < 6.0);
            // 两区同时有解后卡片出现分页器，一区一页。
            QCOMPARE(japanRow.value("location").toString(),
                     QStringLiteral("日本本州南岸近海附近"));
        }
        {
            auto* pager = findVisualItem(window, "sourceEstimatePager");
            QVERIFY(pager);
            QTRY_VERIFY(pager->isVisible());
            auto* pagerText = findVisualItem(window, "sourceEstimatePagerText");
            QVERIFY(pagerText);
            QCOMPARE(pagerText->property("text").toString(), QStringLiteral("1 / 2"));
            // 翻页：按钮点击后页码与卡片内容一起换（一区一页，不堆叠）。
            QVERIFY(clickByName(window, "sourceEstimateNextPage"));
            QTRY_COMPARE(pagerText->property("text").toString(), QStringLiteral("2 / 2"));
            QTRY_COMPARE(textOf("sourceEstimateLocation"),
                         QStringLiteral("日本本州南岸近海附近"));
            QVERIFY(screenshot("41b-source-estimate-japan-page"));
            QVERIFY(clickByName(window, "sourceEstimatePrevPage"));
            QTRY_COMPARE(pagerText->property("text").toString(), QStringLiteral("1 / 2"));
            QTRY_COMPARE(textOf("sourceEstimateLocation"), QStringLiteral("中国四川附近"));
            // 载荷每秒刷新（tick → publish）时**不能把用户翻到的页顶回去**：
            // 区域集合没变就不该重排分页，否则刚翻到的日本区下一秒就跳回第 1 页。
            QVERIFY(clickByName(window, "sourceEstimateNextPage"));
            QTRY_COMPARE(pagerText->property("text").toString(), QStringLiteral("2 / 2"));
            now += 2000;
            controller.sourceEstimation_->tick();
            QCoreApplication::processEvents();
            QTest::qWait(120);
            QCOMPARE(pagerText->property("text").toString(), QStringLiteral("2 / 2"));
            QCOMPARE(textOf("sourceEstimateLocation"),
                     QStringLiteral("日本本州南岸近海附近"));
            QVERIFY(clickByName(window, "sourceEstimatePrevPage"));
            QTRY_COMPARE(pagerText->property("text").toString(), QStringLiteral("1 / 2"));
        }
        QVERIFY2(marker->isVisible(), "中国区的标记不能因为日本区出解而消失");
        QVERIFY(!window->findChild<QQuickItem*>("sourceEstimateMarker-korea")->isVisible());
        QVERIFY(screenshot("41-source-estimate"));
        // 深色主题下的对比度自查（浅色取证已覆盖默认态）。
        controller.settings()->setThemeMode("dark");
        QTest::qWait(120);
        QVERIFY(screenshot("45-source-estimate-dark"));
        controller.settings()->setThemeMode("system");
        QTest::qWait(80);

        // 4. 地图标记受独立显示开关控制；推算卡本身不受影响（两个区域的标记一起藏）。
        controller.settings()->setShowEstimatedEpicenter(false);
        QTRY_VERIFY(!marker->isVisible());
        QTRY_VERIFY(!japanMarker->isVisible());
        QVERIFY(card->isVisible());
        controller.settings()->setShowEstimatedEpicenter(true);
        QTRY_VERIFY(marker->isVisible());
        QTRY_VERIFY(japanMarker->isVisible());

        // 5. 关闭总开关：卡片与所有区域的标记一起消失（服务清空 + 卡片收进估算门控）。
        controller.settings()->setSourceEstimationEnabled(false);
        QTRY_VERIFY(!card->isVisible());
        QTRY_VERIFY(!marker->isVisible());
        QTRY_VERIFY(!japanMarker->isVisible());
        QVERIFY(screenshot("42-source-estimate-disabled"));

        // 5b. 默认关闭是产品契约：恢复默认后总开关必须回到 false。
        controller.settings()->resetToDefaults();
        QVERIFY(!controller.settings()->sourceEstimationEnabled());

        // 6. 设置页新增的「震源推算」分区可导航、控件可点击并即时落盘。
        //    此时总开关是默认的「关」，正好从关到开点一次。
        //    中国区卡只在开发者模式显示（默认开放的只有日本区），resetToDefaults()
        //    刚把开发者模式也清回了默认关，这里重新打开；完整的「开发者模式 → 三区显隐」
        //    验证见 sourceEstimationSettingsControlsTakeEffect()。
        controller.settings()->setDeveloperMode(true);
        window->setProperty("showSettings", true);
        QTRY_VERIFY(window->findChild<QQuickItem*>("settingsPage"));
        QVERIFY(clickByName(window, "settingsNav-sourceEstimation"));
        auto* page = window->findChild<QQuickItem*>("settingsPage");
        QTRY_COMPARE(page->property("currentSection").toInt(), 5);
        auto* panel = window->findChild<QQuickItem*>("settingsPanel-sourceEstimation");
        QVERIFY(panel && panel->isVisible());
        auto* toggle = window->findChild<QQuickItem*>("sourceEstimationSwitch");
        QVERIFY(toggle);
        QVERIFY(clickByName(window, "sourceEstimationSwitch"));
        QTRY_VERIFY(controller.settings()->sourceEstimationEnabled());
        // 常驻标签开关：默认开，点一次落盘为关。必须在关掉「地图标记」之前点 ——
        // 标记关掉后标签开关会跟着灰掉（标签无处可挂）。
        QVERIFY(controller.settings()->showEstimateLabel());
        QVERIFY(clickByName(window, "showEstimateLabelSwitch"));
        QTRY_VERIFY(!controller.settings()->showEstimateLabel());
        QVERIFY(clickByName(window, "showEstimatedEpicenterSwitch"));
        QTRY_VERIFY(!controller.settings()->showEstimatedEpicenter());

        // 逐区域开关：默认全关。中国区卡在视口内，用真实鼠标点击验证一次
        // 「点开关 → 只开该区，不连带其他三区」；日本区在视口外，只验证反向绑定
        // （设置变化 → 开关 checked 跟着变），避免在测试里模拟滚动到屏幕外的控件。
        QVERIFY(!controller.settings()->isSourceEstimationRegionEnabled("china"));
        QVERIFY(clickByName(window, "chinaRegionSwitch"));
        QTRY_VERIFY(controller.settings()->isSourceEstimationRegionEnabled("china"));
        QVERIFY(!controller.settings()->isSourceEstimationRegionEnabled("japan"));
        QVERIFY(!controller.settings()->isSourceEstimationRegionEnabled("korea"));
        QVERIFY(!controller.settings()->isSourceEstimationRegionEnabled("taiwan"));
        QCOMPARE(controller.settings()->enabledSourceEstimationRegions(),
                 QStringList{"china"});
        {
            // 反向绑定：设置变化必须让开关跟着动（这正是之前 `checked:` 字面绑定的 bug：
            // 点过一次后绑定被断开，UI 与真实状态脱节）。
            auto* chinaSwitch = findVisualItem(window, "chinaRegionSwitch");
            QVERIFY(chinaSwitch);
            QVERIFY(chinaSwitch->property("checked").toBool());
            controller.settings()->setSourceEstimationRegionEnabled("china", false);
            QTRY_VERIFY(!chinaSwitch->property("checked").toBool());
            controller.settings()->setSourceEstimationRegionEnabled("china", true);
            QTRY_VERIFY(chinaSwitch->property("checked").toBool());
            controller.settings()->setSourceEstimationRegionEnabled("china", false);
            QTRY_VERIFY(!chinaSwitch->property("checked").toBool());
        }
        QVERIFY(screenshot("43-settings-source-estimation"));
        // 「触发区合并距离」在分区下半部：滚到它再取一张图，确认控件本身渲染正常。
        {
            auto* scroll = window->findChild<QQuickItem*>("settingsScroll");
            QVERIFY(scroll);
            scroll->setProperty("contentY", 420);
            QTest::qWait(120);
            QVERIFY(screenshot("43b-settings-cluster-radius"));
            scroll->setProperty("contentY", 0);
        }
        controller.settings()->resetToDefaults();
        QVERIFY(!controller.settings()->sourceEstimationEnabled());
        QVERIFY(controller.settings()->showEstimatedEpicenter());
        // 标签默认开：恢复默认后必须和标记一样回到「显示」。
        QVERIFY(controller.settings()->showEstimateLabel());
    }

    /// 推定震中的常驻标签：文字必须贴着标记、贴边不裁切、两区同屏时不互相压住。
    ///
    /// 对齐 kanameishi 的「推定震源」浮层（参考/kanameishi 的 NiedNet.vue，
    /// layoutInferredHypocenterLabels）：标签不是固定挂在标记下方 ——
    /// 区域解常常相邻（中国东部 / 韩国 / 台湾），固定朝下会叠字；靠近视口边缘时
    /// 还会被地图的 clip 裁掉。本用例分两层：
    ///   · 纯几何：直接喂点给 `placeEstimateLabels()`，验证换边、视口夹取、
    ///     视口外的震中不挂标签（不需要真的造两次地震）；
    ///   · 集成：真实出解后标签真的渲染在标记旁，文本带精度/震级/深度/站数，
    ///     设置开关一关就消失而标记仍在。
    void estimateLabelsSitBesideMarkers() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EstimateLabelLayoutTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setBasemapId("vector");
        controller.settings()->setSourceEstimationEnabled(true);
        // 中国区默认只对开发者模式开放（只有日本区在真实台网上标定过）。
        controller.settings()->setDeveloperMode(true);
        controller.settings()->setSourceEstimationRegionEnabled("china", true);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map);

        // ---- 1. 纯几何：两个几乎重合的标签必须换边，不能叠在一起 ----
        auto layoutFor = [&](const QVariantList& points, double vw, double vh) {
            QVariant out;
            const bool ok = QMetaObject::invokeMethod(map, "placeEstimateLabels",
                                                      Q_RETURN_ARG(QVariant, out),
                                                      Q_ARG(QVariant, points),
                                                      Q_ARG(QVariant, vw),
                                                      Q_ARG(QVariant, vh));
            return ok ? out.toMap() : QVariantMap();
        };
        auto labelPoint = [](const QString& id, double cx, double cy, double confidence) {
            QVariantMap p;
            p["id"] = id;
            p["cx"] = cx;
            p["cy"] = cy;
            p["w"] = 176.0;
            p["h"] = 30.0;
            p["confidence"] = confidence;
            return QVariant(p);
        };
        {
            QVariantList points;
            points << labelPoint("a", 400, 300, 0.9) << labelPoint("b", 420, 320, 0.5);
            const QVariantMap layout = layoutFor(points, 800, 600);
            QVERIFY2(layout.contains("a") && layout.contains("b"), "两个标签都必须有位置");
            const QVariantMap a = layout.value("a").toMap();
            const QVariantMap b = layout.value("b").toMap();
            // 置信度高的先挑：第一个候选就是「下方」，偏移等于 gap（24）。
            QCOMPARE(a.value("dy").toDouble(), 24.0);
            const QRectF ra(400 + a.value("dx").toDouble(), 300 + a.value("dy").toDouble(),
                            176, 30);
            const QRectF rb(420 + b.value("dx").toDouble(), 320 + b.value("dy").toDouble(),
                            176, 30);
            QVERIFY2(!ra.intersects(rb),
                     qPrintable(QStringLiteral("两个推定震中标签重叠: a=%1,%2 b=%3,%4")
                                    .arg(ra.x()).arg(ra.y()).arg(rb.x()).arg(rb.y())));
            // 第二个必须离开「下方」（会被 a 压住）。
            QVERIFY2(b.value("dy").toDouble() != 24.0, "第二枚标签未换边");
        }
        // ---- 2. 贴边：夹进视口，不允许伸出边缘（伸出部分会被 clip 裁掉） ----
        {
            QVariantList points;
            points << labelPoint("edge", 5, 5, 0.9);
            const QVariantMap layout = layoutFor(points, 800, 600);
            QVERIFY(layout.contains("edge"));
            const QVariantMap edge = layout.value("edge").toMap();
            const double left = 5 + edge.value("dx").toDouble();
            const double top = 5 + edge.value("dy").toDouble();
            QVERIFY2(left >= 7.5 && left + 176 <= 792.5,
                     qPrintable(QStringLiteral("贴边标签水平越界: left=%1").arg(left)));
            QVERIFY2(top >= 7.5 && top + 30 <= 592.5,
                     qPrintable(QStringLiteral("贴边标签垂直越界: top=%1").arg(top)));
        }
        // ---- 3. 视口外的震中不挂标签（夹到边缘会变成与震中脱节的浮字） ----
        {
            QVariantList points;
            points << labelPoint("outside", -80, 300, 0.9);
            QVERIFY(!layoutFor(points, 800, 600).contains("outside"));
        }

        // ---- 4. 集成：真实出解后标签渲染在标记旁 ----
        std::vector<StationSnapshot::Entry> frame;
        std::vector<StationSnapshot::StationMeta> metas;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry e;
                e.lat = lat;
                e.lon = lon;
                e.csis = true;
                e.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                e.meta = static_cast<int>(frame.size());
                frame.push_back(e);
                StationSnapshot::StationMeta m;
                m.source = QStringLiteral("PancakesSeis");
                metas.push_back(m);
            }
        }
        controller.stationSnapshot_->setEntries(frame, metas);
        controller.sourceEstimation_->ingestObservations(frame);
        controller.sourceEstimation_->runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.sourceEstimation_->regionState(SourceRegion::China).estimate.valid, 5000);
        if (controller.sourceEstimation_->watcher_.isRunning())
            controller.sourceEstimation_->watcher_.waitForFinished();
        QCoreApplication::processEvents();
        // 标签只给视口内的震中挂：把镜头挪到推定震中上。
        QVERIFY(QMetaObject::invokeMethod(map, "moveCamera",
                                          Q_ARG(QVariant, 30.0), Q_ARG(QVariant, 104.0),
                                          Q_ARG(QVariant, 7.0), Q_ARG(QVariant, 0.0),
                                          Q_ARG(QVariant, QStringLiteral("flight"))));
        QTest::qWait(250);

        auto* marker = window->findChild<QQuickItem*>("sourceEstimateMarker");
        auto* label = window->findChild<QQuickItem*>("sourceEstimateLabel-china");
        auto* title = window->findChild<QQuickItem*>("sourceEstimateLabelTitle-china");
        auto* meta = window->findChild<QQuickItem*>("sourceEstimateLabelMeta-china");
        QVERIFY2(marker && label && title && meta, "缺少常驻标签");
        QTRY_VERIFY2(label->isVisible(), "推定震中旁没有常驻标签");
        // 文本契约：首行「区域 · 精度 X · 第 N 报」，次行「M x.x · 深 xx km · N 站」。
        QVERIFY2(title->property("text").toString().contains(QStringLiteral("精度 ")),
                 qPrintable(QStringLiteral("标签首行缺少精度: ")
                                + title->property("text").toString()));
        QVERIFY2(title->property("text").toString().contains(QStringLiteral("报")),
                 qPrintable(QStringLiteral("标签首行缺少报数: ")
                                + title->property("text").toString()));
        const QString metaText = meta->property("text").toString();
        QVERIFY2(metaText.contains(QStringLiteral("M ")) && metaText.contains(QStringLiteral("深 ")),
                 qPrintable(QStringLiteral("标签次行缺少震级/深度: ") + metaText));
        QVERIFY2(metaText.contains(QStringLiteral("站")),
                 qPrintable(QStringLiteral("标签次行缺少站数: ") + metaText));

        // 几何契约：标签完整落在视口内，且不压住震中标记本身。
        const QPointF labelPos = label->mapToItem(map, QPointF(0, 0));
        const QRectF labelRect(labelPos, QSizeF(label->width(), label->height()));
        QVERIFY2(labelRect.left() >= 7.5 && labelRect.right() <= map->width() - 7.5,
                 qPrintable(QStringLiteral("标签水平越界: %1..%2 / 视口 %3")
                                .arg(labelRect.left()).arg(labelRect.right()).arg(map->width())));
        QVERIFY2(labelRect.top() >= 7.5 && labelRect.bottom() <= map->height() - 7.5,
                 qPrintable(QStringLiteral("标签垂直越界: %1..%2 / 视口 %3")
                                .arg(labelRect.top()).arg(labelRect.bottom()).arg(map->height())));
        const QPointF markerCenter = marker->mapToItem(
            map, QPointF(marker->width() / 2, marker->height() / 2));
        const QRectF markerBox(markerCenter - QPointF(9, 9), QSizeF(18, 18));
        QVERIFY2(!labelRect.intersects(markerBox), "标签压住了震中标记");

        // 开关：关掉标签后标记必须还在（两个开关互不连带）。
        controller.settings()->setShowEstimateLabel(false);
        QTRY_VERIFY(!label->isVisible());
        QVERIFY(marker->isVisible());
        controller.settings()->setShowEstimateLabel(true);
        QTRY_VERIFY(label->isVisible());

        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window) + "/48-estimate-label.png"));
    }

    /// 回归：左上角 HUD 的翻页按钮必须能翻到**每一条**活动事件，页码跟着走。
    ///
    /// 用户报的现象：「切换按钮只能切换两个，显示还不变化，一直是 1/3」。
    /// 根因在 `shiftHud()` 末尾无条件调用 `syncWarning()`：后者从「当前选择」出发，
    /// 只要会话里有机构优先级更高的事件就把它顶回去。于是翻页改的是 `mapFocus_`
    /// （HUD 内容跟着换），而 `selectedWarning_`（页码来源）瞬间被改回最高优先级那条 ——
    /// 页码卡在 1/N，内容只在「最优那条」和「第一次翻到的那条」之间来回。
    /// 这里用 1 条 JMA + 2 条 GQ 固定复现：JMA 永远比 GQ 更优。
    void hudPagerCyclesAllActiveEvents() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("HudPagerTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);

        auto make = [&](const QString& id, const QString& agency, double lon) {
            EarthquakeEvent e;
            e.id = e.eventId = id.toStdString();
            e.sourceProvider = "Wolfx";
            e.sourceAgency = agency.toStdString();
            e.source = agency == QStringLiteral("JMA") ? "JMA 紧急地震速报" : "GQ 地震信息";
            e.timestamp = e.reportTime = now;
            e.latitude = 35.0;
            e.longitude = lon;
            e.depth = 10.0;
            e.magnitude = 5.0;
            e.location = id.toStdString();
            return e;
        };
        controller.handleEvent(make(QStringLiteral("HUD-JMA"), QStringLiteral("JMA"), 140.0),
                               false, false);
        controller.handleEvent(make(QStringLiteral("HUD-GQ-1"), QStringLiteral("GQ"), 104.0),
                               false, false);
        controller.handleEvent(make(QStringLiteral("HUD-GQ-2"), QStringLiteral("GQ"), 105.0),
                               false, false);
        QCOMPARE(controller.hudCount(), 3);
        // 优先级最高的（JMA）排在第一页。
        QCOMPARE(controller.hudIndex(), 0);

        // 向后翻满一圈：1 → 2 → 3 → 1。每一页都必须真的到位。
        for (int step = 1; step <= 3; ++step) {
            controller.hudNext();
            QCOMPARE(controller.hudIndex(), step % 3);
        }
        // 向前翻也要按环形走。
        controller.hudPrev();
        QCOMPARE(controller.hudIndex(), 2);
        controller.hudPrev();
        QCOMPARE(controller.hudIndex(), 1);
    }

    /// 回归：震源推算设置页的开关与滑块必须**真的**生效。
    ///
    /// 用户报的两个现象：①「开关点按有反应，但没作用 —— 打开开关，下面滑动条依然是灰的；
    /// 拖动滑动条，数值不变」；②「中国 / 韩国 / 台湾区域只有开发者模式才显示」。
    ///
    /// ① 有两个独立根因，本用例各钉一条：
    ///   · **依赖**：逐区数值原先直接绑在 Q_INVOKABLE 上（`sourceEstimationTriggerFor(region)`
    ///     等），QML 只跟踪属性读取、不跟踪函数调用，函数返回值变了绑定也不会重算 ——
    ///     于是「开关点开了，滑块还是灰的、数值文本停在旧值」。现在统一读
    ///     `sourceEstimationRegionParams` 快照（Q_PROPERTY）。
    ///   · **破坏性绑定**：`SettingSlider` 里 `value: root.value` 是字面绑定，用户拖动会向
    ///     滑块写值、从而断开它，之后设置再变滑块也不动。改用
    ///     `Binding { restoreMode: RestoreBindingOrValue }`。
    ///   另外 `Slider.moved()` 是**无参**信号，原先的 `onMoved: value => …` 里那个参数名
    ///   遮蔽了 Slider 自己的 `value` 属性。
    ///
    /// ② 由 `SettingsStore::isSourceEstimationRegionAvailable` 统一判定（服务层同一判据）：
    /// 只有日本区在真实台网上标定过，其余三区默认不开放、开发者模式才放出来。
    void sourceEstimationSettingsControlsTakeEffect() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("SourceEstimationSettingsTest");
        AppController controller(nullptr, false);
        controller.settings()->setEnableSoundAlert(false);
        QQmlApplicationEngine engine;
        QStringList qmlWarnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) qmlWarnings.append(warning.toString());
        });
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        const QString evidence = evidenceDirectory(window);
        auto screenshot = [&](const QString& name) {
            QTest::qWait(150);
            return window->grabWindow().save(evidence + "/" + name + ".png");
        };
        window->setProperty("showSettings", true);
        auto* page = window->findChild<QQuickItem*>("settingsPage");
        QVERIFY(page);
        QTRY_VERIFY(clickByName(window, "settingsNav-sourceEstimation"));
        QTRY_COMPARE(page->property("currentSection").toInt(), 5);

        auto* scroll = window->findChild<QQuickItem*>("settingsScroll");
        QVERIFY(scroll);
        // 把控件滚到视口内再操作：命中判定与用户视角一致（裁剪之外的点不点）。
        const auto scrollTo = [&](QQuickItem* item) {
            QVERIFY(item);
            const QPointF inFlick =
                item->mapToItem(scroll, QPointF(item->width() / 2, item->height() / 2));
            const qreal desired = scroll->height() * 0.45;
            scroll->setProperty("contentY",
                                scroll->property("contentY").toReal() + inFlick.y() - desired);
            QTest::qWait(120);
        };

        // ---- ② 默认只显示日本区，其余三区要开发者模式 ----
        auto* chinaCard = findVisualItem(window, "chinaRegionCard");
        auto* japanCard = findVisualItem(window, "japanRegionCard");
        auto* koreaCard = findVisualItem(window, "koreaRegionCard");
        auto* taiwanCard = findVisualItem(window, "taiwanRegionCard");
        QVERIFY(chinaCard && japanCard && koreaCard && taiwanCard);
        QVERIFY(!controller.settings()->developerMode());
        QVERIFY2(!chinaCard->isVisible(), "中国区默认不该出现");
        QVERIFY2(!koreaCard->isVisible(), "韩国区默认不该出现");
        QVERIFY2(!taiwanCard->isVisible(), "台湾区默认不该出现");
        QVERIFY2(japanCard->isVisible(), "日本区必须始终可见");
        // 快照里同样要标出「不开放」：服务层与 UI 用同一份判据。
        {
            const QVariantMap params =
                controller.settings()->sourceEstimationRegionParams();
            QCOMPARE(params.value("china").toMap().value("available").toBool(), false);
            QCOMPARE(params.value("korea").toMap().value("available").toBool(), false);
            QCOMPARE(params.value("taiwan").toMap().value("available").toBool(), false);
            QCOMPARE(params.value("japan").toMap().value("available").toBool(), true);
        }
        QVERIFY(screenshot("43c-settings-source-estimation-default-regions"));

        // 打开开发者模式（走真实开关：它自己也是「点按后绑定断掉」的同类隐患）。
        QVERIFY(clickByName(window, "settingsNav-about"));
        QTRY_COMPARE(page->property("currentSection").toInt(), 8);
        auto* devSwitch = findVisualItem(window, "devModeSwitch");
        QVERIFY(devSwitch);
        scrollTo(devSwitch);
        QVERIFY(clickByName(window, "devModeSwitch"));
        QTRY_VERIFY(controller.settings()->developerMode());
        QVERIFY(devSwitch->property("checked").toBool());
        QVERIFY(clickByName(window, "settingsNav-sourceEstimation"));
        QTRY_COMPARE(page->property("currentSection").toInt(), 5);
        QTRY_VERIFY(chinaCard->isVisible());
        QTRY_VERIFY(koreaCard->isVisible());
        QTRY_VERIFY(taiwanCard->isVisible());
        QVERIFY(screenshot("43d-settings-source-estimation-dev-regions"));

        // ---- ① 开关与滑块 ----
        // 总开关：开着的时候，没启用的区域开关仍然要能点（它自己不受本区开关门控）。
        auto* masterSwitch = findVisualItem(window, "sourceEstimationSwitch");
        QVERIFY(masterSwitch);
        scrollTo(masterSwitch);
        QVERIFY(!controller.settings()->sourceEstimationEnabled());
        QVERIFY(clickByName(window, "sourceEstimationSwitch"));
        QTRY_VERIFY(controller.settings()->sourceEstimationEnabled());
        QVERIFY(masterSwitch->property("checked").toBool());

        auto* triggerControl = findVisualItem(window, "chinaTriggerSlider-control");
        auto* triggerValue = findVisualItem(window, "chinaTriggerSlider-value");
        QVERIFY(triggerControl && triggerValue);
        // 本区还没启用：滑块必须是灰的（`active` 叠加了本区开关）。
        QVERIFY2(!triggerControl->isEnabled(), "未启用的区域，参数控件必须是灰的");
        QCOMPARE(triggerValue->property("text").toString(), QStringLiteral("3.0 度"));

        auto* chinaSwitch = findVisualItem(window, "chinaRegionSwitch");
        QVERIFY(chinaSwitch);
        scrollTo(chinaSwitch);
        QVERIFY(clickByName(window, "chinaRegionSwitch"));
        QTRY_VERIFY(controller.settings()->isSourceEstimationRegionEnabled("china"));
        // 这正是用户报的「打开开关，下面滑动条依然是灰色」：开关一开，整卡必须立刻可用。
        QTRY_VERIFY2(triggerControl->isEnabled(), "区域开关打开后，参数控件必须立刻可用");
        QVERIFY(chinaCard->property("opacity").toReal() > 0.99);

        // 拖动滑块：设置必须跟着变，且数值文本跟着刷新。
        const auto dragTrigger = [&](qreal fraction) {
            auto* handle = triggerControl->property("handle").value<QQuickItem*>();
            QVERIFY(handle);
            const QPointF from = handle->mapToScene(QPointF(handle->width() / 2,
                                                            handle->height() / 2));
            const QPointF to = from + QPointF(triggerControl->width() * fraction, 0);
            const QRectF bounds(0, 0, window->width(), window->height());
            QVERIFY2(bounds.contains(from) && bounds.contains(to), "滑块必须完整落在视口内");
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            QTest::mouseMove(window, to.toPoint());
            QTest::qWait(30);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
        };
        const double before = controller.settings()->sourceEstimationTriggerFor("china");
        scrollTo(triggerControl);
        dragTrigger(0.35);
        QTRY_VERIFY(controller.settings()->sourceEstimationTriggerFor("china") > before);
        const double after = controller.settings()->sourceEstimationTriggerFor("china");
        QCOMPARE(triggerValue->property("text").toString(),
                 QString::number(after, 'f', 1) + QStringLiteral(" 度"));
        // 滑块自己的值也要回到设置项上（Binding 而不是被拖断的字面绑定）。
        QTRY_COMPARE(triggerControl->property("value").toDouble(), after);
        // 反向：改设置（例如「恢复本区默认参数」）必须把滑块拉回去。
        QVERIFY(clickByName(window, "chinaResetButton"));
        QTRY_COMPARE(controller.settings()->sourceEstimationTriggerFor("china"), 3.0);
        QTRY_COMPARE(triggerControl->property("value").toDouble(), 3.0);
        QTRY_COMPARE(triggerValue->property("text").toString(), QStringLiteral("3.0 度"));

        // 其余两个滑块同样要能拖动（「最少触发测站」「触发区合并距离」）。
        auto* minControl = findVisualItem(window, "chinaMinStationsSlider-control");
        auto* radiusControl = findVisualItem(window, "chinaClusterRadiusSlider-control");
        QVERIFY(minControl && radiusControl);
        scrollTo(minControl);
        QVERIFY(minControl->isEnabled());
        {
            const QPointF from = minControl->mapToScene(QPointF(minControl->width() / 2,
                                                               minControl->height() / 2));
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            QTest::mouseMove(window, (from + QPointF(minControl->width() * 0.3, 0)).toPoint());
            QTest::qWait(30);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                (from + QPointF(minControl->width() * 0.3, 0)).toPoint());
        }
        QTRY_VERIFY(controller.settings()->sourceEstimationMinStationsFor("china") != 6);
        scrollTo(radiusControl);
        QVERIFY(radiusControl->isEnabled());
        {
            const QPointF from = radiusControl->mapToScene(QPointF(radiusControl->width() / 2,
                                                                   radiusControl->height() / 2));
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            QTest::mouseMove(window, (from + QPointF(radiusControl->width() * 0.3, 0)).toPoint());
            QTest::qWait(30);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                (from + QPointF(radiusControl->width() * 0.3, 0)).toPoint());
        }
        QTRY_VERIFY(controller.settings()->sourceEstimationClusterRadiusFor("china") != 250);

        // 「结果保留时长」滑块只受总开关门控（与区域无关），也要能动。
        auto* holdControl = findVisualItem(window, "holdSecondsSlider-control");
        QVERIFY(holdControl);
        scrollTo(holdControl);
        QVERIFY(holdControl->isEnabled());
        {
            const QPointF from = holdControl->mapToScene(QPointF(holdControl->width() / 2,
                                                                 holdControl->height() / 2));
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            QTest::mouseMove(window, (from + QPointF(holdControl->width() * 0.25, 0)).toPoint());
            QTest::qWait(30);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                (from + QPointF(holdControl->width() * 0.25, 0)).toPoint());
        }
        QTRY_VERIFY(controller.settings()->sourceEstimationHoldSeconds() != 60);
        QVERIFY(screenshot("43e-settings-source-estimation-sliders"));

        // 关掉总开关：整卡参数控件一起变灰（依赖链要真的传下去）。
        scrollTo(masterSwitch);
        QVERIFY(clickByName(window, "sourceEstimationSwitch"));
        QTRY_VERIFY(!controller.settings()->sourceEstimationEnabled());
        QTRY_VERIFY(!triggerControl->isEnabled());

        // 退出开发者模式：中国 / 韩国 / 台湾三张卡必须重新隐藏，且**不再生效**。
        QVERIFY(clickByName(window, "settingsNav-about"));
        QTRY_COMPARE(page->property("currentSection").toInt(), 8);
        scrollTo(devSwitch);
        QVERIFY(clickByName(window, "devModeSwitch"));
        QTRY_VERIFY(!controller.settings()->developerMode());
        QVERIFY(clickByName(window, "settingsNav-sourceEstimation"));
        QTRY_COMPARE(page->property("currentSection").toInt(), 5);
        QTRY_VERIFY(!chinaCard->isVisible());
        QTRY_VERIFY(!koreaCard->isVisible());
        QTRY_VERIFY(!taiwanCard->isVisible());
        QVERIFY(japanCard->isVisible());
        // 可用性变化必须让服务层也停掉那三区：签名只含生效区域。
        QCOMPARE(controller.settings()->sourceEstimationRegionSignature(), QString());
        QVERIFY(!controller.settings()->sourceEstimationRegionActive());
        controller.settings()->setSourceEstimationRegionEnabled("japan", true);
        QCOMPARE(controller.settings()->sourceEstimationRegionSignature(),
                 QStringLiteral("japan"));
        QVERIFY(controller.settings()->sourceEstimationRegionActive());
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
    }

    /// 回归：CSIS/KMA 方标里的烈度数字必须画在**视觉中心**。
    ///
    /// 旧实现用 `drawText(rect, Qt::AlignCenter)`，居中的是字体行盒（ascent + descent）；
    /// 数字墨迹并不占满行盒，1.3 缩放下 24 px 方标实测偏右下约 1 px，看起来就是「数字不居中」。
    /// 现在按字形 outline 的墨迹外框对齐，并把图集按设备像素排版（1.25/1.5 这类分数缩放
    /// 不再二次采样）。这里对 13 档烈度 × 3 组 (zoom, dpr) 断言墨迹中心与图块中心的偏差。
    void stationMarkerDigitsAreOpticallyCentered() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("StationMarkerCenterTest");
        AppController controller(nullptr, false);
        controller.settings()->setBasemapId("vector");
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        auto* layer = window->findChild<komira::StationMapItem*>("stationIntensityLayer");
        QVERIFY(layer);

        auto measure = [&](int level, double zoom, double dpr, double* dx, double* dy) -> bool {
            QImage tile;
            const bool ok = QMetaObject::invokeMethod(
                layer, "markerTileForTest", Q_RETURN_ARG(QImage, tile), Q_ARG(int, level),
                Q_ARG(double, zoom), Q_ARG(double, dpr));
            if (!ok || tile.isNull() || tile.width() < 8) return false;
            // 底色 = 图块里出现次数最多的颜色（方标本体占绝大多数像素），比取边角
            // 像素稳：边角落在描边上时会误判明暗，进而找错方向的墨迹。
            QHash<QRgb, int> histogram;
            for (int y = 0; y < tile.height(); ++y)
                for (int x = 0; x < tile.width(); ++x)
                    ++histogram[tile.pixel(x, y)];
            QRgb dominant = 0;
            int best = 0;
            for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
                if (it.value() > best) { best = it.value(); dominant = it.key(); }
            }
            const QColor fill = QColor::fromRgb(dominant);
            const double fillLum = 0.2126 * fill.redF() + 0.7152 * fill.greenF() +
                                   0.0722 * fill.blueF();
            const bool wantBright = fillLum < 0.5;
            int x0 = tile.width(), x1 = -1, y0 = tile.height(), y1 = -1;
            const double cx = (tile.width() - 1) / 2.0, cy = (tile.height() - 1) / 2.0;
            const double limit = tile.width() * 0.34;
            for (int y = 0; y < tile.height(); ++y) {
                for (int x = 0; x < tile.width(); ++x) {
                    if (std::hypot(x - cx, y - cy) > limit) continue;
                    const QColor c = tile.pixelColor(x, y);
                    const double lum = 0.2126 * c.redF() + 0.7152 * c.greenF() +
                                       0.0722 * c.blueF();
                    if (wantBright ? lum > 0.94 : lum < 0.18) {
                        x0 = qMin(x0, x); x1 = qMax(x1, x);
                        y0 = qMin(y0, y); y1 = qMax(y1, y);
                    }
                }
            }
            if (x1 < 0) return false;
            // 图块中心用**连续坐标** width/2（像素中心在 x+0.5），与绘制端一致；
            // 用 (width-1)/2 会凭空引入 +0.5 px 的常量偏差。
            *dx = (x0 + x1 + 1) / 2.0 - tile.width() / 2.0;
            *dy = (y0 + y1 + 1) / 2.0 - tile.height() / 2.0;
            return true;
        };

        for (const auto& cfg : {std::pair<double, double>{6.0, 1.0}, std::pair<double, double>{9.0, 1.3},
                                std::pair<double, double>{12.0, 1.5}}) {
            const double zoom = cfg.first;
            const double dpr = cfg.second;
            int measured = 0;
            for (int level = 0; level <= 12; ++level) {
                double dx = 0, dy = 0;
                if (!measure(level, zoom, dpr, &dx, &dy)) continue;   // 该档字形太细，跳过
                ++measured;
                // 容差按设备像素给：允许四舍五入带来的半个像素 + 抗锯齿的半个像素。
                const double tol = std::max(1.0, 0.75 * dpr);
                QVERIFY2(std::hypot(dx, dy) <= tol,
                         qPrintable(QStringLiteral("烈度 %1 的数字没居中：zoom %2 dpr %3 偏差 (%4, %5) px")
                                        .arg(level).arg(zoom).arg(dpr).arg(dx, 0, 'f', 2)
                                        .arg(dy, 0, 'f', 2)));
            }
            QVERIFY2(measured >= 10, "图集里可测量的方标太少，测试没有真正生效");
        }
    }

    /// 回归：波前淡出后「跟随震中」按钮仍然生效。
    ///
    /// 曾经 `frameEvent` 在 `hasWaves && faded` 时直接 return —— 而 faded 自 P 波超过 CSIS
    /// 可感半径（opacity 降到 0.25）后**一直为真**。于是按钮只把 following 置真、镜头纹丝
    /// 不动，用户看到的就是「按钮失效」。显式聚焦（toggleFollowEvent -> focusEvent）现在
    /// 不受该冻结限制（该冻结只用于自动逐帧跟随）。
    void followButtonReframesWhenWavesFaded() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("FollowFadedTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map);
        QTest::qWait(400);

        EarthquakeEvent e;
        e.id = e.eventId = "FOLLOW-A";
        e.timestamp = now;
        e.latitude = 30.6;
        e.longitude = 104.0;
        e.magnitude = 5.0;
        e.depth = 10.0;
        e.rawIntensity = 3.0;
        controller.handleEvent(e, false, false);
        QTest::qWait(700);   // 等入场取景补间结束
        QVERIFY(map->property("hasEvent").toBool());

        // 推进到波前已淡出的时刻：pOpacity 落到 0.25（<=0.25 即 faded）。
        now += 60'000;
        controller.updateWaveRadii();
        const double wavePOpacity = controller.property("wavePOpacity").toDouble();
        QVERIFY2(wavePOpacity <= 0.25,
                 qPrintable(QStringLiteral("前置条件失败：波前未淡出 opacity=%1")
                                .arg(wavePOpacity)));

        // 用户手动拖远并退出跟随。
        map->setProperty("following", false);
        map->setProperty("centerLon", 120.0);
        map->setProperty("centerLat", 20.0);
        QTest::qWait(60);
        QVERIFY(!map->property("following").toBool());
        QVERIFY(std::abs(map->property("centerLon").toDouble() - 120.0) < 1.0);

        // 点击「跟随震中」按钮对应 toggleFollowEvent -> focusEvent。
        QVERIFY(QMetaObject::invokeMethod(map, "toggleFollowEvent"));
        QTest::qWait(700);
        QVERIFY(map->property("following").toBool());
        const double hypoLon = map->property("shiftedHypo").toMap().value("lon").toDouble();
        QVERIFY2(std::abs(map->property("centerLon").toDouble() - hypoLon) < 3.0,
                 qPrintable(QStringLiteral("跟随按钮未把镜头带回震中: center=%1 hypo=%2")
                                .arg(map->property("centerLon").toDouble()).arg(hypoLon)));
    }

    /// 回归：按住地图拖动期间，新事件到达不得抢走镜头。
    ///
    /// 曾经新事件聚焦直接写 center/zoom，会与拖拽逐帧抢同一组属性，手感上就是
    /// 「鼠标被抢」。现在手势期间只登记待执行聚焦，松开后再飞过去。
    void newEventDoesNotStealCameraWhileDragging() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("DragFocusTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map);
        QTest::qWait(400);

        EarthquakeEvent a;
        a.id = a.eventId = "DRAG-A";
        a.timestamp = now;
        a.latitude = 30.6;
        a.longitude = 104.0;
        a.magnitude = 5.0;
        a.depth = 10.0;
        a.rawIntensity = 3.0;
        controller.handleEvent(a, false, false);
        QTest::qWait(700);   // 等入场取景补间结束
        QVERIFY(map->property("following").toBool());
        QVERIFY(!map->property("gestureActive").toBool());

        // 按住并拖动：镜头 1:1 跟手，同时退出跟随。
        const QPointF origin = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, origin.toPoint());
        QTest::mouseMove(window, (origin + QPointF(120, 0)).toPoint());
        QTest::qWait(20);
        QVERIFY(map->property("gestureActive").toBool());
        QVERIFY(!map->property("following").toBool());
        const double draggedLon = map->property("centerLon").toDouble();

        // 仍按着时，远处新事件到达：只登记，不许写镜头。
        EarthquakeEvent b = a;
        b.id = b.eventId = "DRAG-B";
        b.timestamp = now;
        b.longitude = 112.0;
        controller.handleEvent(b, false, false);
        QTest::qWait(400);
        QVERIFY(!map->property("following").toBool());
        QVERIFY2(std::abs(map->property("centerLon").toDouble() - draggedLon) < 0.01,
                 qPrintable(QStringLiteral("拖动期间镜头被抢: before=%1 after=%2")
                                .arg(draggedLon).arg(map->property("centerLon").toDouble())));

        // 松开后，被推迟的聚焦才执行，把镜头带到新事件震中。
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                            (origin + QPointF(120, 0)).toPoint());
        QVERIFY(!map->property("gestureActive").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(map->property("following").toBool(), 1000);
        const double hypoLon = map->property("shiftedHypo").toMap().value("lon").toDouble();
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(map->property("centerLon").toDouble() - hypoLon) < 5.0, 3000);
    }

    /// 回归：从一个事件切到另一个事件时，镜头应完整补间飞行。
    ///
    /// 曾经切换焦点时 `setMapFocus` 先 `updateWaveTimer()`（同步发 waveRadiiChanged），
    /// 再发 centerMapRequested（QML 里 `Qt.callLater(focusEvent)`）。自动跟随分支与
    /// 布局重取景都用 duration 0 落位，它们排在入场补间之前，先把镜头瞬移到新震中，
    /// 随后那段 600ms 飞行就成了零距离 —— 用户看到的就是「几乎没有动画」。
    void switchingEventsKeepsFlightAnimation() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("SwitchFlightTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map);
        QTest::qWait(400);

        EarthquakeEvent a;
        a.id = a.eventId = "SWITCH-A";
        a.timestamp = now;
        a.latitude = 30.6;
        a.longitude = 104.0;
        a.magnitude = 5.0;
        a.depth = 10.0;
        a.rawIntensity = 3.0;
        controller.handleEvent(a, false, false);
        QTest::qWait(800);   // 等入场取景补间结束
        QVERIFY(map->property("following").toBool());
        const double startLon = map->property("centerLon").toDouble();

        // 切到远处的事件 B：应是 600ms 飞行，而不是瞬移。
        EarthquakeEvent b = a;
        b.id = b.eventId = "SWITCH-B";
        b.latitude = 38.0;
        b.longitude = 140.0;
        b.magnitude = 7.2;
        b.depth = 320.0;
        b.location = "日本本州东岸近海深源地震（超长地名用来撑高 HUD 卡片高度）";
        controller.handleEvent(b, false, false);
        // 采样整段补间：任何一次采样步进都不该吃掉大半路程（瞬移的特征）。
        // 切换事件会同时改变 HUD 高度 -> 布局重取景；若它先于入场补间落位，
        // 镜头会在一帧内跳到新震中，后面那段飞行就是零距离。
        QList<QPair<int, double>> samples;
        for (int t = 0; t <= 640; t += 20) {
            if (t > 0) QTest::qWait(20);
            samples.append({t, map->property("centerLon").toDouble()});
        }
        const double targetLon = map->property("shiftedHypo").toMap().value("lon").toDouble();
        const double distance = std::abs(targetLon - startLon);
        QVERIFY2(distance > 20.0, "测试前置条件失败：两个事件距离太近");
        double maxStep = 0.0;
        int maxStepAt = 0;
        for (int i = 1; i < samples.size(); ++i) {
            const double step = std::abs(samples[i].second - samples[i - 1].second);
            if (step > maxStep) { maxStep = step; maxStepAt = samples[i].first; }
        }
        QVERIFY2(maxStep < distance * 0.25,
                 qPrintable(QStringLiteral("切换事件时镜头瞬移（t=%1ms 单步 %2°，总路程 %3°）")
                                .arg(maxStepAt).arg(maxStep, 0, 'f', 1).arg(distance, 0, 'f', 1)));
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(map->property("centerLon").toDouble() - targetLon) < 8.0, 3000);
    }

    void settingsNavigationAndTheme() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("SettingsNavigationTest");
        AppController controller(nullptr, false);
        QVERIFY(!controller.darkMode());
        QSignalSpy themeChanges(&controller, &AppController::darkModeChanged);
        QQmlApplicationEngine engine;
        QStringList qmlErrors;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) {
                const auto message = warning.toString();
                if (message.contains("SettingsPage.qml") || message.contains("TypeError")
                    || message.contains("ReferenceError") || message.contains("Binding loop"))
                    qmlErrors.append(message);
            }
        });
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showSettings", true);
        auto* page = window->findChild<QQuickItem*>("settingsPage");
        auto* sidebar = window->findChild<QQuickItem*>("settingsSidebar");
        auto* background = window->findChild<QQuickItem*>("settingsBackground");
        auto* scroll = window->findChild<QQuickItem*>("settingsScroll");
        auto* light = findVisualItem(window, "lightThemeButton");
        auto* dark = findVisualItem(window, "darkThemeButton");
        auto* systemTheme = findVisualItem(window, "systemThemeButton");
        QVERIFY(page && sidebar && background && scroll && light && dark && systemTheme);
        QTest::qWait(100);
        QVERIFY(sidebar->isVisible());
        QCOMPARE(page->property("currentSection").toInt(), 0);
        // 默认是「跟随系统」；测试环境无深色配色方案，故解析结果为浅色。
        QCOMPARE(controller.settings()->themeMode(), QString("system"));
        QVERIFY(systemTheme->property("checked").toBool());
        QVERIFY(!light->property("checked").toBool());
        QVERIFY(!dark->property("checked").toBool());
        QVERIFY(!controller.darkMode());
        const QColor lightBackground = background->property("color").value<QColor>();
        QCOMPARE(lightBackground, QColor("#F4F7F7"));
        QCOMPARE(window->color(), lightBackground);
        auto* theme = page->property("theme").value<QObject*>();
        auto* pickLocation = findVisualItem(window, "pickLocationButton");
        QVERIFY(theme && pickLocation);
        QCOMPARE(theme->property("accentForeground").value<QColor>(), QColor("#FFFFFF"));
        QCOMPARE(pickLocation->property("foregroundColor").value<QColor>(), QColor("#FFFFFF"));
        const QString evidence = evidenceDirectory(window);
        auto capture = [&](const QString& name) {
            QTest::qWait(80);
            return window->grabWindow().save(evidence + "/" + name + ".png");
        };
        auto click = [&](const QString& name) {
            QTest::qWait(30); // Let category changes polish their new layout before hit testing.
            auto* item = findVisualItem(window, name);
            if (!item || !item->isVisible() || !item->isEnabled()) return false;
            const QPoint position = item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint();
            if (!QRect(QPoint(0, 0), window->size()).contains(position)) return false;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
            return true;
        };
        QVERIFY(click("settingsNav-appearance"));
        QVERIFY(capture("settings-default-light"));
        // Click the real QML controls: calling the C++ setter here would miss the original bug.
        QVERIFY(click("darkThemeButton"));
        QTRY_VERIFY(controller.darkMode());
        QCOMPARE(themeChanges.count(), 1);
        QVERIFY(controller.settings()->themeMode() == "dark");
        QVERIFY(dark->property("checked").toBool());
        QVERIFY(!light->property("checked").toBool());
        QCOMPARE(background->property("color").value<QColor>(), QColor("#101719"));
        QCOMPARE(window->color(), background->property("color").value<QColor>());
        QCOMPARE(theme->property("accentForeground").value<QColor>(), QColor("#073637"));
        QCOMPARE(pickLocation->property("foregroundColor").value<QColor>(), QColor("#073637"));
        SettingsStore persisted;
        QVERIFY(persisted.themeMode() == "dark");
        {
            AppController reloaded(nullptr, false);
            QVERIFY(reloaded.darkMode());
        }
        QVERIFY(capture("settings-selected-dark"));
        QVERIFY(click("darkThemeButton"));
        QVERIFY(dark->property("checked").toBool());
        QCOMPARE(themeChanges.count(), 1);
        light->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!controller.darkMode());
        QCOMPARE(themeChanges.count(), 2);
        QVERIFY(persisted.themeMode() == "light");
        QCOMPARE(background->property("color").value<QColor>(), lightBackground);
        QVERIFY(capture("settings-selected-light"));

        const QStringList categories = {"warning", "audio", "map", "appearance", "location",
                                       "sourceEstimation", "source", "clock", "about"};
        for (int index = 0; index < categories.size(); ++index) {
            QVERIFY(click("settingsNav-" + categories[index]));
            QTRY_COMPARE(page->property("currentSection").toInt(), index);
            QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);
            for (int other = 0; other < categories.size(); ++other) {
                auto* panel = window->findChild<QQuickItem*>("settingsPanel-" + categories[other]);
                QVERIFY(panel);
                QCOMPARE(panel->isVisible(), other == index);
            }
            scroll->setProperty("contentY", 100);
        }
        QVERIFY(click("settingsNav-location"));
        QVERIFY(capture("settings-location"));
        QVERIFY(click("pickLocationButton"));
        QTRY_VERIFY(!window->property("showSettings").toBool());
        auto* map = window->findChild<QQuickItem*>("mapView");
        QVERIFY(map && map->property("pickingLocation").toBool());
        map->setProperty("pickingLocation", false);
        window->setProperty("showSettings", true);
        QVERIFY(click("settingsNav-appearance"));
        QVERIFY(click("darkThemeButton"));
        QTRY_VERIFY(controller.darkMode());
        controller.settings()->setReduceMotion(true);
        QVERIFY(click("resetSettingsButton"));
        auto* dialog = window->findChild<QObject*>("resetSettingsDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(capture("settings-reset-confirmation"));
        QVERIFY(click("cancelResetButton"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(controller.darkMode());
        QVERIFY(controller.settings()->reduceMotion());
        QVERIFY(click("resetSettingsButton"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(click("confirmResetButton"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(!controller.darkMode());
        QVERIFY(!controller.settings()->reduceMotion());
        // 恢复默认后主题回到「跟随系统」，选中态必须跟着回落，不能停在旧按钮上。
        QCOMPARE(controller.settings()->themeMode(), QString("system"));
        QVERIFY(systemTheme->property("checked").toBool());
        QVERIFY(!light->property("checked").toBool());
        QVERIFY(!dark->property("checked").toBool());
        QCOMPARE(background->property("color").value<QColor>(), lightBackground);

        window->resize(360, 520);
        QTRY_VERIFY(!sidebar->isVisible());
        // 当前停在「外观」；窄屏下拉从当前项开始上下移动。
        page->setProperty("currentSection", 3);
        auto* combo = window->findChild<QQuickItem*>("settingsCategoryCombo");
        QVERIFY(combo && combo->isVisible());
        combo->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(page->property("currentSection").toInt(), 4);
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_COMPARE(page->property("currentSection").toInt(), 3);
        QTest::qWait(50);
        QVERIFY(click("darkThemeButton"));
        QTRY_VERIFY(controller.darkMode());
        QVERIFY(capture("settings-narrow-dark"));
        QVERIFY(click("lightThemeButton"));
        QTRY_VERIFY(!controller.darkMode());
        QVERIFY(capture("settings-narrow-light"));
        const QPointF sidebarOrigin = sidebar->position();
        scroll->setProperty("contentY", 180);
        window->resize(1280, 800);
        QTRY_VERIFY(sidebar->isVisible());
        QCOMPARE(sidebar->position(), sidebarOrigin);
        QVERIFY(click("settingsNav-audio"));
        QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);
        // 左下角版本号下方只在发现新版本时出现提示；其余状态不占位。
        auto* versionLabel = findVisualItem(window, "settingsVersionLabel");
        auto* updateHint = findVisualItem(window, "settingsUpdateHint");
        QVERIFY(versionLabel && updateHint);
        QVERIFY(versionLabel->isVisible());
        QVERIFY(!updateHint->isVisible());
        QCOMPARE(versionLabel->property("text").toString(),
                 "v" + controller.updater()->currentVersion());
        auto* updater = controller.updater();
        for (const QString& state : {QString("idle"), QString("checking"),
                                     QString("upToDate"), QString("error")}) {
            updater->setState(state, QString());
            QTest::qWait(20);
            QVERIFY2(!updateHint->isVisible(), qPrintable("unexpected hint in " + state));
        }
        updater->latest_ = "9.9.9";
        updater->setState(QStringLiteral("updateAvailable"), QStringLiteral("发现新版本 v9.9.9"));
        QTRY_VERIFY(updateHint->isVisible());
        bool hintTextFound = false;
        for (auto* item : visualItems(updateHint)) {
            if (item->property("text").toString() == "新版本 v9.9.9") hintTextFound = true;
        }
        QVERIFY(hintTextFound);
        QVERIFY(capture("settings-update-available"));
        updater->setState(QStringLiteral("upToDate"), QString());
        QTRY_VERIFY(!updateHint->isVisible());
        page->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("showSettings").toBool());
        QVERIFY2(qmlErrors.isEmpty(), qPrintable(qmlErrors.join('\n')));
    }

    // 侧栏只展示稳定的历史目录：实时预警不产生行（它只留在 HUD/地图）。
    // 同时验收卡片信息层级（距离列）与无定位时「500 km 内」不可选。
    void catalogListShowsStableHistoryOnly() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("CatalogListTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);

        EarthquakeEvent live;
        live.id = live.eventId = "LIVE-1";
        live.sourceAgency = "JMA";
        live.source = "JMA 紧急地震速报";
        live.timestamp = now;
        live.reportTime = now;
        live.latitude = 36.0; live.longitude = 140.0;
        live.magnitude = 6.0;
        live.location = "实时测试预警";
        controller.handleEvent(live, false, false);

        EarthquakeEvent catalog;
        catalog.id = catalog.eventId = "CAT-1";
        catalog.sourceAgency = "CENC";
        catalog.source = "中国地震台网 地震信息";
        catalog.timestamp = now;
        catalog.reportTime = now;
        catalog.latitude = 30.0; catalog.longitude = 104.0;
        catalog.magnitude = 4.6;
        catalog.location = "四川测试震中";
        controller.handleEvent(catalog, false, true);

        QQmlApplicationEngine engine;
        QStringList qmlErrors;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) {
                const auto message = warning.toString();
                if (message.contains("EventTile.qml") || message.contains("EventListSidebar.qml")
                    || message.contains("TypeError") || message.contains("ReferenceError")
                    || message.contains("Binding loop"))
                    qmlErrors.append(message);
            }
        });
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showList", true);
        QTRY_COMPARE(controller.eventModel()->rowCount(), 1);
        QTest::qWait(150);

        auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
        QVERIFY(sidebar);
        bool sawCatalog = false;
        bool sawLive = false;
        for (auto* item : visualItems(sidebar)) {
            const QString text = item->property("text").toString();
            if (text == QStringLiteral("四川测试震中")) sawCatalog = true;
            if (text == QStringLiteral("实时测试预警")) sawLive = true;
        }
        QVERIFY(sawCatalog);
        QVERIFY2(!sawLive, "实时预警不应出现在历史列表里");
        QString offender;
        QVERIFY2(timesFullyShown(sidebar, &offender),
                 qPrintable("列表发震时刻被截断: " + offender));
        // 无定位：「500 km 内」禁用（行上根本没有距离，选它只会得到空列表）。
        QQuickItem* within = nullptr;
        for (auto* item : visualItems(sidebar)) {
            if (item->property("text").toString() == QStringLiteral("500 km 内")) within = item;
        }
        QVERIFY(within);
        QVERIFY(!within->isEnabled());
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/12-event-list-catalog.png"));
        QVERIFY2(qmlErrors.isEmpty(), qPrintable(qmlErrors.join('\n')));
    }

    // 分级提醒信号：同一帧实时事件按是否达到本地门槛归入 alert / eew；目录事件归 info。
    void notificationTiersEmitPerEventKind() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("NotificationTierTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        QSignalSpy tiers(&controller, &AppController::notifiableEvent);

        auto makeEvent = [&](const QString& id, double magnitude) {
            EarthquakeEvent e;
            e.id = e.eventId = id.toStdString();
            e.timestamp = now;
            e.reportTime = now;
            e.latitude = 30.0;
            e.longitude = 104.0;
            e.depth = 10.0;
            e.magnitude = magnitude;
            e.location = "测试事件";
            e.sourceAgency = "TEST";
            e.warningLevel = WarningLevel::Warning;
            return e;
        };

        // 总开关关闭：未通过本地过滤 → eew 级，而不是静默丢弃。
        controller.handleEvent(makeEvent("EEW-1", 5.0), false, false);
        QCOMPARE(tiers.count(), 1);
        QCOMPARE(tiers.at(0).at(1).toString(), QStringLiteral("eew"));

        // 打开总开关且过滤为 0：同一路径应升级为 alert 级。
        controller.settings()->setEnableWarnings(true);
        controller.handleEvent(makeEvent("ALERT-1", 6.0), false, false);
        QCOMPARE(tiers.count(), 2);
        QCOMPARE(tiers.at(1).at(1).toString(), QStringLiteral("alert"));

        // 目录事件：新入库且时间新鲜 → info 级。
        EarthquakeEvent info = makeEvent("INFO-1", 4.2);
        info.latitude = 31.5;
        info.longitude = 105.5;
        info.timestamp = now + 1000;
        info.reportTime = now;
        controller.handleEvent(info, false, true);
        QCOMPARE(tiers.count(), 3);
        QCOMPARE(tiers.at(2).at(1).toString(), QStringLiteral("info"));

        // 同一物理地震的后续目录更新不再重复发 info。
        controller.handleEvent(info, false, true);
        QCOMPARE(tiers.count(), 3);

        // 逐源震级过滤在展示层求值：Wolfx 设为「不接收」后，目录照常入库（改回阈值能
        // 立刻重新出现），只是不进列表、不通知。
        WolfxSource probe;
        controller.settings()->setInfoMagnitudeFilter(QStringLiteral("wolfx"), -1.0);
        EarthquakeEvent filtered = makeEvent("FILTERED-1", 3.0);
        filtered.latitude = 33.0;
        filtered.longitude = 110.0;
        filtered.timestamp = now + 2000;
        filtered.reportTime = now;
        const auto displayed = [&controller](const QString& eventId) {
            for (const QVariant& entry : controller.eventList()) {
                if (entry.toMap().value(QStringLiteral("id")).toString() ==
                    QStringLiteral("TEST|") + eventId)
                    return true;
            }
            return false;
        };
        controller.handleEvent(filtered, false, true, &probe);
        QVERIFY(!displayed(QStringLiteral("FILTERED-1")));
        // 行模型同样要重筛（展示层过滤 + 0 ms 合并刷新）。
        QTRY_COMPARE(controller.eventModel()->rowCount(), 1);   // 只剩 INFO-1
        QCOMPARE(tiers.count(), 3);

        // 改回不过滤：同一条记录立即重新出现在列表里。它是已入库的旧闻，
        // 不补发提醒（否则调低阈值会被历史事件刷屏）。
        controller.settings()->setInfoMagnitudeFilter(QStringLiteral("wolfx"), 0.0);
        QTRY_COMPARE(controller.eventModel()->rowCount(), 2);
        QVERIFY(displayed(QStringLiteral("FILTERED-1")));
        QCOMPARE(tiers.count(), 3);
    }

    // 新增控件必须真的写回设置：提醒矩阵、播报分轨、关闭到托盘各点一次。
    void granularSettingsRoundTripThroughUi() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("GranularSettingsTest");
        AppController controller(nullptr, false);
        controller.settings()->setEnableWarnings(true);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showSettings", true);
        auto* page = window->findChild<QQuickItem*>("settingsPage");
        auto* scroll = window->findChild<QQuickItem*>("settingsScroll");
        QVERIFY(page && scroll);
        QTest::qWait(80);
        // 分区内的控件常落在视口之外：先把目标滚到中间再点，否则点击会被窗口丢弃。
        auto click = [&](const QString& name) {
            auto* item = findVisualItem(window, name);
            if (!item || !item->isVisible() || !item->isEnabled()) return false;
            const qreal maxY =
                qMax<qreal>(0, scroll->property("contentHeight").toReal() - scroll->height());
            const qreal contentY = scroll->property("contentY").toReal();
            const QPointF center =
                item->mapToItem(scroll, QPointF(item->width() / 2, item->height() / 2));
            scroll->setProperty("contentY",
                                qBound<qreal>(0, contentY + center.y() - scroll->height() / 2, maxY));
            QTest::qWait(60);
            const QPoint position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
            if (!QRect(QPoint(0, 0), window->size()).contains(position)) return false;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
            return true;
        };

        // 提醒矩阵（默认分区）
        QVERIFY(click("notifyAlertSwitch"));
        QTRY_VERIFY(!controller.settings()->notifyAlert());
        QVERIFY(click("raiseAlertSwitch"));
        QTRY_VERIFY(controller.settings()->raiseAlert());

        // 播报分轨
        page->setProperty("currentSection", 1);
        QTest::qWait(60);
        QVERIFY(click("announceCountdownSwitch"));
        QTRY_VERIFY(!controller.settings()->announceCountdown());

        // 关闭到托盘（「启动与更新」分区，索引 8）
        page->setProperty("currentSection", 8);
        QTest::qWait(60);
        QVERIFY(click("closeToTraySwitch"));
        QTRY_VERIFY(!controller.settings()->closeToTray());

        // 数据源 → 子数据源（索引 6）：默认折叠，展开后逐项落盘且跨源同名频道互不影响。
        page->setProperty("currentSection", 6);
        QTest::qWait(80);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/56-settings-sources-split.png"));
        QVERIFY(controller.settings()->isSubSourceEnabled("wolfx", "jma_eew"));
        QVERIFY(click("wolfxSubHeader"));
        QTRY_VERIFY(findVisualItem(window, "wolfxSub-jma_eew"));
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/57-settings-sources-subs.png"));
        QVERIFY(click("wolfxSub-jma_eew"));
        QTRY_VERIFY(!controller.settings()->isSubSourceEnabled("wolfx", "jma_eew"));
        QVERIFY(controller.settings()->isSubSourceEnabled("whews", "jma_eew"));
        // 分组「全开」恢复整组；展开状态不因 app.sources 重建而丢失（按钮仍可找到）。
        QVERIFY(click("wolfxSubAll-live"));
        QTRY_VERIFY(controller.settings()->isSubSourceEnabled("wolfx", "jma_eew"));
        QVERIFY(findVisualItem(window, "wolfxSub-jma_eew"));

        // 左右分栏：左栏点选源，右栏详情随之切换；密钥与子源仍在该源自己的详情里。
        QVERIFY(findVisualItem(window, "sourceNav-wolfx"));
        QVERIFY(click("sourceNav-pancakes"));
        QTRY_VERIFY(findVisualItem(window, "pancakesSwitch"));
        QVERIFY(!findVisualItem(window, "wolfxSwitch"));
        QVERIFY(click("sourceNav-jian"));
        QTRY_VERIFY(findVisualItem(window, "jianLoginField"));
        // 切回 Wolfx：换源会收起子源面板，展开后仍可操作。
        QVERIFY(click("sourceNav-wolfx"));
        QTRY_VERIFY(findVisualItem(window, "wolfxSwitch"));
        QVERIFY(!findVisualItem(window, "wolfxSub-jma_eew")->isVisible());

        // 版面必须读成**一整块**：左右两列住在同一张卡里，卡高由两列较高者决定。
        // 旧版是「独立的左栏卡 + 独立的右栏卡」：两卡各长各的，接缝处两条描边对不齐、
        // 左栏下面空出一截 —— 那正是「分栏没弄好」的观感来源。
        auto* navCard = findVisualItem(window, "sourceNavCard");
        auto* detail = findVisualItem(window, "sourceDetailCard");
        auto* splitRow = findVisualItem(window, "sourceSplitRow");
        auto* splitCard = findVisualItem(window, "sourceSplitCard");
        auto* divider = findVisualItem(window, "sourceSplitDivider");
        QVERIFY(navCard && detail && splitRow && splitCard && divider);
        QVERIFY(navCard->isVisible());
        QVERIFY(divider->isVisible());
        const QPointF navTop = navCard->mapToScene(QPointF(0, 0));
        const QPointF detailTop = detail->mapToScene(QPointF(0, 0));
        QVERIFY(navTop.x() < detailTop.x());
        QCOMPARE(navTop.y(), detailTop.y());
        QCOMPARE(navCard->height(), detail->height());
        QCOMPARE(detail->mapToItem(splitRow, QPointF(detail->width(), 0)).x(),
                 splitRow->width());
        // 两列都在卡内、都被卡的圆角包住：任何一列的边缘越出卡就是「卡中卡」。
        for (auto* column : {navCard, detail}) {
            const QRectF inside(column->mapToItem(splitCard, QPointF(0, 0)),
                                QSizeF(column->width(), column->height()));
            QVERIFY2(inside.left() >= -0.5 && inside.right() <= splitCard->width() + 0.5
                         && inside.top() >= -0.5 && inside.bottom() <= splitCard->height() + 0.5,
                     qPrintable(QStringLiteral("列越出卡片: %1 x %2 @ (%3,%4) / 卡 %5 x %6")
                                    .arg(inside.width()).arg(inside.height())
                                    .arg(inside.left()).arg(inside.top())
                                    .arg(splitCard->width()).arg(splitCard->height())));
        }
        // 分隔线落在两列之间、且与两列等高（它划出边界，不能只画一半）。
        QVERIFY(divider->mapToItem(splitRow, QPointF(0, 0)).x() > navCard->width() - 1);
        QCOMPARE(divider->height(), splitRow->height());
        // 卡外不该再有第二张「事件源」卡：整块分栏只有 sourceSplitCard 一张。
        int sourceCards = 0;
        for (auto* item : visualItems(window->contentItem())) {
            if (item->objectName() == "sourceSplitCard" && item->isVisible()) ++sourceCards;
        }
        QCOMPARE(sourceCards, 1);

        // 长内容不靠「把卡片和页面一起撑长」：Whews 有近 40 个子源，展开后必须落在
        // 卡片内部滚动，卡片高度封顶（否则整页会被拉成几千像素的滚动条）。
        QVERIFY(click("sourceNav-whews"));
        QTRY_VERIFY(findVisualItem(window, "whewsSwitch"));
        QVERIFY(click("whewsSubHeader"));
        auto* detailScroll = findVisualItem(window, "sourceDetailScroll");
        QVERIFY(detailScroll);
        QTRY_VERIFY(detailScroll->property("contentHeight").toReal()
                    > detailScroll->height() + 50);
        QVERIFY2(detail->height() <= 560,
                 qPrintable(QStringLiteral("右栏高度未封顶: %1").arg(detail->height())));
        QCOMPARE(navCard->height(), detail->height());
        // 滚动条是「下面还有内容」的唯一提示，必须真的存在且落在卡片内。
        auto* detailBar = findVisualItem(window, "sourceDetailScrollBar");
        QVERIFY(detailBar);
        QVERIFY(detailBar->width() > 0);
        QVERIFY(detailBar->mapToItem(detail, QPointF(0, 0)).x() < detail->width());
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/57b-settings-sources-whews.png"));
        QVERIFY(click("sourceNav-wolfx"));
        QTRY_VERIFY(findVisualItem(window, "wolfxSwitch"));

        // ── 延迟显示 ────────────────────────────────────────────────────────
        // 设置页此前对四个源永远看不到延迟（只有 Wolfx 曾经 ping 过，其余三个源连
        // pong 都没接）。链路层由 qt_business 的 everyEventSourceMeasuresLinkLatency
        // 覆盖；这里断言的是**显示层**：有了延迟数值之后，左栏行与右栏状态行都要出现，
        // 没有时不占位（不显示「— ms」，那对用户只是噪声）。
        auto* section = findVisualItem(window, "settingsPanel-source");
        auto* latencyLabel = findVisualItem(window, "sourceLatencyLabel");
        auto* navLatency = findVisualItem(window, "sourceNavLatency-wolfx");
        QVERIFY(section && latencyLabel && navLatency);

        // 快照里必须是**数值**字段：字符串比较（`latency !== "— ms"`）既脆弱又拿不到
        // 数值，做不了「只在一个区间内才算良好」这类判断。
        bool sawLatencyKeys = false;
        for (const auto& item : controller.sources()) {
            const QVariantMap map = item.toMap();
            if (map["id"].toString() != "wolfx") continue;
            sawLatencyKeys = map.contains("latencyMs") && map.contains("directoryLatencyMs")
                && map["latencyMs"].toLongLong() < 0 == (map["latencyText"].toString().isEmpty());
        }
        QVERIFY2(sawLatencyKeys, "源快照缺少 latencyMs / directoryLatencyMs 数值字段");

        // 用一张假的源快照直接驱动详情卡：不依赖真实网络状态，断言的是排版逻辑。
        QVariantMap fakeSource;
        fakeSource["id"] = "wolfx";
        fakeSource["name"] = "Wolfx";
        fakeSource["enabled"] = true;
        fakeSource["configured"] = true;
        fakeSource["status"] = "在线";
        fakeSource["statusTag"] = "CONNECTED";
        fakeSource["latencyMs"] = 42;
        fakeSource["latencyText"] = "42 ms";
        fakeSource["directoryLatencyMs"] = 118;
        fakeSource["directoryLatencyText"] = "118 ms";
        fakeSource["directory"] = "目录刷新成功";
        fakeSource["directoryStatusTag"] = "NORMAL";
        fakeSource["description"] = "Wolfx all_eew 聚合（CENC/SC/JMA/CWA/FJ/CQ）";
        fakeSource["region"] = "全球";
        fakeSource["subSources"] = QVariantList{};

        // 左栏行的文案由 SourceSection.sourceLatencyText 统一生成（两处口径必须一致）。
        QVariant leftText;
        QVERIFY(QMetaObject::invokeMethod(section, "sourceLatencyText",
                                          Q_RETURN_ARG(QVariant, leftText),
                                          Q_ARG(QVariant, fakeSource)));
        QCOMPARE(leftText.toString(), QStringLiteral("实时 42 ms · 目录 118 ms"));

        // 详情卡：把 source 换掉后，状态行右侧必须出现同样的延迟文本。
        auto* detailItem = findVisualItem(window, "sourceDetailCard");
        QVERIFY(detailItem);
        QQuickItem* detailCardItem = nullptr;
        for (auto* child : detailItem->childItems()) {
            if (child->property("source").isValid()) { detailCardItem = child; break; }
        }
        QVERIFY(detailCardItem);
        const QVariant savedSource = detailCardItem->property("source");
        detailCardItem->setProperty("source", fakeSource);
        QTRY_VERIFY(latencyLabel->isVisible());
        QCOMPARE(latencyLabel->property("text").toString(),
                 QStringLiteral("实时 42 ms · 目录 118 ms"));
        // 只测出实时、目录还没刷新过：只显示实时那一段。
        fakeSource["directoryLatencyMs"] = -1;
        fakeSource["directoryLatencyText"] = "";
        detailCardItem->setProperty("source", fakeSource);
        QTRY_COMPARE(latencyLabel->property("text").toString(), QStringLiteral("实时 42 ms"));
        // 一个都没测出（-1 = 未测出）：整行不占位，不能写成「— ms」。
        fakeSource["latencyMs"] = -1;
        fakeSource["latencyText"] = "";
        detailCardItem->setProperty("source", fakeSource);
        QTRY_VERIFY(!latencyLabel->isVisible());
        QVERIFY(latencyLabel->property("text").toString().isEmpty());
        // 还原真实快照，后面的取证与断言继续按真实状态走。
        detailCardItem->setProperty("source", savedSource);

        // 退化路径：没有延迟时整行隐藏，左栏那一行也不出现 —— 两处都不得留一格空白。
        QVERIFY(QMetaObject::invokeMethod(section, "sourceLatencyText",
                                          Q_RETURN_ARG(QVariant, leftText),
                                          Q_ARG(QVariant, QVariantMap(fakeSource))));
        QVERIFY(leftText.toString().isEmpty());

        // 深色主题下的取证：分栏卡的底色必须跟着主题走。若卡片自己没注入 theme，
        // 深色主题下会挂着一张浅色卡（浅字 + 白底，几乎全糊）——只能靠看图发现。
        controller.settings()->setThemeMode("dark");
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/56b-settings-sources-dark.png"));
        controller.settings()->setThemeMode("light");
        QTest::qWait(120);

        // 恢复默认后全部回到初始值。
        controller.settings()->resetToDefaults();
        QVERIFY(controller.settings()->notifyAlert());
        QVERIFY(!controller.settings()->raiseAlert());
        QVERIFY(controller.settings()->announceCountdown());
        QVERIFY(controller.settings()->closeToTray());
        QVERIFY(controller.settings()->disabledSubSources().isEmpty());
    }

    // 「数据源」分区的延迟必须真的显示出来。
    //
    // 端到端链路：本机 QWebSocketServer（按 RFC 6455 自动回 pong）→ 真实 WolfxSource →
    // AppController 快照 → QML 左栏行 + 右栏状态行。断言两处文本是同一组数字，格式为
    // 「实时 N ms」，而**不是**恒空的占位。
    //
    // 钉住的原缺陷：`latencyMs` 此前对除 Wolfx 以外的源恒为 -1（没人 ping），而 QML 用
    // 字符串 `!== "— ms"` 判空 —— 于是「延迟」这一栏在设置页里从来没见过。
    //
    // **踩坑**：左栏行是 Repeater 的 delegate，`app.sources` 一变整批 delegate 就被销毁
    // 重建。任何 `qWait`/`QTRY_*` 之后都不能复用之前抓到的指针（会 SEGV），必须重新
    // `findVisualItem`。这条经验对设置页里所有 Repeater 委托都成立。
    void sourceLatencyIsVisibleInSettings() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("SourceLatencyTest");

        // 本机对端：只做 ping/pong，不收发业务帧。
        QWebSocketServer server(QStringLiteral("komira-latency-ui-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&server]() {
            while (server.hasPendingConnections()) {
                auto* peer = server.nextPendingConnection();
                QObject::connect(peer, &QWebSocket::disconnected, peer, &QWebSocket::deleteLater);
            }
        });
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));

        AppController controller(nullptr, false);
        auto* wolfx = controller.sources_.at(0);
        QCOMPARE(wolfx->id(), QStringLiteral("wolfx"));

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showSettings", true);
        auto* page = window->findChild<QQuickItem*>("settingsPage");
        QVERIFY(page);
        page->setProperty("currentSection", 6);
        QTest::qWait(150);

        QVERIFY(findVisualItem(window, "settingsPanel-source"));
        // 未连接时：没有延迟可显示，左栏那一行必须隐藏（不留空白、也不写「— ms」）。
        {
            auto* navLatency = findVisualItem(window, "sourceNavLatency-wolfx");
            QVERIFY(navLatency);
            QVERIFY(!navLatency->isVisible());
        }

        // 真连一条本机链路：探针拿到 pong 后，源快照的 `latencyMs` 变成 >= 0。
        auto* source = static_cast<WolfxSource*>(wolfx);
        source->running_ = true;
        source->generation_ = 1;
        auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, source);
        source->socket_ = socket;
        source->attachSocketHandlers(socket, /*generation*/ 1, ++source->attempt_);
        socket->open(url);
        QTRY_VERIFY_WITH_TIMEOUT(source->info().latencyMs >= 0, 6000);

        // 走到这里 `app.sources` 已随 250 ms 合并定时器重建过若干次，之前抓的 item 全部
        // 失效 —— 一律重新查（见函数头的踩坑说明）。
        QTest::qWait(400);   // 等一次 statusChanged 落地
        auto* navLatency = findVisualItem(window, "sourceNavLatency-wolfx");
        QVERIFY(navLatency);
        QTRY_VERIFY(navLatency->isVisible());
        const QString navText = navLatency->property("text").toString();
        QVERIFY2(navText.startsWith(QStringLiteral("实时 ")),
                 qPrintable(QStringLiteral("左栏延迟文案不对: %1").arg(navText)));
        QVERIFY2(navText.contains(QStringLiteral(" ms")), qPrintable(navText));
        QVERIFY2(!navText.contains(QStringLiteral("—")), qPrintable(navText));

        // 右栏状态行同一组数字：两处口径必须一致，不能「一边有、一边空」。
        auto* detailLabel = findVisualItem(window, "sourceLatencyLabel");
        QVERIFY(detailLabel);
        QTRY_VERIFY(detailLabel->isVisible());
        QCOMPARE(detailLabel->property("text").toString(), navText);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/58-settings-sources-latency.png"));

        // 关掉源：延迟必须立刻收起（旧值不能冒充当前状况）。
        source->stop();
        QVERIFY(source->info().latencyMs < 0);
        QTest::qWait(400);
        // delegate 已重建，重新查。
        auto* detailLabel2 = findVisualItem(window, "sourceLatencyLabel");
        auto* navLatency2 = findVisualItem(window, "sourceNavLatency-wolfx");
        QVERIFY(detailLabel2 && navLatency2);
        QTRY_VERIFY(!detailLabel2->isVisible());
        QVERIFY(!navLatency2->isVisible());
        QVERIFY(detailLabel2->property("text").toString().isEmpty());
    }

    void mapLayerSettingsAndVectorOverlay() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("MapLayerSettingsTest");

        AppController controller(nullptr, false);
        // 事件时刻固定在 now：EarthquakeEvent::expired() 把「比 now 晚 60s 以上」判为过期，
        // 若不钉住时钟，这个 2027 年的时间戳会被当作未来事件丢弃，焦点为空、着色自然为空。
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setBasemapId("amap_vector");
        controller.settings()->setVectorOverlay(false);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showSettings", true);
        QTest::qWait(100);

        auto* page = window->findChild<QQuickItem*>("settingsPage");
        auto* scroll = window->findChild<QQuickItem*>("settingsScroll");
        auto* vectorSwitch = findVisualItem(window, "vectorOverlaySwitch");
        QVERIFY(page && scroll && vectorSwitch);
        page->setProperty("currentSection", 2);   // 「地图与图层」
        QTest::qWait(60);
        QVERIFY(vectorSwitch->isVisible());
        QVERIFY2(vectorSwitch->width() > 0 && vectorSwitch->width() <= scroll->width(),
                 "map switch must remain inside the settings column");

        // 开关位于「地图底图」分区底部，默认落在视口之外 —— isVisible() 不反映滚动裁剪，
        // 直接点击会落在窗口外被丢弃。先把设置页滚到让目标居中的位置再点。
        auto ensureVisible = [&](QQuickItem* item) {
            const qreal maxY =
                qMax<qreal>(0, scroll->property("contentHeight").toReal() - scroll->height());
            const qreal contentY = scroll->property("contentY").toReal();
            const QPointF center =
                item->mapToItem(scroll, QPointF(item->width() / 2, item->height() / 2));
            scroll->setProperty("contentY",
                                qBound<qreal>(0, contentY + center.y() - scroll->height() / 2, maxY));
            QTest::qWait(50);
        };
        auto click = [&](QQuickItem* item) {
            ensureVisible(item);
            const QPoint position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
            QVERIFY2(QRect(QPoint(0, 0), window->size()).contains(position),
                     "map switch must be inside the window after scrolling");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
        };
        click(vectorSwitch);
        QTRY_VERIFY(controller.settings()->vectorOverlay());

        EarthquakeEvent event;
        event.id = event.eventId = "DISTRICT-TOGGLE";
        event.timestamp = now;
        event.latitude = 30.6;
        event.longitude = 104.0;
        event.depth = 10.0;
        event.magnitude = 6.5;
        event.location = "测试事件";
        controller.handleEvent(event, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.districtIntensity_.isEmpty(), 1500);

        // 区县着色跟随「叠加矢量层」：关闭即清空，重新开启按当前事件重算。
        controller.settings()->setVectorOverlay(false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.districtIntensity_.isEmpty(), 1500);
        controller.settings()->setVectorOverlay(true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.districtIntensity_.isEmpty(), 1500);
    }

    void iconsAndPersistence() {
        QTemporaryDir isolated;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("MaterialTest");
        {
            SettingsStore settings;
            QVERIFY(settings.backgroundBlur());
            settings.setBackgroundBlur(false);
            settings.setReduceMotion(true);
        }
        SettingsStore reloaded;
        QVERIFY(!reloaded.backgroundBlur());
        QVERIFY(reloaded.reduceMotion());
        reloaded.setReduceMotion(false);
        QVERIFY(!reloaded.backgroundBlur());
        reloaded.resetToDefaults();
        QVERIFY(reloaded.backgroundBlur());
        QVERIFY(!reloaded.reduceMotion());
        IconProvider provider;
        const auto resources = QDir(":/icons").entryList({"*.svg"});
        QVERIFY(resources.size() >= 41);
        for (const auto& resource : resources) {
            QSize actual;
            const auto image = provider.requestImage(resource.chopped(4) + "/ff006874", &actual, QSize(40, 40));
            QVERIFY2(!image.isNull(), qPrintable(resource));
            QCOMPARE(actual, QSize(40, 40));
            bool painted = false;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 250) continue;
                painted = true;
                QCOMPARE(pixel.red(), 0);
                QCOMPARE(pixel.green(), 104);
                QCOMPARE(pixel.blue(), 116);
            }
            QVERIFY2(painted, qPrintable(resource));
        }
    }

    void backdropGeometryAndPixels() {
        QTemporaryDir isolated;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        AppController controller(nullptr, false);
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setBackgroundBlur(true);
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl::fromLocalFile(QStringLiteral(KOMIRA_SOURCE_DIR "/tests/MaterialScene.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto* card = window->findChild<QQuickItem*>("fixtureCard");
        auto* parent = window->findChild<QQuickItem*>("movingParent");
        auto* source = window->findChild<QQuickItem*>("fixtureMap");
        QVERIFY(card && parent && source);
        const bool software = window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        QCOMPARE(card->property("blurActive").toBool(), !software);
        auto* sample = window->findChild<QQuickItem*>("backdropSample");
        if (software) {
            QVERIFY(!sample);
            QCOMPARE(card->property("color").value<QColor>().alpha(), 255);
        } else {
            QVERIFY(sample);
            auto* blur = window->findChild<QQuickItem*>("backdropBlur");
            QVERIFY(blur);
            const qreal padding = blur->property("padding").toReal();
            QVERIFY(padding >= blur->property("blurRadius").toReal());
            // 模糊核恒为全模糊：相机移动（live=true）与静止（live=false）不得降档。
            // 回归自 e0d8262 的「移动时 blurMax 32 / 静止 64」——用户实测看到拖动时
            // 模糊像被关掉一会儿（blurRadius 同时决定 padding，一变就要重建采样纹理）。
            QCOMPARE(blur->property("blurRadius").toInt(), 64);
            card->setProperty("backdropLive", false);
            QTRY_VERIFY(!blur->property("live").toBool());
            QCOMPARE(blur->property("blurRadius").toInt(), 64);
            QCOMPARE(blur->property("padding").toReal(), padding);
            card->setProperty("backdropLive", true);
            QTRY_VERIFY(blur->property("live").toBool());
            QCOMPARE(blur->property("blurRadius").toInt(), 64);
            QCOMPARE(blur->property("padding").toReal(), padding);
            auto rect = [&] { return sample->property("sourceRect").toRectF(); };
            QCOMPARE(rect().topLeft(), QPointF(100 - padding, 90 - padding));
            card->setX(120);
            QTRY_COMPARE(rect().x(), 140 - padding);
            parent->setY(40);
            QTRY_COMPARE(rect().y(), 110 - padding);
            source->setX(10);
            QCOMPARE(source->x(), 10.0);
            QTRY_COMPARE(rect().x(), 130 - padding);
            source->setTransformOrigin(QQuickItem::TopLeft);
            source->setScale(2);
            QTRY_COMPARE(rect().width(), (card->width() + 2 * padding) / 2);
            source->setScale(1); source->setX(0); parent->setY(20); card->setX(80);
            QTRY_COMPARE(rect().topLeft(), QPointF(100 - padding, 90 - padding));
            QVERIFY(!sample->property("recursive").toBool());
            QCOMPARE(sample->property("sourceItem").value<QQuickItem*>(), source);
        }
        const QString evidence = evidenceDirectory(window);
        auto capture = [&] {
            QTest::qWait(150);
            const auto image = window->grabWindow();
            // Keep sample coordinates in logical pixels on high-DPI displays.
            return image.size() == window->size() ? image
                : image.scaled(window->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        };
        auto colorDistance = [](const QColor& a, const QColor& b) {
            return (std::abs(a.red() - b.red()) + std::abs(a.green() - b.green())
                    + std::abs(a.blue() - b.blue())) / 3.0;
        };
        auto stripeEnergy = [&](const QImage& image) {
            double sum = 0;
            int count = 0;
            for (int y = 120; y < 144; ++y) for (int x = 140; x < 204; ++x) {
                sum += colorDistance(image.pixelColor(x, y), image.pixelColor(x + 1, y));
                ++count;
            }
            return sum / count;
        };
        const auto* crisp = window->findChild<QQuickItem*>("crispText");
        const auto* mark = window->findChild<QQuickItem*>("crispMark");
        QVERIFY(crisp && crisp->parentItem() == card);
        QVERIFY(mark && mark->parentItem() == card);
        const QPoint markCenter = mark->mapToScene(QPointF(mark->width() / 2, mark->height() / 2)).toPoint();
        for (bool dark : {false, true}) {
            controller.settings()->setThemeMode(dark ? "dark" : "light");
            controller.settings()->setBackgroundBlur(true);
            const QString theme = dark ? "dark" : "light";
            const auto blurred = capture();
            QVERIFY(!blurred.isNull());
            QVERIFY(blurred.save(evidence + "/12-material-" + theme + "-blur.png"));
            QCOMPARE(card->property("blurActive").toBool(), !software);
            QCOMPARE(blurred.pixelColor(markCenter), mark->property("color").value<QColor>());
            if (!software) {
                auto* effect = window->findChild<QQuickItem*>("backdropEffect");
                QVERIFY(effect);
                const QColor tint = card->property("color").value<QColor>();
                QVERIFY(tint.alphaF() > 0.0 && tint.alphaF() < 0.75);
                // Same translucent tint, but no shader: transparency alone must not pass.
                effect->setVisible(false);
                const auto tintOnly = capture();
                QVERIFY(!tintOnly.isNull());
                QVERIFY(tintOnly.save(evidence + "/12-material-" + theme + "-tint-only.png"));
                QCOMPARE(card->property("color").value<QColor>(), tint);
                const double unfilteredEnergy = stripeEnergy(tintOnly);
                const double blurredEnergy = stripeEnergy(blurred);
                QVERIFY(unfilteredEnergy > 5.0);
                QVERIFY2(blurredEnergy < unfilteredEnergy * 0.25,
                         qPrintable(QStringLiteral("Backdrop stripes: blurred %1, tint-only %2")
                                    .arg(blurredEnergy).arg(unfilteredEnergy)));
                // The orange swatch must survive: an opaque or empty capture is not blur.
                QVERIFY(colorDistance(blurred.pixelColor(180, 120), blurred.pixelColor(300, 120)) > 12.0);
                QCOMPARE(blurred.pixelColor(102, 92), tintOnly.pixelColor(102, 92));
                QCOMPARE(blurred.pixelColor(markCenter), tintOnly.pixelColor(markCenter));
                effect->setVisible(true);
            }
            controller.settings()->setBackgroundBlur(false);
            const auto opaque = capture();
            QVERIFY(!opaque.isNull());
            QVERIFY(opaque.save(evidence + "/13-material-" + theme + "-opaque.png"));
            QVERIFY(!card->property("blurActive").toBool());
            QCOMPARE(card->property("color").value<QColor>().alpha(), 255);
            QVERIFY(!window->findChild<QQuickItem*>("backdropSample"));
            QCOMPARE(opaque.pixelColor(markCenter), blurred.pixelColor(markCenter));
            QCOMPARE(opaque.pixelColor(180, 120), opaque.pixelColor(300, 120));
            QCOMPARE(blurred.pixelColor(102, 92), opaque.pixelColor(102, 92));
            if (software) QCOMPARE(blurred, opaque);
        }
    }

    void protectedAlertNotDisplacedByObservationEvent() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("ProtectedAlertTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(2.0);
        controller.setManualLocation(30.0, 103.0, "本地观测站");

        // 1. 注入一个触发本地烈度限制且有倒计时的紧急预警（本地震中距约 40km，烈度 4.5）
        EarthquakeEvent alert;
        alert.id = alert.eventId = "CRITICAL-ALERT";
        alert.sourceProvider = "Wolfx"; alert.sourceAgency = "CENC"; alert.source = "四川地震预警";
        alert.timestamp = now; alert.latitude = 30.3; alert.longitude = 103.3;
        alert.depth = 10.0; alert.magnitude = 5.5; alert.location = "四川某地";
        alert.rawIntensity = 4.5;
        controller.handleEvent(alert, false, false);

        QVERIFY(controller.hasWarning());
        QCOMPARE(controller.activeWarning_.eventId, alert.eventId);
        QCOMPARE(controller.selectedWarning_, alert.identity());
        QVERIFY(controller.isProtectedAlert(controller.activeWarning_));
        QVERIFY(controller.countdown() > 0);

        // 2. 此时到来了一个普通地震观测情报（未达到本地烈度阈值的普通事件）
        EarthquakeEvent obs;
        obs.id = obs.eventId = "OBS-EVENT";
        obs.sourceProvider = "CENC"; obs.sourceAgency = "CENC"; obs.source = "中国地震台网";
        obs.timestamp = now + 1000; obs.latitude = 20.0; obs.longitude = 120.0;
        obs.depth = 20.0; obs.magnitude = 4.0; obs.location = "台湾海域";
        obs.rawIntensity = 0.5; // 未达到本地烈度限制

        QSignalSpy camera(&controller, &AppController::centerMapRequested);
        controller.handleEvent(obs, false, false);

        // 验证：普通观测数据未抢占相机焦点、未顶掉活动关键预警、未把倒计时干没
        QCOMPARE(camera.count(), 0);
        QCOMPARE(controller.activeWarning_.eventId, alert.eventId);
        QCOMPARE(controller.selectedWarning_, alert.identity());
        QVERIFY(controller.countdown() > 0);
        // 且观测数据被收录到了会话列表中供查阅
        QVERIFY(controller.activeWarnings().size() >= 2);

        // 3. 模拟又来了一个同样触发本地烈度限制的关键紧急预警
        EarthquakeEvent alert2;
        alert2.id = alert2.eventId = "CRITICAL-ALERT-2";
        alert2.sourceProvider = "Wolfx"; alert2.sourceAgency = "CENC"; alert2.source = "另一高危预警";
        alert2.timestamp = now + 2000; alert2.latitude = 30.1; alert2.longitude = 103.1;
        alert2.depth = 10.0; alert2.magnitude = 6.0; alert2.location = "极近处大地震";
        alert2.rawIntensity = 5.5;

        controller.handleEvent(alert2, false, false);
        // 新的关键紧急预警达到门槛，允许正常切换
        QCOMPARE(controller.activeWarning_.eventId, alert2.eventId);
        QCOMPARE(controller.selectedWarning_, alert2.identity());
    }

    /// 机构优先级：JMA/CEA 永远压过 USGS/GQ；GQ 的新报不能把权威事件顶掉。
    /// 手动选中 GQ 后同样如此（用户要求「USGS 和 GQ 永远是最低的」）。
    void eventPriorityPutsAuthorityAboveUsgsAndGq() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventPriorityTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);

        const auto make = [](const QString& id, const QString& agency,
                             double lat, double lon, long long stamp) {
            EarthquakeEvent e;
            e.id = e.eventId = id.toStdString();
            e.sourceProvider = "Pancakes";
            e.sourceAgency = agency.toStdString();
            e.source = (agency + QStringLiteral(" 地震信息")).toStdString();
            e.timestamp = stamp;
            e.reportTime = stamp;
            e.latitude = lat;
            e.longitude = lon;
            e.depth = 10.0;
            e.magnitude = 5.0;
            e.location = "优先级测试";
            return e;
        };
        const auto focusedId = [&]() {
            const QVariantMap map = controller.mapEvent().toMap();
            return map.value("id").toString();
        };

        // 1. 先来一条 GQ：没有竞争者，正常聚焦。
        const EarthquakeEvent gq1 = make("GQ-1", "GQ", 45.0, 150.0, now);
        controller.handleEvent(gq1, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(gq1.identity()));

        // 2. JMA 到达（更晚，但即使更早也一样）：权威机构接管。
        now += 60'000;
        const EarthquakeEvent jma = make("JMA-1", "JMA", 36.0, 140.0, now);
        controller.handleEvent(jma, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(jma.identity()));
        QCOMPARE(controller.hudIndex(), 0);   // 优先级最高的排在第一页

        // 3. 后续 GQ 报（更新）不得把 JMA 顶掉。
        now += 60'000;
        const EarthquakeEvent gq2 = make("GQ-2", "GQ", 30.0, 104.0, now);
        controller.handleEvent(gq2, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(jma.identity()));

        // 4. 用户手动选中 GQ 后，普通 GQ 仍不能抢；但权威机构事件可以。
        controller.toggleMapFocus(QString::fromStdString(gq2.identity()));
        QCOMPARE(focusedId(), QString::fromStdString(gq2.identity()));
        now += 60'000;
        const EarthquakeEvent gq3 = make("GQ-3", "GQ", 31.0, 105.0, now);
        controller.handleEvent(gq3, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(gq2.identity()));

        now += 60'000;
        const EarthquakeEvent jma2 = make("JMA-2", "JMA", 37.0, 141.0, now);
        controller.handleEvent(jma2, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(jma2.identity()));

        // 5. 取消焦点后再来一条 GQ：会话里已有 JMA，镜头仍应回到 JMA 而不是 GQ
        //    （焦点目标取「会话中优先级最高的那条」，不只是刚到的那条）。
        controller.clearMapFocus();
        now += 60'000;
        const EarthquakeEvent gq4 = make("GQ-4", "GQ", 32.0, 106.0, now);
        controller.handleEvent(gq4, false, false);
        QCOMPARE(focusedId(), QString::fromStdString(jma2.identity()));
    }

    /// 目录去重只认「同一份上游报文的重复聚合链路」：同机构 + 同一上游事件 ID。
    /// 同机构、震中邻近但事件 ID 不同的记录必须各自保留 —— 附近连续两场小地震
    /// 不能被压成一条，也不能让「最佳记录」覆盖掉另一条的时间/震级。
    void catalogKeepsDistinctSourceRecords() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("CatalogDedupTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);

        EarthquakeEvent usgs;
        usgs.id = "pancakes_usgs_A";
        usgs.eventId = "usgs:us7000abcd";
        usgs.sourceProvider = "Pancakes"; usgs.sourceAgency = "USGS";
        usgs.source = "USGS 地震信息";
        usgs.timestamp = now; usgs.reportTime = now;
        usgs.latitude = 30.0; usgs.longitude = 104.0; usgs.magnitude = 5.6;
        usgs.location = "四川";
        controller.handleEvent(usgs, false, true);

        // 同一份上游报文的另一条聚合链路：eventId 只差频道前缀，仍应合成一条并保留两个来源。
        EarthquakeEvent relayed = usgs;
        relayed.id = "wolfx_usgs_A";
        relayed.eventId = "us7000abcd";
        relayed.sourceProvider = "Wolfx";
        relayed.reportTime = now - 5000;
        relayed.magnitude = 5.5;
        controller.handleEvent(relayed, false, true);

        // 同机构、20 s 后、约 4 km 外的另一场地震：事件 ID 不同 → 必须独立保留。
        EarthquakeEvent nearby = usgs;
        nearby.id = "pancakes_usgs_B";
        nearby.eventId = "usgs:us7000abce";
        nearby.timestamp = now + 20'000;
        nearby.reportTime = now + 20'000;
        nearby.latitude = 30.04; nearby.longitude = 104.0;
        nearby.magnitude = 4.1;
        controller.handleEvent(nearby, false, true);

        // 不同机构即使时间和震中相同，也要保留为独立记录。
        EarthquakeEvent jma = usgs;
        jma.id = "wolfx_jma_A";
        jma.eventId = "jma_eqlist:20240101";
        jma.sourceProvider = "Wolfx";
        jma.sourceAgency = "JMA";
        jma.source = "JMA 地震情报";
        jma.reportTime = now - 4000;
        jma.magnitude = 5.5;
        controller.handleEvent(jma, false, true);

        const QVariantList list = controller.eventList();
        QCOMPARE(list.size(), 3);

        QVariantMap merged;
        QVariantMap nearbyRow;
        for (const QVariant& entry : list) {
            const QVariantMap map = entry.toMap();
            if (map.value("magnitude").toDouble() == 5.6) merged = map;
            if (map.value("magnitude").toDouble() == 4.1) nearbyRow = map;
        }
        QVERIFY(!merged.isEmpty());
        QVERIFY(!nearbyRow.isEmpty());
        const QStringList sourceTags = merged.value("sourceTags").toStringList();
        QVERIFY(sourceTags.contains(QStringLiteral("Pancakes·USGS")));
        QVERIFY(sourceTags.contains(QStringLiteral("Wolfx·USGS")));
        // 邻近事件没有被并入上面那条：时刻、震级都是它自己的。
        QCOMPARE(nearbyRow.value("timestamp").toLongLong(), now + 20'000);
        QCOMPARE(nearbyRow.value("sourceTags").toStringList(),
                 QStringList{QStringLiteral("Pancakes·USGS")});
    }

    /// CENC 会按分发链路给同一场地震不同的 EventID（中段是各链路的报文时刻，不是发震
    /// 时刻）：2026-10-22 22:33:39 的 M7.0 在 Wolfx 是 `AU.20261022223344.0`、在 Jian 是
    /// `CC.20261022224454.0`。ID 对不上，但同机构 + 同发震时刻 + 同震中仍是同一事件，
    /// 必须合成一条；同秒却震中相距很远的记录照旧各占一行。
    void catalogMergesCencCrossChannelIds() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("CatalogCencIdTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);

        EarthquakeEvent wolfx;
        wolfx.id = "cenc_AU.20261022223344.0";
        wolfx.eventId = "AU.20261022223344.0";
        wolfx.source = "中国地震台网 地震信息";
        wolfx.sourceProvider = "Wolfx";
        wolfx.sourceAgency = "CENC";
        wolfx.sourceId = "wolfx";
        wolfx.timestamp = now;
        wolfx.reportTime = now - 600'000;
        wolfx.latitude = 32.40;
        wolfx.longitude = 93.79;
        wolfx.depth = 10.0;
        wolfx.magnitude = 7.0;
        wolfx.location = "西藏那曲市巴青县";
        wolfx.maxIntensityRaw = 9.0;
        wolfx.maxIntensityText = "IX";
        controller.handleEvent(wolfx, false, true);

        // 同一份报文的另一条链路：只有上游 ID 不同。
        EarthquakeEvent jian = wolfx;
        jian.id = "cenc_CC.20261022224454.0";
        jian.eventId = "CC.20261022224454.0";
        jian.sourceProvider = "Jian";
        jian.sourceId = "jian";
        jian.maxIntensityRaw = 0.0;
        jian.maxIntensityText.clear();
        controller.handleEvent(jian, false, true);

        // 同机构、同一秒、但震中 700 km 外：不是同一场地震，必须独立成行。
        EarthquakeEvent other = wolfx;
        other.id = "cenc_AU.20261022223344.7";
        other.eventId = "AU.20261022223344.7";
        other.latitude = 39.20;
        other.longitude = 100.10;
        other.magnitude = 3.2;
        controller.handleEvent(other, false, true);

        const QVariantList list = controller.eventList();
        QCOMPARE(list.size(), 2);

        QVariantMap merged;
        QVariantMap otherRow;
        for (const QVariant& entry : list) {
            const QVariantMap map = entry.toMap();
            if (map.value("magnitude").toDouble() == 7.0) merged = map;
            if (map.value("magnitude").toDouble() == 3.2) otherRow = map;
        }
        QVERIFY(!merged.isEmpty());
        QVERIFY(!otherRow.isEmpty());
        const QStringList tags = merged.value("sourceTags").toStringList();
        QCOMPARE(tags.size(), 2);
        QVERIFY(tags.contains(QStringLiteral("Wolfx·CENC")));
        QVERIFY(tags.contains(QStringLiteral("Jian·CENC")));
        QCOMPARE(otherRow.value("sourceTags").toStringList(),
                 QStringList{QStringLiteral("Wolfx·CENC")});
    }

    /// 同一条速报在两条链路里的秒数处理不同：Wolfx 的 cenc_eqlist 保留秒
    /// （`16:47:57`），Whews / Jian 只到分钟（`16:47:00`），上游 ID 也各不相同。
    /// 列表里必须仍然是**一行**（来源标签两条），否则同一场地震会被显示两次。
    /// 分钟相同但震中相距 100 km 的另一场地震照旧独立。
    void catalogMergesMinuteTruncatedInfoLinks() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("CatalogMinuteMergeTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        const long long minute = (now / 60'000) * 60'000;

        EarthquakeEvent wolfx;
        wolfx.id = "cenc_CD.20261024164757.9";
        wolfx.eventId = "CD.20261024164757.9";
        wolfx.source = "中国地震台网 地震信息";
        wolfx.sourceProvider = "Wolfx";
        wolfx.sourceAgency = "CENC";
        wolfx.sourceId = "wolfx";
        wolfx.timestamp = minute + 57'000;   // 16:47:57，带秒
        wolfx.reportTime = minute + 60'000;
        wolfx.latitude = 32.54;
        wolfx.longitude = 94.00;
        wolfx.depth = 10.0;
        wolfx.magnitude = 3.4;
        wolfx.location = "青海玉树州杂多县";
        wolfx.isInfo = true;
        wolfx.determination = "reviewed";
        controller.handleEvent(wolfx, false, true);

        // 同一条报文的另一条链路：ID 不同、发震时刻被截断到分钟、震中小幅抖动。
        EarthquakeEvent whews = wolfx;
        whews.id = "whews_20261024164700000";
        whews.eventId = "20261024164700000";
        whews.sourceProvider = "Whews";
        whews.sourceId = "whews";
        whews.timestamp = minute;
        whews.latitude = 32.56;
        whews.longitude = 94.02;
        controller.handleEvent(whews, false, true);

        // 同一分钟、同一机构，但震中相距约 110 km：不是同一场地震。
        EarthquakeEvent other = wolfx;
        other.id = "cenc_AU.20261024164758.0";
        other.eventId = "AU.20261024164758.0";
        other.latitude = 33.5;
        other.longitude = 94.0;
        other.magnitude = 4.2;
        controller.handleEvent(other, false, true);

        const QVariantList list = controller.eventList();
        QCOMPARE(list.size(), 2);
        QVariantMap merged;
        QVariantMap far;
        for (const QVariant& entry : list) {
            const QVariantMap map = entry.toMap();
            if (map.value("magnitude").toDouble() == 3.4) merged = map;
            if (map.value("magnitude").toDouble() == 4.2) far = map;
        }
        QVERIFY(!merged.isEmpty());
        QVERIFY(!far.isEmpty());
        const QStringList tags = merged.value("sourceTags").toStringList();
        QCOMPARE(tags.size(), 2);
        QVERIFY(tags.contains(QStringLiteral("Wolfx·CENC")));
        QVERIFY(tags.contains(QStringLiteral("Whews·CENC")));
        // 情报在列表里不带「第 N 报」，而是标「情报」。
        QCOMPARE(merged.value("reportNumText").toString(), QStringLiteral("情报"));
        QCOMPARE(far.value("sourceTags").toStringList(), QStringList{QStringLiteral("Wolfx·CENC")});
    }

    /// 普通事件的波前保持实线；震中下方浮动信息照旧。虚线只属于推算（见
    /// estimateFocusOutranksUsgsAndGq 的虚线断言），不能把两者画混。
    void eventWavefrontsStaySolidAndEpicenterInfoShowsBelowMarker() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("DashedWaveTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setBasemapId("vector");
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        auto* pWave = window->findChild<QQuickItem*>("pWaveRing");
        auto* sWave = window->findChild<QQuickItem*>("sWaveRing");
        auto* sFill = window->findChild<QQuickItem*>("sWaveFill");
        auto* info = window->findChild<QQuickItem*>("epicenterInfo");
        QVERIFY(map && pWave && sWave && sFill && info);
        QTest::qWait(400);

        // 40 秒前的 M7.5：P 波半径足够大，像素级判据才有效，且尚未淡出。
        EarthquakeEvent e;
        e.id = e.eventId = "SOLID-A";
        e.sourceProvider = "Wolfx"; e.sourceAgency = "CENC"; e.source = "中国地震台网";
        e.timestamp = now - 40'000;
        e.latitude = 30.0; e.longitude = 104.0;
        e.depth = 10.0; e.magnitude = 7.5; e.location = "四川雅安";
        controller.handleEvent(e, false, false);
        QTest::qWait(900);   // 等入场取景补间结束

        QVERIFY2(controller.property("wavePKm").toDouble() > 0.0, "P 波半径必须已算出");
        QVERIFY2(controller.property("wavePOpacity").toDouble() > 0.5, "P 波必须可见");
        QVERIFY2(!pWave->property("dashed").toBool(), "普通事件的 P 波必须是实线");
        QVERIFY2(!sWave->property("dashed").toBool(), "普通事件的 S 波必须是实线");
        QVERIFY2(sFill->isVisible(), "普通事件的 S 波渐变填充仍在");
        // 信息块默认不显示，指针悬停到震中十字上才出现。
        const double hx = map->property("hypocenterX").toDouble();
        const double hy = map->property("hypocenterY").toDouble();
        const QPointF epicenter = map->mapToScene(QPointF(hx, hy));
        QVERIFY2(!info->isVisible(), "信息块默认不该常驻，只应在悬停震中时显示");
        // 模糊必须采样地图图层（而不是整棵树，否则会把浮层自己采进去）。
        QCOMPARE(info->property("backdropSource").value<QQuickItem*>(),
                 map->property("renderLayer").value<QQuickItem*>());
        QTest::mouseMove(window, epicenter.toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(info->isVisible(), 500);
        QCOMPARE(info->property("placeText").toString(), QStringLiteral("四川雅安"));
        // 卡片必须带上发震时刻（与 HUD 同源的 UTC+8 文本），不能只有地名/震级。
        auto* infoTime = window->findChild<QQuickItem*>("epicenterInfoTime");
        QVERIFY(infoTime);
        const QString timeLine = infoTime->property("text").toString();
        QVERIFY2(timeLine.contains("UTC+8") && timeLine.size() >= 21,
                 qPrintable(QStringLiteral("震中卡片缺少发震时刻: \"%1\"").arg(timeLine)));

        // 实线像素判据：沿 P 波圆右半侧采样，实线几乎每格都有蓝色；若误改成虚线，
        // 命中率会掉到 0.6 左右。只断言属性抓不到「strokeStyle 写错」。
        const double radiusPx = controller.property("wavePKm").toDouble() *
                                map->property("pxPerKmValue").toDouble();
        QVERIFY2(radiusPx > 40.0, "P 波圆太小，无法验证实线");
        const double hitRatio = blueRingHitRatio(window, epicenter, radiusPx);
        QVERIFY2(hitRatio > 0.90,
                 qPrintable(QStringLiteral("普通事件 P 波不是实线（命中率 %1）").arg(hitRatio)));

        // 信息块在十字下方，且不越出窗口。
        QVERIFY2(info->y() > hy, "信息块应位于震中下方");
        QVERIFY(info->y() + info->height() <= map->height() + 1);
        QVERIFY(info->x() >= -1);
        QVERIFY(info->x() + info->width() <= map->width() + 1);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window) + "/46-event-wavefronts-solid.png"));

        // 指针移开后信息块收起。
        QTest::mouseMove(window, QPoint(4, 4));
        QTRY_VERIFY(!info->isVisible());

        // 焦点清掉后不再标注（历史事件另行显示十字也无所谓，信息块只跟当前事件）。
        controller.clearMapFocus();
        QTRY_VERIFY(!info->isVisible());
    }

    /// 推算也参与聚焦：USGS/GQ 占着镜头时，本地推算解出现就接管；权威事件到达后让位。
    void estimateFocusOutranksUsgsAndGq() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EstimateFocusTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        controller.settings()->setEnableSoundAlert(false);
        controller.settings()->setEnableWarnings(true);
        controller.settings()->setLocalIntensityFilter(0.0);
        controller.settings()->setSourceEstimationEnabled(true);
        controller.settings()->setDeveloperMode(true);
        controller.settings()->setSourceEstimationRegionEnabled("china", true);

        QSignalSpy camera(&controller, &AppController::centerMapRequested);
        EarthquakeEvent gq;
        gq.id = gq.eventId = "GQ-FOCUS";
        gq.sourceProvider = "Pancakes"; gq.sourceAgency = "GQ";
        gq.source = "GlobalQuake地震信息";
        gq.timestamp = now;
        gq.latitude = 45.0; gq.longitude = 150.0;
        gq.magnitude = 5.0; gq.location = "远东";
        controller.handleEvent(gq, false, false);
        QCOMPARE(controller.mapEvent().toMap().value("id").toString(),
                 QString::fromStdString(gq.identity()));

        std::vector<StationSnapshot::Entry> frame;
        std::vector<StationSnapshot::StationMeta> metas;
        for (double lat = 28.4; lat <= 31.6 + 1e-9; lat += 0.2) {
            for (double lon = 102.4; lon <= 105.6 + 1e-9; lon += 0.2) {
                StationSnapshot::Entry entry;
                entry.lat = lat;
                entry.lon = lon;
                entry.csis = true;
                entry.raw = SourceEstimator::predictedIntensity(
                    IntensityFamily::Csis, 5.8,
                    QuakeCalculator::haversineDistance(30.0, 104.0, lat, lon), 10.0);
                entry.meta = static_cast<int>(frame.size());
                frame.push_back(entry);
                StationSnapshot::StationMeta meta;
                meta.source = QStringLiteral("PancakesSeis");
                metas.push_back(meta);
            }
        }
        controller.stationSnapshot_->setEntries(frame, metas);
        controller.sourceEstimation_->ingestObservations(frame);
        controller.sourceEstimation_->runEstimation();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.focusEstimate_.isEmpty(), 5000);
        if (controller.sourceEstimation_->watcher_.isRunning())
            controller.sourceEstimation_->watcher_.waitForFinished();
        QCoreApplication::processEvents();

        const QVariantMap estimate = controller.focusEstimate_;
        QCOMPARE(estimate.value("id").toString(), QStringLiteral("china"));
        QVERIFY(std::abs(estimate.value("latitude").toDouble() - 30.0) < 2.0);
        QVERIFY(std::abs(estimate.value("longitude").toDouble() - 104.0) < 2.0);
        // 主 HUD 让位（由推算卡做唯一主展示），但活动预警本身仍在会话里。
        QVERIFY2(!controller.hudEvent().isValid(), "推算焦点期间主 HUD 应隐藏");
        QVERIFY(controller.hasWarning());
        // 镜头被带到推定震中，而不是 GQ 的远东位置。
        QVERIFY(!camera.isEmpty());
        const QList<QVariant> last = camera.last();
        QVERIFY(std::abs(last.at(0).toDouble() - 30.0) < 2.0);
        QVERIFY(std::abs(last.at(1).toDouble() - 104.0) < 2.0);

        // QML 侧：镜头真的落在推定震中上，事件十字/主 HUD 让位，推算卡成为主展示。
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* map = window->findChild<QQuickItem*>("mapView");
        auto* hud = window->findChild<QQuickItem*>("hudCard");
        auto* info = window->findChild<QQuickItem*>("epicenterInfo");
        auto* estimateCard = window->findChild<QQuickItem*>("sourceEstimateCard");
        QVERIFY(map && hud && info && estimateCard);
        QTRY_VERIFY(map->property("estimateFocused").toBool());
        QTest::qWait(900);   // 等取景补间结束
        QVERIFY2(std::abs(map->property("centerLon").toDouble() - 104.0) < 3.0,
                 qPrintable(QStringLiteral("镜头未跟到推定震中: centerLon=%1")
                                .arg(map->property("centerLon").toDouble())));
        QVERIFY2(std::abs(map->property("centerLat").toDouble() - 30.0) < 3.0,
                 qPrintable(QStringLiteral("镜头未跟到推定震中: centerLat=%1")
                                .arg(map->property("centerLat").toDouble())));
        QVERIFY2(!info->isVisible(), "推算焦点期间不应再画事件十字信息");
        QVERIFY2(!hud->isVisible(), "推算焦点期间主 HUD 应让位");
        QTRY_VERIFY(estimateCard->isVisible());
        // 推定震中标记必须落在镜头所指的震中附近（回归：曾只注入 shifted 没写 x/y，
        // 四个区域标记全叠在 (0,0)，镜头却飞到看不见标记的地方）。
        auto* marker = window->findChild<QQuickItem*>("sourceEstimateMarker");
        QVERIFY(marker && marker->isVisible());
        const QPointF markerCenter =
            marker->mapToScene(QPointF(marker->width() / 2, marker->height() / 2));
        QVERIFY2(markerCenter.x() > 200 && markerCenter.x() < window->width() - 100,
                 qPrintable(QStringLiteral("推定震中标记 x 不在视口内: %1").arg(markerCenter.x())));
        QVERIFY2(markerCenter.y() > 200 && markerCenter.y() < window->height() - 100,
                 qPrintable(QStringLiteral("推定震中标记 y 不在视口内: %1").arg(markerCenter.y())));

        // 推算波前：虚线、圆心就是推定震中、颜色仍用普通的 pWave/sWave。
        QTRY_VERIFY_WITH_TIMEOUT(controller.property("wavePKm").toDouble() > 0.0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.property("wavePOpacity").toDouble() > 0.3, 2000);
        auto* pWave = window->findChild<QQuickItem*>("pWaveRing");
        auto* sWave = window->findChild<QQuickItem*>("sWaveRing");
        auto* sFill = window->findChild<QQuickItem*>("sWaveFill");
        QVERIFY(pWave && sWave && sFill);
        QVERIFY2(pWave->isVisible(), "推算焦点的 P 波必须可见");
        QVERIFY2(pWave->property("dashed").toBool(), "推算 P 波必须是虚线");
        QVERIFY2(sWave->property("dashed").toBool(), "推算 S 波必须是虚线");
        QVERIFY2(!sFill->isVisible(), "推算焦点不画 S 波实色填充，否则会盖住虚线边界");
        QCOMPARE(qRound(pWave->x() + pWave->width() / 2),
                 qRound(marker->x() + marker->width() / 2));
        QCOMPARE(qRound(pWave->y() + pWave->height() / 2),
                 qRound(marker->y() + marker->height() / 2));
        const double waveRadius = pWave->width() / 2.0;
        QVERIFY2(waveRadius > 40.0, "推算 P 波圆太小，无法验证虚线");
        const QPointF waveCenter =
            map->mapToScene(QPointF(pWave->x() + pWave->width() / 2,
                                    pWave->y() + pWave->height() / 2));
        const double waveHit = blueRingHitRatio(window, waveCenter, waveRadius);
        QVERIFY2(waveHit > 0.30 && waveHit < 0.85,
                 qPrintable(QStringLiteral("推算 P 波不是虚线（命中率 %1）").arg(waveHit)));

        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window) + "/47-estimate-focus.png"));

        // 权威事件到达：推算让位，事件重新成为焦点。
        now += 60'000;
        EarthquakeEvent jma;
        jma.id = jma.eventId = "JMA-FOCUS";
        jma.sourceProvider = "Whews"; jma.sourceAgency = "JMA";
        jma.source = "JMA 紧急地震速报";
        jma.timestamp = now;
        jma.latitude = 36.0; jma.longitude = 140.0;
        jma.magnitude = 6.0; jma.location = "千叶县东方海域";
        controller.handleEvent(jma, false, false);
        QVERIFY(controller.focusEstimate_.isEmpty());
        QCOMPARE(controller.mapEvent().toMap().value("id").toString(),
                 QString::fromStdString(jma.identity()));
        QVERIFY(controller.hudEvent().isValid());
    }

    /// 详情页数据：同一报经两路聚合源送达只算一报、来源全部保留；后报按报数排序；
    /// 原始 JSON 与「≥1 度」的区县表都要能直接给 QML 用。
    void eventDetailAggregatesReportsAndSources() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };

        const auto make = [&](const QString& provider, const QString& id, int reportNum,
                              const QString& raw) {
            EarthquakeEvent e;
            e.id = "wolfx_" + id.toStdString();
            e.eventId = id.toStdString();
            e.sourceProvider = provider.toStdString();
            e.sourceAgency = "CENC";
            e.source = "中国地震台网 地震信息";
            e.timestamp = now;
            e.reportTime = now;
            e.reportNum = reportNum;
            e.magnitude = reportNum == 1 ? 5.5 : 6.0;
            e.depth = 10.0;
            e.latitude = 30.0;
            e.longitude = 103.0;
            e.location = "四川测试震中";
            e.distanceKm = -1.0;
            e.estimatedIntensity = "--";
            e.rawJson = raw.toStdString();
            e.sourceId = provider == "Wolfx" ? "wolfx" : "pancakes";
            return e;
        };

        const EarthquakeEvent first = make("Wolfx", "A", 1, R"({"src":"wolfx","n":1})");
        controller.handleEvent(first, false, true);
        // 同一份报文经 Pancakes 再送一次：EventGate 会判重复，但归档仍要留下这一路来源。
        controller.handleEvent(make("Pancakes", "A", 1, R"({"src":"pancakes","n":1})"),
                               false, true);
        controller.handleEvent(make("Wolfx", "A", 2, R"({"src":"wolfx","n":2})"), false, true);

        const QString id = QString::fromStdString(first.identity());
        controller.showEventDetail(id);
        const QVariantMap detail = controller.eventDetail();
        QCOMPARE(detail.value("id").toString(), id);
        QCOMPARE(detail.value("location").toString(), QStringLiteral("四川测试震中"));

        const QVariantList sources = detail.value("sources").toList();
        QCOMPARE(sources.size(), 2);
        bool wolfxHasRaw = false;
        bool pancakesHasRaw = false;
        for (const QVariant& value : sources) {
            const QVariantMap source = value.toMap();
            const QString tag = source.value("tag").toString();
            QVERIFY2(!source.value("firstReceivedText").toString().isEmpty(),
                     "每个来源都要有接收时间");
            if (tag == QStringLiteral("Wolfx·CENC")) {
                wolfxHasRaw = source.value("hasRaw").toBool()
                              && source.value("rawJson").toString().contains("\"n\":2");
                QCOMPARE(source.value("reportCount").toInt(), 2);
            }
            if (tag == QStringLiteral("Pancakes·CENC")) {
                pancakesHasRaw = source.value("hasRaw").toBool();
                QCOMPARE(source.value("reportCount").toInt(), 1);
            }
        }
        QVERIFY2(wolfxHasRaw, "Wolfx 应保留最新一报的原始 JSON");
        QVERIFY2(pancakesHasRaw, "Pancakes 的重复报也应留下来源与原文");

        const QVariantList reports = detail.value("reports").toList();
        QCOMPARE(reports.size(), 2);   // 同一报的两路来源合并，不重复计数
        QCOMPARE(reports.at(0).toMap().value("numText").toString(), QStringLiteral("第 1 报"));
        QCOMPARE(reports.at(0).toMap().value("sourceTags").toStringList().size(), 2);
        QCOMPARE(reports.at(1).toMap().value("numText").toString(), QStringLiteral("第 2 报"));
        QCOMPARE(reports.at(1).toMap().value("statusText").toString(), QString());

        // 区县烈度表只保留 ≥1 度（0.x 一律不出现）。
        const QVariantList districts = detail.value("districts").toList();
        QVERIFY2(!districts.isEmpty(), "M6.0 四川震中周边应有 ≥1 度区县");
        for (const QVariant& value : districts)
            QVERIFY(value.toMap().value("raw").toDouble() >= 1.0);

        controller.closeEventDetail();
        QVERIFY(controller.eventDetail().isEmpty());
    }

    /// 同一场地震的**实时预警**与**地震情报**是两条链路，必须落进同一份报次归档：
    /// 预警按报数逐条排，情报单独标「情报」并区分自动/正式测定。发震时刻口径不同
    /// （预警到秒、情报截断到分钟）不能把它们拆成两组。
    void eventDetailMergesEewAndInfoReports() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailMergeTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        // 预警发震时刻带秒（16:29:58），情报只有分钟（16:29:00）。
        const long long origin = 1'800'000'000'000LL;
        const long long minute = (origin / 60'000) * 60'000;

        const auto make = [&](bool isInfo, int reportNum, long long stamp, double latitude,
                              const QString& id) {
            EarthquakeEvent e;
            e.id = "wolfx_" + id.toStdString();
            e.eventId = id.toStdString();
            e.sourceProvider = "Wolfx";
            e.sourceAgency = "CENC";
            e.source = isInfo ? "中国地震台网 地震信息" : "中国地震预警网 地震预警";
            e.timestamp = stamp;
            e.reportTime = stamp + 30'000;
            e.reportNum = reportNum;
            e.magnitude = 5.0 + reportNum * 0.1;
            e.depth = 10.0;
            e.latitude = latitude;
            e.longitude = 103.0;
            e.location = "合并测试震中";
            e.distanceKm = -1.0;
            e.estimatedIntensity = "--";
            e.isInfo = isInfo;
            e.isFinal = isInfo;
            e.rawJson = QByteArray(R"({"id":"")") + id.toUtf8() + QByteArray("\"}");
            e.sourceId = "wolfx";
            return e;
        };

        controller.handleEvent(make(false, 1, origin, 30.0, "M1"), false, false);
        controller.handleEvent(make(false, 2, origin, 30.0, "M1"), false, false);
        // 情报：同一场地震，发震时刻被截断到分钟，震中抖动 0.03°。
        EarthquakeEvent automatic = make(true, 1, minute, 30.03, "M1");
        automatic.determination = "automatic";
        controller.handleEvent(automatic, false, true);
        // 正式测定：同一份情报的第二次定稿，发布时刻更晚 → 单独一行。
        EarthquakeEvent reviewed = make(true, 1, minute, 30.03, "M1");
        reviewed.determination = "reviewed";
        reviewed.reportTime += 600'000;
        reviewed.magnitude = 5.3;
        controller.handleEvent(reviewed, false, true);

        const QString id = QString::fromStdString(make(false, 1, origin, 30.0, "M1").identity());
        controller.showEventDetail(id);
        const QVariantMap detail = controller.eventDetail();
        QCOMPARE(detail.value("id").toString(), id);

        const QVariantList reports = detail.value("reports").toList();
        QVERIFY2(reports.size() == 4,
                 qPrintable(QStringLiteral("预警 2 报 + 情报 2 条应各自成行，实际 %1 行")
                                .arg(reports.size())));
        // 预警在前，按报数升序。
        QCOMPARE(reports.at(0).toMap().value("numText").toString(), QStringLiteral("第 1 报"));
        QCOMPARE(reports.at(1).toMap().value("numText").toString(), QStringLiteral("第 2 报"));
        QCOMPARE(reports.at(1).toMap().value("isInfo").toBool(), false);
        // 情报不带报数，标「情报」并用测定状态区分自动/正式。
        for (int i = 2; i < 4; ++i) {
            QCOMPARE(reports.at(i).toMap().value("numText").toString(), QStringLiteral("情报"));
            QCOMPARE(reports.at(i).toMap().value("isInfo").toBool(), true);
        }
        QCOMPARE(reports.at(2).toMap().value("determinationText").toString(),
                 QStringLiteral("自动测定"));
        QCOMPARE(reports.at(3).toMap().value("determinationText").toString(),
                 QStringLiteral("正式测定"));

        // 另一场地震（同时刻、震中差 0.5°≈55 km）不得并进来。
        controller.handleEvent(make(true, 1, minute, 30.5, "M2"), false, true);
        controller.showEventDetail(id);
        QCOMPARE(controller.eventDetail().value("reports").toList().size(), 4);

        // QML：展开「报数」后必须真的画出「情报」与测定状态，而不是四条「第 N 报」。
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showList", true);
        QTRY_VERIFY(controller.eventModel()->rowCount() >= 1);
        auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
        QVERIFY(sidebar);
        sidebar->setProperty("detailEventId", id);
        auto* panel = window->findChild<QQuickItem*>("eventDetailPanel");
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        panel->setProperty("reportsExpanded", true);
        QTest::qWait(150);
        int infoLabels = 0;
        bool sawReviewText = false;
        bool sawAutoText = false;
        for (auto* item : visualItems(panel)) {
            const QString text = item->property("text").toString();
            if (text == QStringLiteral("情报")) ++infoLabels;
            if (text == QStringLiteral("正式测定")) sawReviewText = true;
            if (text == QStringLiteral("自动测定")) sawAutoText = true;
        }
        QCOMPARE(infoLabels, 2);
        QVERIFY2(sawReviewText, "情报行缺少「正式测定」标注");
        QVERIFY2(sawAutoText, "情报行缺少「自动测定」标注");
        // 取证：把「报数」区滚进视口（报数在页尾，直接拉到 contentHeight 会越界成空白）。
        auto* scroll = window->findChild<QQuickItem*>("eventDetailScroll");
        QVERIFY(scroll);
        QQuickItem* infoRow = nullptr;
        for (auto* item : visualItems(panel)) {
            if (item->property("text").toString() == QStringLiteral("自动测定")) {
                infoRow = item;
                break;
            }
        }
        QVERIFY(infoRow);
        const QPointF rowY = infoRow->mapToItem(scroll->property("contentItem").value<QQuickItem*>(),
                                                QPointF(0, 0));
        const double maxY = scroll->property("contentHeight").toReal()
                            - scroll->property("height").toReal();
        scroll->setProperty("contentY", qBound(0.0, rowY.y() - 260.0, qMax(0.0, maxY)));
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/60-event-detail-eew-and-info.png"));

        controller.closeEventDetail();
    }

    /// 回归：详情面板里三处「没对齐」。
    ///
    /// 用户报的是三处像素级错位，成因却是三个不同的东西：
    ///   ① 数据源行 `height: 52` 写死，而左侧「来源名 + 接收时间」需要 37px 内容高，
    ///      加 12+12 边距共 61px。行矮了 9px，布局器只能把超高的子项钉在行顶，
    ///      右侧 30px 按钮又按自己居中 —— 两个参照系，看着就是「原始 JSON 偏高」。
    ///   ② 报数标题的折叠箭头被当成**独立**布局项、按整行（26px）Qt.AlignVCenter 居中，
    ///      而文字是按基线摆的；15px 图标盒与 16px 文字盒各自居中差 1.3px，箭头看着偏高。
    ///   ③ 报次抬头用 `Row`（定位器只顶对齐）混排 12px「情报」与 10px「正式测定」，
    ///      盒高 18 对 15，顶对顶之后两条基线差 2.3px。
    /// 这些都不是「差一两个像素」的审美问题：基线不共线，一行字就会读成两行。
    /// 断言的是真实几何（mapToItem + baselineOffset），不是 QML 里写的对齐属性 ——
    /// 属性写对了但布局器没照做，才是这三处 bug 的实际形态。
    void eventDetailContentAlignsOnOneBaseline() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailAlignTest");
        AppController controller(nullptr, false);
        long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };

        const auto make = [&](int reportNum, double magnitude, const QString& id) {
            EarthquakeEvent e;
            e.id = "wolfx_" + id.toStdString();
            e.eventId = id.toStdString();
            e.sourceProvider = "Wolfx";
            e.sourceAgency = "CENC";
            e.source = "中国地震台网 地震信息";
            e.timestamp = now;
            e.reportTime = now + reportNum * 60'000;
            e.reportNum = reportNum;
            e.magnitude = magnitude;
            e.depth = 10.0;
            e.latitude = 30.0;
            e.longitude = 103.0;
            e.location = "对齐测试震中";
            e.distanceKm = -1.0;
            e.estimatedIntensity = "--";
            e.rawJson = R"({"type":"cenc_eew","ReportNum":1,"Magnitude":5.2,"depth":10})";
            e.sourceId = "wolfx";
            return e;
        };
        // 一条带原文的预警报 + 自动测定 / 正式测定两条情报：三种抬头文案都覆盖到。
        controller.handleEvent(make(1, 5.2, "ALIGN"), false, false);
        EarthquakeEvent automatic = make(1, 5.1, "ALIGN");
        automatic.isInfo = true;
        automatic.determination = "automatic";
        controller.handleEvent(automatic, false, true);
        EarthquakeEvent reviewed = make(1, 5.3, "ALIGN");
        reviewed.isInfo = true;
        reviewed.determination = "reviewed";
        reviewed.reportTime += 600'000;
        controller.handleEvent(reviewed, false, true);

        const QString id = QString::fromStdString(make(1, 5.2, "ALIGN").identity());
        controller.showEventDetail(id);
        QCOMPARE(controller.eventDetail().value("id").toString(), id);

        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showList", true);
        auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
        QVERIFY(sidebar);
        sidebar->setProperty("detailEventId", id);
        auto* panel = window->findChild<QQuickItem*>("eventDetailPanel");
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        panel->setProperty("reportsExpanded", true);
        QTRY_VERIFY_WITH_TIMEOUT(sidebar->width() > 200, 5000);
        QTest::qWait(300);

        // 基线/中心一律换算到面板坐标系再比，避免不同父项各自本地坐标混进来。
        // 必须映射**中心点**而不是左上角：展开态的箭头带 `rotation: 180`，
        // 绕中心旋转会把左上角映射到右下角，小角点算出来的 y 会凭空多出一个图标高
        // （这正是这个用例第一版自己踩的坑）。中心点在绕中心旋转下不动。
        const auto centerIn = [](QQuickItem* item, QQuickItem* space) {
            return item->mapToItem(space, QPointF(item->width() / 2.0, item->height() / 2.0)).y();
        };
        const auto baselineIn = [&](QQuickItem* item, QQuickItem* space) {
            return centerIn(item, space) - item->height() / 2.0 + item->baselineOffset();
        };
        const auto texts = [&](const QString& text) {
            QList<QQuickItem*> hits;
            for (auto* item : visualItems(panel)) {
                if (item->property("text").toString() == text) hits.append(item);
            }
            return hits;
        };

        // ── ① 数据源行：高度要装得下内容，右侧按钮要和左侧文字块同心 ──────────
        auto* sourceRow = findVisualItem(window, QStringLiteral("eventDetailSourceRow"));
        QVERIFY2(sourceRow, "详情面板缺少数据源行");
        QQuickItem* sourceInner = nullptr;
        for (auto* child : sourceRow->childItems()) {
            if (child->metaObject()->className() == QLatin1String("QQuickRowLayout"))
                sourceInner = child;
        }
        QVERIFY2(sourceInner, "数据源行缺少内层 RowLayout");
        QVERIFY2(sourceRow->height() + 0.5 >= sourceInner->implicitHeight() + 24.0,
                 qPrintable(QStringLiteral("数据源行高 %1 装不下内容 %2 + 上下边距 24，"
                                           "布局器会把子项钉在行顶")
                                .arg(sourceRow->height())
                                .arg(sourceInner->implicitHeight())));
        auto* rawButton = findVisualItem(window, QStringLiteral("eventDetailSourceRawButton"));
        QVERIFY2(rawButton && rawButton->isVisible(), "带原文的来源缺少「原始 JSON」按钮");
        // 左侧文字块 = 内层布局里的 ColumnLayout。
        QQuickItem* sourceText = nullptr;
        for (auto* child : sourceInner->childItems()) {
            if (child->metaObject()->className() == QLatin1String("QQuickColumnLayout"))
                sourceText = child;
        }
        QVERIFY2(sourceText, "数据源行缺少左侧文字块");
        QVERIFY2(rawButton->y() >= 0.0 && rawButton->y() + rawButton->height()
                     <= sourceRow->height() + 0.01,
                 "「原始 JSON」按钮被挤出了数据源行");
        QVERIFY2(std::abs(centerIn(rawButton, panel) - centerIn(sourceText, panel)) <= 0.5,
                 qPrintable(QStringLiteral("「原始 JSON」与左侧文字块不同心：差 %1 px")
                                .arg(centerIn(rawButton, panel) - centerIn(sourceText, panel))));

        // ── ⓪ 面板标题行：20px 标题与 11px「实时告警中」共基线 ─────────────────
        // 同一类错误的第四处：两个 Text 都不写 alignment 时按行顶对齐，
        // 字重/字形 ascent 不同，实测基线差 4.4px。
        {
            const auto titleHits = texts(QStringLiteral("地震详情"));
            QVERIFY2(!titleHits.isEmpty(), "详情面板缺少「地震详情」标题");
            QQuickItem* statusText = nullptr;
            for (auto* item : visualItems(panel)) {
                if (item->property("text").toString() == QStringLiteral("实时告警中"))
                    statusText = item;
            }
            QVERIFY2(statusText, "活动预警时标题行缺少「实时告警中」");
            QVERIFY2(std::abs(baselineIn(titleHits.first(), panel) - baselineIn(statusText, panel))
                         <= 0.5,
                     qPrintable(QStringLiteral("「地震详情」与「实时告警中」不在一条基线上：差 %1 px")
                                    .arg(baselineIn(statusText, panel)
                                         - baselineIn(titleHits.first(), panel))));
        }

        // ── ② 报数标题：折叠文字与「报数」共基线，箭头与文字同心 ───────────────
        const auto titleHits = texts(QStringLiteral("报数"));
        QVERIFY2(!titleHits.isEmpty(), "详情面板缺少「报数」标题");
        QQuickItem* toggleLabel =
            findVisualItem(window, QStringLiteral("eventDetailReportToggleLabel"));
        QVERIFY2(toggleLabel, "报数标题缺少展开/收起文字");
        QVERIFY2(std::abs(baselineIn(titleHits.first(), panel) - baselineIn(toggleLabel, panel))
                     <= 0.5,
                 qPrintable(QStringLiteral("「%1」与「报数」不在一条基线上：差 %2 px")
                                .arg(toggleLabel->property("text").toString())
                                .arg(baselineIn(toggleLabel, panel) - baselineIn(titleHits.first(), panel))));
        auto* toggleIcon = findVisualItem(window, QStringLiteral("eventDetailReportToggleIcon"));
        QVERIFY2(toggleIcon, "报数标题缺少折叠箭头");
        QVERIFY2(std::abs(centerIn(toggleIcon, panel) - centerIn(toggleLabel, panel)) <= 0.5,
                 qPrintable(QStringLiteral("折叠箭头与「%1」不同心：差 %2 px")
                                .arg(toggleLabel->property("text").toString())
                                .arg(centerIn(toggleIcon, panel) - centerIn(toggleLabel, panel))));

        // ── ③ 报次抬头：「情报」与测定状态必须是同一条基线 ─────────────────────
        const auto determinationHits = texts(QStringLiteral("正式测定"));
        QVERIFY2(!determinationHits.isEmpty(), "展开后缺少「正式测定」标注");
        int compared = 0;
        for (auto* determination : determinationHits) {
            QQuickItem* headerRow = determination->parentItem();
            QVERIFY(headerRow);
            QQuickItem* numText = nullptr;
            for (auto* sibling : headerRow->childItems()) {
                if (sibling == determination) continue;
                const QString text = sibling->property("text").toString();
                if (text == QStringLiteral("情报") || text.startsWith(QStringLiteral("第 ")))
                    numText = sibling;
            }
            QVERIFY2(numText, "报次抬头缺少报数/情报文字");
            ++compared;
            QVERIFY2(std::abs(baselineIn(numText, panel) - baselineIn(determination, panel)) <= 0.5,
                     qPrintable(QStringLiteral("「%1」与「正式测定」不在一条基线上：差 %2 px")
                                    .arg(numText->property("text").toString())
                                    .arg(baselineIn(numText, panel) - baselineIn(determination, panel))));
        }
        QVERIFY2(compared >= 1, "没有可比对的报次抬头");

        // ── 报次卡片：高度同样由内容撑开，抬头两行不能被裁 ─────────────────────
        QList<QQuickItem*> reportRows;
        for (auto* item : visualItems(panel)) {
            if (item->objectName() == QStringLiteral("eventDetailReportRow") && item->isVisible())
                reportRows.append(item);
        }
        QVERIFY2(reportRows.size() >= 2, "展开后应至少有两个报次卡片");
        for (auto* row : reportRows) {
            // 卡片里是一列（x=12/y=10 的 Column），内容高度 + 上下各 10 必须装得下。
            QQuickItem* column = nullptr;
            for (auto* child : row->childItems()) {
                if (child->metaObject()->className() == QLatin1String("QQuickColumn"))
                    column = child;
            }
            QVERIFY2(column, "报次卡片缺少内层 Column");
            QVERIFY2(row->height() + 0.5 >= column->implicitHeight() + 20.0,
                     qPrintable(QStringLiteral("报次卡片高 %1 装不下内容 %2 + 上下边距 20")
                                    .arg(row->height())
                                    .arg(column->implicitHeight())));
        }

        // ── 摘要变长时不能把折叠开关挤出卡片 ───────────────────────────────────
        auto* summary = window->findChild<QQuickItem*>("eventDetailReportSummary");
        QVERIFY2(summary, "报数标题缺少测定状态摘要");
        QVERIFY2(!summary->property("text").toString().isEmpty(), "摘要应显示测定状态");
        const double summaryRight =
            summary->mapToItem(panel, QPointF(summary->width(), 0)).x();
        const double toggleLeft =
            toggleLabel->mapToItem(panel, QPointF(0, 0)).x();
        QVERIFY2(summaryRight <= toggleLeft + 0.01,
                 qPrintable(QStringLiteral("摘要把折叠开关压出了原位：摘要右缘 %1 > 「%2」左缘 %3")
                                .arg(summaryRight)
                                .arg(toggleLabel->property("text").toString())
                                .arg(toggleLeft)));
        const QPointF toggleRight =
            toggleIcon->mapToItem(panel, QPointF(toggleIcon->width(), 0));
        QVERIFY2(toggleRight.x() <= panel->width() + 0.01,
                 qPrintable(QStringLiteral("折叠开关被挤出面板：右缘 %1 > 面板宽 %2")
                                .arg(toggleRight.x())
                                .arg(panel->width())));

        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/71c-detail-alignment.png"));
        controller.closeEventDetail();
    }

    /// 交互：悬停卡片浮出两个按钮；「查看详情」切到详情面板，返回键回到列表。
    void eventCardHoverButtonsOpenDetailAndBack() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailUiTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };
        EarthquakeEvent catalog;
        catalog.id = "wolfx_HOVER";
        catalog.eventId = "HOVER";
        catalog.sourceProvider = "Wolfx";
        catalog.sourceAgency = "CENC";
        catalog.source = "中国地震台网 地震信息";
        catalog.timestamp = now;
        catalog.reportTime = now;
        catalog.latitude = 30.0;
        catalog.longitude = 103.0;
        catalog.magnitude = 5.2;
        catalog.location = "悬停测试震中";
        catalog.rawJson = R"({"type":"cenc_eew","ReportNum":1,"Magnitude":5.2,"isFinal":false,"location":"四川雅安","depth":10,"stations":[{"name":"A","value":1.2}]})";
        controller.handleEvent(catalog, false, true);
        const QString identity = QString::fromStdString(catalog.identity());

        QQmlApplicationEngine engine;
        QStringList qmlErrors;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) {
                const auto message = warning.toString();
                if (message.contains("EventDetailPanel.qml") || message.contains("EventTile.qml")
                    || message.contains("TypeError") || message.contains("ReferenceError")
                    || message.contains("Binding loop"))
                    qmlErrors.append(message);
            }
        });
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showList", true);
        QTRY_COMPARE(controller.eventModel()->rowCount(), 1);
        QTest::qWait(120);

        auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
        QVERIFY(sidebar);
        QQuickItem* tile = nullptr;
        for (auto* item : visualItems(sidebar)) {
            if (item->property("eventId").toString() == identity) {
                tile = item;
                break;
            }
        }
        QVERIFY(tile);
        QTest::mouseMove(window, QPoint(0, 0));
        QTest::mouseMove(window, tile->mapToScene(QPointF(tile->width() / 2, tile->height() / 2))
                                    .toPoint());
        QQuickItem* detailButton = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            detailButton = nullptr;
            for (auto* item : visualItems(sidebar)) {
                if (item->objectName() == QStringLiteral("eventDetailAction") && item->isVisible())
                    detailButton = item;
            }
            return detailButton != nullptr;
        })(), 1000);
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/49-event-card-hover-actions.png"));
        const QPoint clickPos =
            detailButton->mapToScene(QPointF(detailButton->width() / 2, detailButton->height() / 2))
                .toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, clickPos);
        QTRY_COMPARE(sidebar->property("detailEventId").toString(), identity);
        QTRY_COMPARE(controller.eventDetail().value("id").toString(), identity);
        auto* panel = window->findChild<QQuickItem*>("eventDetailPanel");
        QVERIFY(panel && panel->isVisible());
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window) + "/50-event-detail.png"));
        panel->setProperty("reportsExpanded", true);
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/51-event-detail-reports.png"));

        QQuickItem* rawButton = nullptr;
        for (auto* item : visualItems(panel)) {
            if (item->property("accessibleName").toString().endsWith(QStringLiteral("的原始 JSON"))
                && item->isVisible()) {
                rawButton = item;
                break;
            }
        }
        QVERIFY(rawButton);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          rawButton->mapToScene(QPointF(rawButton->width() / 2,
                                                        rawButton->height() / 2)).toPoint());
        QObject* rawPopup = panel->findChild<QObject*>("rawJsonPopup");
        QVERIFY(rawPopup);
        QTRY_VERIFY(rawPopup->property("visible").toBool());
        QTest::qWait(120);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window) + "/52-raw-json.png"));
        QMetaObject::invokeMethod(rawPopup, "close");

        auto* detailScroll = window->findChild<QQuickItem*>("eventDetailScroll");
        QVERIFY(detailScroll);
        detailScroll->setProperty("contentY",
                                  detailScroll->property("contentHeight").toReal()
                                      - detailScroll->height());
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/53-district-intensity.png"));

        QQuickItem* backButton = nullptr;
        for (auto* item : visualItems(panel)) {
            if (item->property("accessibleName").toString() == QStringLiteral("返回事件列表")
                && item->isVisible()) {
                backButton = item;
                break;
            }
        }
        QVERIFY(backButton);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          backButton->mapToScene(QPointF(backButton->width() / 2,
                                                         backButton->height() / 2)).toPoint());
        QTRY_COMPARE(sidebar->property("detailEventId").toString(), QString());
        QVERIFY(panel->isVisible() == false);
        QVERIFY2(qmlErrors.isEmpty(), qPrintable(qmlErrors.join('\n')));
    }

    /// 报次归档要跨进程重启存活：目录每场地震只留最新一报，历史报次全靠 event_reports。
    void eventDetailReportsSurviveRestart() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailPersistTest");
        const long long now = 1'800'000'000'000LL;
        QString identity;
        {
            AppController controller(nullptr, false);
            controller.nowProvider_ = [&] { return now; };
            EarthquakeEvent event;
            event.id = "wolfx_PERSIST";
            event.eventId = "PERSIST";
            event.sourceProvider = "Wolfx";
            event.sourceAgency = "CENC";
            event.source = "中国地震台网 地震信息";
            event.timestamp = now;
            event.reportTime = now;
            event.reportNum = 1;
            event.magnitude = 4.8;
            event.depth = 12.0;
            event.latitude = 31.0;
            event.longitude = 104.0;
            event.location = "重启测试震中";
            event.rawJson = R"({"ReportNum":1})";
            event.sourceId = "wolfx";
            identity = QString::fromStdString(event.identity());
            controller.handleEvent(event, false, true);
            event.reportNum = 2;
            event.magnitude = 5.0;
            event.isFinal = true;
            event.rawJson = R"({"ReportNum":2})";
            controller.handleEvent(event, false, true);
            controller.flushHistoryWrites();
        }
        {
            AppController controller(nullptr, false);
            controller.nowProvider_ = [&] { return now; };
            controller.showEventDetail(identity);
            const QVariantMap detail = controller.eventDetail();
            QCOMPARE(detail.value("id").toString(), identity);
            const QVariantList reports = detail.value("reports").toList();
            QCOMPARE(reports.size(), 2);
            QCOMPARE(reports.at(1).toMap().value("numText").toString(), QStringLiteral("第 2 报"));
            QCOMPARE(reports.at(1).toMap().value("statusText").toString(),
                     QStringLiteral("最终报"));
            const QVariantList sources = detail.value("sources").toList();
            QCOMPARE(sources.size(), 1);
            QVERIFY(sources.at(0).toMap().value("hasRaw").toBool());
            QVERIFY(sources.at(0).toMap().value("rawJson").toString().contains("ReportNum"));
        }
    }

    /// 升级前的历史记录没有报次归档，也没有原始报文：详情仍要能打开，
    /// 用 events 行里的最后一报合成一条报次，并明确告知原文未保存。
    void eventDetailFallsBackForLegacyRecords() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
        QCoreApplication::setOrganizationName("KomiraQuakeTest");
        QCoreApplication::setApplicationName("EventDetailLegacyTest");
        AppController controller(nullptr, false);
        const long long now = 1'800'000'000'000LL;
        controller.nowProvider_ = [&] { return now; };

        // 旧库形态：没有 provider/agency/eventId，也没有 event_reports。
        EarthquakeEvent legacy;
        legacy.id = "cenc_legacy";
        legacy.source = "中国地震台网 地震信息";
        legacy.timestamp = now;
        legacy.magnitude = 4.1;
        legacy.depth = 8.0;
        legacy.latitude = 29.5;
        legacy.longitude = 102.0;
        legacy.location = "旧库测试震中";
        legacy.reportNum = 1;
        legacy.isFinal = true;
        controller.history_.push_back(legacy);
        controller.rebuildHistory();

        const QString id = QString::fromStdString(legacy.identity());
        controller.showEventDetail(id);
        const QVariantMap detail = controller.eventDetail();
        QCOMPARE(detail.value("id").toString(), id);
        QCOMPARE(detail.value("location").toString(), QStringLiteral("旧库测试震中"));
        QCOMPARE(detail.value("reportsSynthesized").toBool(), true);

        // 没有 provider/agency 时退回 event.source 作为来源名，而不是「暂无来源记录」。
        const QVariantList sources = detail.value("sources").toList();
        QCOMPARE(sources.size(), 1);
        QCOMPARE(sources.at(0).toMap().value("tag").toString(),
                 QStringLiteral("中国地震台网 地震信息"));
        QCOMPARE(sources.at(0).toMap().value("hasRaw").toBool(), false);

        // 最后一报仍可查看（默认折叠由 QML 控制），但没有原文可开。
        const QVariantList reports = detail.value("reports").toList();
        QCOMPARE(reports.size(), 1);
        QCOMPARE(reports.at(0).toMap().value("numText").toString(), QStringLiteral("第 1 报"));
        QCOMPARE(reports.at(0).toMap().value("statusText").toString(),
                 QStringLiteral("最终报"));

        // QML：旧数据也要真的画出详情，并说明原文未保存。
        QQmlApplicationEngine engine;
        configureNativeUi(engine);
        engine.rootContext()->setContextProperty("app", &controller);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/KomiraQuake/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        window->setProperty("showList", true);
        QTRY_COMPARE(controller.eventModel()->rowCount(), 1);
        auto* sidebar = window->findChild<QQuickItem*>("eventSidebar");
        QVERIFY(sidebar);
        sidebar->setProperty("detailEventId", id);
        auto* panel = window->findChild<QQuickItem*>("eventDetailPanel");
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QTest::qWait(120);
        bool sawLegacyHint = false;
        bool sawSourceFallback = false;
        for (auto* item : visualItems(panel)) {
            const QString text = item->property("text").toString();
            if (text == QStringLiteral("旧数据仅保留最后一报，原始报文未保存。")) sawLegacyHint = true;
            if (text == QStringLiteral("中国地震台网 地震信息")) sawSourceFallback = true;
        }
        QVERIFY2(sawLegacyHint, "旧数据详情缺少「原文未保存」说明");
        QVERIFY2(sawSourceFallback, "旧数据详情缺少来源名回退");
        QVERIFY(window->grabWindow().save(evidenceDirectory(window)
                                          + "/54-legacy-event-detail.png"));
    }
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
