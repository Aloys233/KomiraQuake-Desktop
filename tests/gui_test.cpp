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
#include <functional>
#include <cmath>
#include "core/intensity_calculator.h"
#include "core/warning_session.h"
#include "prefs/settings_store.h"
#include "theme/native_ui.h"
#include <QQuickStyle>
#include <QSGRendererInterface>
#include <QQmlEngine>
#include "map/vector_map_item.h"
#include "map/station_map_item.h"
#include "model/station_snapshot.h"
#include "source/wolfx_source.h"
#include "store/history_store.h"
#define private public
#include "app/app_controller.h"
#include "service/alert_announcer.h"
#include "service/tray_controller.h"
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
        for (auto* item : warningHud->findChildren<QQuickItem*>())
            if (item->property("text").toString() == "停止本次提醒" && item->property("hovered").isValid()) alertButton = item;
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
        for (int section = 0; section < 8; ++section) {
            settingsPage->setProperty("currentSection", section);
            QTest::qWait(30);
            for (auto* item : visualItems(settingsPage)) {
                if (!item->isVisible() || item->width() == 0 || item->height() == 0) continue;
                if (item->metaObject()->indexOfSignal("clicked()") < 0 && item->objectName() != "manualLatitude" && item->objectName() != "manualLongitude") continue;
                const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                QVERIFY2(rect.left() >= 0 && rect.right() <= window->width() + 1, qPrintable(item->property("text").toString()));
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
        QVERIFY2(district->z() < hypocenter->z(), "hypocenter must be above district intensity shading");
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
            // VectorMapItem 首次 paint 才建离屏缓存（~90 ms/三层），且绘制结果要下一帧
            // 才进场景图。用 QTRY 反复抓帧直到出现内容 —— 若始终空白则超时失败，
            // 真·空白 bug 依然会被抓到。
            QTRY_VERIFY_WITH_TIMEOUT(mapColours() > 5, 5000);
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

        auto qmlNum = [&](const char* fn, double arg) -> double {
            QVariant ret;
            const bool ok = QMetaObject::invokeMethod(mapView, fn, Q_RETURN_ARG(QVariant, ret),
                                                      Q_ARG(QVariant, QVariant(arg)));
            return ok ? ret.toDouble() : qQNaN();
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

        window->grabWindow();   // 预热：首次会加载 .kvb 并建投影缓存

        qint64 worst = 0, total = 0;
        const int frames = 40;
        QElapsedTimer t;
        for (int i = 0; i < frames; ++i) {
            t.restart();
            mapView->setProperty("centerLon", 116.4 - i * 0.05);
            window->grabWindow();
            const qint64 ms = t.elapsed();
            worst = qMax(worst, ms);
            total += ms;
        }
        qInfo() << "PROBE drag:" << frames << "frames, avg" << (total / frames) << "ms, worst"
                << worst << "ms";
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

        auto qmlNum = [&](const char* fn, double arg) -> double {
            QVariant ret;
            const bool ok = QMetaObject::invokeMethod(mapView, fn, Q_RETURN_ARG(QVariant, ret),
                                                      Q_ARG(QVariant, QVariant(arg)));
            return ok ? ret.toDouble() : qQNaN();
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
        const auto wr = controller.property("waveRadii").toMap();
        QVERIFY2(wr.value("pOpacity").toDouble() <= 0.25,
                 qPrintable(QStringLiteral("前置条件失败：波前未淡出 opacity=%1")
                                .arg(wr.value("pOpacity").toDouble())));

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

        const QStringList categories = {"warning", "audio", "map", "appearance", "location", "source", "clock", "about"};
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
        page->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("showSettings").toBool());
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

        // 逐源震级过滤：Wolfx 设为「不接收」后，它的目录事件不入库、不通知。
        WolfxSource probe;
        controller.settings()->setInfoMagnitudeFilter(QStringLiteral("wolfx"), -1.0);
        EarthquakeEvent filtered = makeEvent("FILTERED-1", 3.0);
        filtered.latitude = 33.0;
        filtered.longitude = 110.0;
        filtered.timestamp = now + 2000;
        filtered.reportTime = now;
        const int before = controller.eventList().size();
        controller.handleEvent(filtered, false, true, &probe);
        QCOMPARE(controller.eventList().size(), before);
        QCOMPARE(tiers.count(), 3);

        // 回到不过滤后同一条事件重新入库并通知一次。
        controller.settings()->setInfoMagnitudeFilter(QStringLiteral("wolfx"), 0.0);
        controller.handleEvent(filtered, false, true, &probe);
        QCOMPARE(tiers.count(), 4);
        QCOMPARE(tiers.at(3).at(1).toString(), QStringLiteral("info"));
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

        // 关闭到托盘
        page->setProperty("currentSection", 7);
        QTest::qWait(60);
        QVERIFY(click("closeToTraySwitch"));
        QTRY_VERIFY(!controller.settings()->closeToTray());

        // 恢复默认后全部回到初始值。
        controller.settings()->resetToDefaults();
        QVERIFY(controller.settings()->notifyAlert());
        QVERIFY(!controller.settings()->raiseAlert());
        QVERIFY(controller.settings()->announceCountdown());
        QVERIFY(controller.settings()->closeToTray());
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
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
