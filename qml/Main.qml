import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    // 静默启动：设置开启时本次启动直接进入托盘，不显示主窗口（等同「关闭窗口」）。
    visible: !app.startHidden
    width: 1280; height: 800
    minimumWidth: 360; minimumHeight: 520
    title: "KomiraQuake - 地震预警"
    Theme { id: appTheme }
    color: appTheme.surface
    onClosing: function(close) { close.accepted = false; window.hide(); }
    onVisibleChanged: app.setWindowVisible(visible)
    readonly property bool desktop: width >= 1024
    onDesktopChanged: if (!desktop) showList = false
    property bool showSettings: false
    property bool showList: false
    property bool initialLocationApplied: false
    function sameEvent(a, b) { return !!a && !!b && a.id === b.id && a.sourceTag === b.sourceTag; }
    // HUD 默认隐藏：仅活动预警或用户点击选中的事件才显示（hudEvent 由控制器裁决）。
    readonly property var hudEvent: app.hudEvent
    // 本地达标倒计时的受保护预警正在活动中：绝不能被收起或遮挡
    readonly property bool hasProtectedWarning: app.alertEligible && !!app.activeWarning && (app.countdown >= 0 || app.hasWarning)
    // 波前走完后收起左上角 HUD；新的波前或切换到其它事件时恢复。但若有本地高危预警倒计时，不可收起。
    property string wavesDoneId: ""
    readonly property bool hudSuppressed: !hasProtectedWarning && !!hudEvent && hudEvent.id === wavesDoneId
    // 地图标记仍画最近一次事件，但不作为默认取景中心；取景只跟随显式焦点。
    readonly property var mapEvent: app.mapEvent
    readonly property bool hudActive: sameEvent(hudEvent, app.activeWarning)
    Loader {
        id: mapLoader
        objectName: "mapLoader"
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: window.desktop ? window.width - listSidebar.width : window.width
        active: window.visible
        visible: window.visible
        asynchronous: false
        sourceComponent: MapView {
            id: mapItem
            anchors.fill: parent
            theme: appTheme
            hasUser: app.hasLocation
            userLat: app.userLatitude; userLon: app.userLongitude
            event: window.mapEvent
            stations: app.stationIntensity
            hasFocus: app.hasMapFocus
            warningActive: !!app.activeWarning
            hudWidth: hud.visible ? hud.width : 0
            // 取景用的顶部/底部遮挡：HUD 实际高度（含余量）与左下徽章合计高度。
            hudHeight: hud.visible ? hud.height + 32 : 0
            zoom: 6
            listExpanded: window.showList && !window.showSettings
            onWavesFinished: if (window.hudEvent) window.wavesDoneId = window.hudEvent.id
            onWavesStarted: window.wavesDoneId = ""
            onToggleListRequested: { window.showSettings = false; window.showList = !window.showList }
            onOpenSettingsRequested: window.showSettings = true
            onLocationPicked: (lat, lon) => { app.setManualLocation(lat, lon, "地图选点"); globalToast.show("基准位置已更新"); }
        }
        onLoaded: window.applyDefaultView()
    }
    readonly property var map: mapLoader.item
    function applyDefaultView() {
        if (!map) return;
        // 用户若已手动控制过镜头，定位到达时不再抢镜头。
        if (!initialLocationApplied && !hudEvent && app.hasLocation && !map.userMovedCamera) {
            const pt = map.shiftCoord(app.userLatitude, app.userLongitude);
            // 固定缩放到用户周边；不再沿用 map.zoom —— 无定位时地图会先按全国范围取景，
            // 那个缩放级别不适合作为"聚焦我的位置"的默认值。
            map.moveCamera(pt.lat, pt.lon, 6, 0);
            initialLocationApplied = true;
        }
    }
    // 默认位置交给窗口管理器时，Windows / 部分 X11 环境会按左上角或层叠摆放。
    // 首次显示前显式把窗口对齐到所在屏幕中心（Wayland 下由合成器决定，此设置会被忽略）。
    // 注意：这里只改位置、不改尺寸 —— 在 Component.onCompleted 里 resize 会让场景图
    // 在子树尚未稳定时重排（离屏/软件后端会崩），屏幕小于默认尺寸时窗口溢出即可。
    function centerOnScreen() {
        const screenW = Screen.width
        const screenH = Screen.height
        if (screenW <= 0 || screenH <= 0) return
        // 屏幕比窗口小时不要居中成负坐标（会顶掉标题栏），贴住屏幕左上即可。
        window.x = Math.round(Math.max(Screen.virtualX, Screen.virtualX + (screenW - window.width) / 2))
        window.y = Math.round(Math.max(Screen.virtualY, Screen.virtualY + (screenH - window.height) / 2))
    }
    Component.onCompleted: { centerOnScreen(); applyDefaultView(); app.setWindowVisible(window.visible) }
    Connections {
        target: app
        function onLocationChanged() { window.applyDefaultView(); }
        function onCenterMapRequested(latitude, longitude) {
            // 显式聚焦/选中（含再次选中同一事件）应恢复 HUD。
            window.wavesDoneId = "";
            // 显式聚焦：波前一淡出也要把镜头带过去（见 MapView.focusEvent）。
            if (map) Qt.callLater(map.focusEvent);
        }
        function onResetMapRequested() { if (map) map.resetView(); }
    }
    HudCard {
        id: hud
        width: Math.min(352, (map ? map.width : window.width) - 88)
        visible: !!window.hudEvent && !window.hudSuppressed && !window.showSettings && !(map && map.pickingLocation)
        anchors.left: parent.left; anchors.top: parent.top
        anchors.leftMargin: 16; anchors.topMargin: 16
        theme: appTheme
        backdropSource: map ? map.renderLayer : null
        event: window.hudEvent; active: window.hudActive
        selected: !!event && app.isMapFocused(event.id)
        pageIndex: app.hudIndex
        pageCount: app.hudCount
        onPrevPage: app.hudPrev()
        onNextPage: app.hudNext()
    }
    // 左下角数据源状态：逐源展示各自的连接状态，不再把多个源名合并成一条同色。
    // 之前用聚合状态给整串源名上色，会出现「模拟源没连上、却因别的源在线而被染绿」。
    GlassCard {
        id: sourceBadge
        objectName: "sourceBadge"
        anchors.left: parent.left; anchors.bottom: clockBadge.top
        anchors.leftMargin: 16; anchors.bottomMargin: 8
        theme: appTheme; backdropSource: map ? map.renderLayer : null
        width: sourceRow.implicitWidth + 24; height: 34; radius: 10
        visible: !window.showSettings
        // 只展示真正在跑的源：已启用且已配置（与启动门控一致）。
        readonly property var activeSources: app.sources.filter(function (s) {
            return s.enabled && s.configured
        })
        // 把「源名」展开成「分隔点 + 源名」的扁平项列表：分隔点作为独立布局项，
        // RowLayout 的 spacing 会均匀落在它两侧，因此正好居中（内联进文本里就会偏）。
        readonly property var sourceTokens: {
            const out = [];
            const srcs = activeSources;
            for (let i = 0; i < srcs.length; ++i) {
                if (i > 0) out.push({ sep: true });
                out.push({ sep: false, name: srcs[i].name, tag: srcs[i].statusTag });
            }
            return out;
        }
        RowLayout {
            id: sourceRow
            anchors.centerIn: parent; spacing: 8
            // 中英混排：按基线对齐（AlignVCenter 会让拉丁字母与中文不在一条线上，
            // 因为两者的 ascent/descent 不同、居中后视觉基线错位）。
            Text {
                Layout.alignment: Qt.AlignBaseline
                text: "数据源"; color: appTheme.outline; font.pixelSize: 11
            }
            Text {
                Layout.alignment: Qt.AlignBaseline
                visible: sourceBadge.activeSources.length === 0
                text: "无启用数据源"; color: appTheme.outline; font.pixelSize: 11
            }
            Repeater {
                model: sourceBadge.sourceTokens
                delegate: Text {
                    required property var modelData
                    Layout.alignment: Qt.AlignBaseline
                    // 分隔点用实心圆点 •（比 · 粗），独立成项。
                    text: modelData.sep ? "•" : modelData.name
                    color: modelData.sep ? appTheme.outline : appTheme.connectionColor(modelData.tag)
                    font.pixelSize: 11
                    font.weight: modelData.sep ? Font.Normal : Font.Medium
                    font.letterSpacing: modelData.sep ? 0 : 0.3
                }
            }
        }
    }
    GlassCard {
        id: clockBadge
        objectName: "clockBadge"
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 16
        theme: appTheme; backdropSource: map ? map.renderLayer : null
        width: badgeRow.implicitWidth + 24; height: 34; radius: 10
        readonly property bool synced: app.clockInfo.synced === true
        property string timeText: ""
        Timer { interval: 1000; running: window.visible; repeat: true; triggeredOnStart: true; onTriggered: clockBadge.timeText = app.clockTextUtc8() }
        RowLayout {
            id: badgeRow
            anchors.centerIn: parent; spacing: 8
            AppIcon {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 13; Layout.preferredHeight: 13
                name: "clock"; size: 13
                color: clockBadge.synced ? appTheme.clockSynced : appTheme.clockUnsynced
            }
            Text {
                Layout.alignment: Qt.AlignVCenter
                text: clockBadge.timeText
                color: clockBadge.synced ? appTheme.clockSynced : appTheme.clockUnsynced
                font.family: appTheme.numberFamily; font.pixelSize: 11
            }
            Text {
                Layout.alignment: Qt.AlignVCenter
                text: "UTC+8"; color: appTheme.outline; font.pixelSize: 10
            }
        }
    }
    EventListSidebar {
        id: listSidebar
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        theme: appTheme
        compact: !window.desktop
        expanded: window.showList && !window.showSettings
        hasLocation: app.hasLocation
        onToggleRequested: window.showList = !window.showList
    }
    SettingsPage {
        anchors.fill: parent; visible: window.showSettings; theme: appTheme
        onBack: window.showSettings = false
        onPickLocationRequested: { window.showSettings = false; if (map) map.pickingLocation = true; globalToast.show("点击地图设置基准位置"); }
    }
    // 桌面端不使用全屏预警：主 HUD 下方再放一张预警卡承载本地到时与操作。
    WarningHud {
        id: warningHud
        anchors.left: hud.visible ? hud.left : parent.left
        anchors.leftMargin: hud.visible ? 0 : 16
        anchors.top: hud.visible ? hud.bottom : parent.top
        anchors.topMargin: hud.visible ? 8 : 16
        width: hud.visible ? hud.width : Math.min(352, (map ? map.width : window.width) - 88)
        visible: !!app.activeWarning && app.alertEligible && (hud.visible || window.hasProtectedWarning)
        theme: appTheme
        backdropSource: map ? map.renderLayer : null
        event: app.activeWarning
        countdown: app.countdown
        onMuted: app.muteWarning()
        onStopped: app.stopWarning()
    }
    Toast { id: globalToast; theme: appTheme }
}
