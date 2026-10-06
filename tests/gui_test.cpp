#include <QtTest>
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
#define private public
#include "app/app_controller.h"
#include "service/alert_announcer.h"
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
private slots:
    void initTestCase() {
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
        for (bool dark : {false, true}) {
            controller.settings()->setThemeMode(dark ? "dark" : "light");
            const QPoint center = alertButton->mapToScene(QPointF(alertButton->width()/2, alertButton->height()/2)).toPoint();
            QTest::mouseMove(window, center);
            QTest::qWait(30);
            QVERIFY(alertButton->property("hovered").toBool());
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
            QVERIFY(clickText("定位与基准地"));
            QVERIFY(screenshot(dark ? "08-location-dark" : "08-location-light"));
            QVERIFY(clickText("界面与地图"));
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
        for (int section = 0; section < 6; ++section) {
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

        const QStringList categories = {"appearance", "location", "warning", "audio", "source", "clock", "about"};
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
        auto* combo = window->findChild<QQuickItem*>("settingsCategoryCombo");
        QVERIFY(combo && combo->isVisible());
        combo->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(page->property("currentSection").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_COMPARE(page->property("currentSection").toInt(), 0);
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
        QCOMPARE(resources.size(), 41);
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
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
