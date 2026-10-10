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
    // 波前走完后收起左上角 HUD；新的波前或切换到其它事件时恢复。
    property string wavesDoneId: ""
    readonly property bool hudSuppressed: !!hudEvent && hudEvent.id === wavesDoneId
    // 地图标记仍画最近一次事件，但不作为默认取景中心；取景只跟随显式焦点。
    readonly property var mapEvent: app.mapEvent
    readonly property bool hudActive: sameEvent(hudEvent, app.activeWarning)
    MapView {
        id: map
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: window.desktop ? window.width - listSidebar.width : window.width
        theme: appTheme
        hasUser: app.hasLocation
        userLat: app.userLatitude; userLon: app.userLongitude
        event: window.mapEvent
        hasFocus: app.hasMapFocus
        warningActive: !!app.activeWarning
        hudInset: hud.visible ? hud.width : 0
        zoom: 6
        listExpanded: window.showList && !window.showSettings
        onWavesFinished: if (window.hudEvent) window.wavesDoneId = window.hudEvent.id
        onWavesStarted: window.wavesDoneId = ""
        onToggleListRequested: { window.showSettings = false; window.showList = !window.showList }
        onOpenSettingsRequested: window.showSettings = true
        onLocationPicked: (lat, lon) => { app.setManualLocation(lat, lon, "地图选点"); globalToast.show("基准位置已更新"); }
    }
    function applyDefaultView() {
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
            Qt.callLater(map.frameEvent);
        }
        function onResetMapRequested() { map.resetView(); }
    }
    HudCard {
        id: hud
        width: Math.min(352, map.width - 88)
        visible: !!window.hudEvent && !window.hudSuppressed && !window.showSettings && !map.pickingLocation
        anchors.left: parent.left; anchors.top: parent.top
        anchors.leftMargin: 16; anchors.topMargin: 16
        theme: appTheme
        backdropSource: map.renderLayer
        event: window.hudEvent; active: window.hudActive
        selected: !!event && app.isMapFocused(event.id)
        pageIndex: app.hudIndex
        pageCount: app.hudCount
        onPrevPage: app.hudPrev()
        onNextPage: app.hudNext()
    }
    // 左下角数据源状态：名称按连接状态着色（对齐 kanameishi）。
    GlassCard {
        id: sourceBadge
        objectName: "sourceBadge"
        anchors.left: parent.left; anchors.bottom: clockBadge.top
        anchors.leftMargin: 16; anchors.bottomMargin: 8
        theme: appTheme; backdropSource: map.renderLayer
        width: sourceRow.implicitWidth + 24; height: 34; radius: 10
        visible: !window.showSettings
        readonly property color statusColor: appTheme.connectionColor(app.sourceInfo.statusTag)
        RowLayout {
            id: sourceRow
            anchors.centerIn: parent; spacing: 8
            AppIcon {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 13; Layout.preferredHeight: 13
                name: "radio"; size: 13; color: sourceBadge.statusColor
            }
            Text { Layout.alignment: Qt.AlignVCenter; text: "数据源"; color: appTheme.outline; font.pixelSize: 11 }
            Text { Layout.alignment: Qt.AlignVCenter; text: app.sourceInfo.name; color: sourceBadge.statusColor; font.pixelSize: 11; font.weight: Font.Medium }
            Text { Layout.alignment: Qt.AlignVCenter; text: app.sourceInfo.status; color: appTheme.outline; font.pixelSize: 10 }
        }
    }
    GlassCard {
        id: clockBadge
        objectName: "clockBadge"
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 16
        theme: appTheme; backdropSource: map.renderLayer
        width: badgeRow.implicitWidth + 24; height: 34; radius: 10
        readonly property bool synced: app.clockInfo.synced === true
        property string timeText: ""
        Timer { interval: 1000; running: true; repeat: true; triggeredOnStart: true; onTriggered: clockBadge.timeText = app.clockTextUtc8() }
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
        onPickLocationRequested: { window.showSettings = false; map.pickingLocation = true; globalToast.show("点击地图设置基准位置"); }
    }
    // 桌面端不使用全屏预警：主 HUD 下方再放一张预警卡承载本地到时与操作。
    WarningHud {
        id: warningHud
        anchors.left: hud.left
        anchors.top: hud.bottom
        anchors.topMargin: 8
        width: hud.width
        visible: !!app.activeWarning && hud.visible && app.alertEligible
        theme: appTheme
        backdropSource: map.renderLayer
        event: app.activeWarning
        countdown: app.countdown
        onMuted: app.muteWarning()
        onStopped: app.stopWarning()
    }
    Toast { id: globalToast; theme: appTheme }
}
