import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    visible: true
    width: 1280; height: 800
    minimumWidth: 360; minimumHeight: 520
    title: "KomiraQuake - 地震预警"
    Theme { id: appTheme }
    color: appTheme.surface
    onClosing: function(close) { close.accepted = false; window.hide(); }
    readonly property bool desktop: width >= 1024
    onDesktopChanged: if (!desktop) showList = false
    property bool showSettings: false
    property bool showList: false
    property bool initialLocationApplied: false
    function sameEvent(a, b) { return !!a && !!b && a.id === b.id && a.sourceTag === b.sourceTag; }
    // HUD 默认隐藏：仅活动预警或用户点击选中的事件才显示（hudEvent 由控制器裁决）。
    readonly property var hudEvent: app.hudEvent
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
        hudInset: hud.visible ? hud.width : 0
        zoom: 6
        listExpanded: window.showList && !window.showSettings
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
    Component.onCompleted: applyDefaultView()
    Connections {
        target: app
        function onLocationChanged() { window.applyDefaultView(); }
        function onCenterMapRequested(latitude, longitude) { Qt.callLater(map.frameEvent); }
        function onResetMapRequested() { map.resetView(); }
    }
    HudCard {
        id: hud
        width: Math.min(352, map.width - 88)
        visible: !!window.hudEvent && !window.showSettings && !map.pickingLocation
        anchors.left: parent.left; anchors.top: parent.top
        anchors.leftMargin: 16; anchors.topMargin: 16
        theme: appTheme
        backdropSource: map.renderLayer
        event: window.hudEvent; active: window.hudActive
        selected: !!event && app.isMapFocused(event.id)
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
        Row {
            id: sourceRow
            anchors.centerIn: parent; spacing: 8
            AppIcon { anchors.verticalCenter: parent.verticalCenter; name: "radio"; size: 13; color: sourceBadge.statusColor }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "数据源"; color: appTheme.outline; font.pixelSize: 11 }
            Text { anchors.verticalCenter: parent.verticalCenter; text: app.sourceInfo.name; color: sourceBadge.statusColor; font.pixelSize: 11; font.weight: Font.Medium }
            Text { anchors.verticalCenter: parent.verticalCenter; text: app.sourceInfo.status; color: appTheme.outline; font.pixelSize: 10 }
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
        Row {
            id: badgeRow
            anchors.centerIn: parent; spacing: 8
            AppIcon { anchors.verticalCenter: parent.verticalCenter; name: "clock"; size: 13; color: clockBadge.synced ? appTheme.clockSynced : appTheme.clockUnsynced }
            Text { anchors.verticalCenter: parent.verticalCenter; text: clockBadge.timeText; color: clockBadge.synced ? appTheme.clockSynced : appTheme.clockUnsynced; font.family: appTheme.numberFamily; font.pixelSize: 11 }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "UTC+8"; color: appTheme.outline; font.pixelSize: 10 }
        }
    }
    EventListSidebar {
        id: listSidebar
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        theme: appTheme
        compact: !window.desktop
        expanded: window.showList && !window.showSettings
        events: app.eventList
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
        visible: !!app.activeWarning && hud.visible && app.settings.enableFullScreenWarning
        theme: appTheme
        event: app.activeWarning
        countdown: app.countdown
        onMuted: app.muteWarning()
        onStopped: app.stopWarning()
    }
    Toast { id: globalToast; theme: appTheme }
}
