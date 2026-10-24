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
    // 主题令牌从这里注入：Theme 本身不引用 context property `app`（见 Theme.qml 注释）。
    Theme {
        id: appTheme
        dark: app ? app.darkMode : false
        reduceMotion: app && app.settings ? app.settings.reduceMotion : false
        backgroundBlur: app && app.settings ? app.settings.backgroundBlur : true
    }
    color: appTheme.surface
    // 关闭到托盘（默认）：只隐藏窗口；关闭该设置后按系统惯例直接退出。
    onClosing: function(close) {
        if (app.settings.closeToTray) { close.accepted = false; window.hide(); }
        else { close.accepted = true; Qt.quit(); }
    }
    onVisibleChanged: app.setWindowVisible(visible)
    readonly property bool desktop: width >= 1024
    onDesktopChanged: if (!desktop) showList = false
    property bool showSettings: false
    /// 设置页第一次打开后常驻（仅隐藏），避免每次进出都重建、丢失页内状态。
    property bool settingsVisited: false
    onShowSettingsChanged: if (showSettings) settingsVisited = true
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
            hasFocus: app.hasMapFocus
            focusEstimate: app.focusEstimate
            warningActive: !!app.activeWarning
            hudWidth: hud.visible ? hud.width : 0
            // 取景用的顶部/底部遮挡：左列（HUD + 震源推算卡）实际占用的最低点，
            // 以及左下角徽章合计高度。必须跟着卡片实际位置走，否则新卡片会压住震中。
            hudHeight: window.leftColumnBottom > 0 ? window.leftColumnBottom + 16 : 0
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
        if (!initialLocationApplied && !hudEvent && app.hasLocation && !map.userMovedCamera
                && !map.estimateFocused) {
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
    // 启动瞬间必须可交互：窗口一出现就拿焦点。
    //
    // 冷启动时窗口是 QML 自己 visible 出来的（见文件开头的 visible 绑定），Qt 不会
    // 替我们 requestActivate()。在 click-to-focus 的窗口管理器下，点一个**未聚焦**
    // 窗口的第一次点击会被 WM 吃掉，用于抬起并聚焦该窗口，应用收不到那次 click ——
    // 用户感受就是「窗口刚出来的一瞬间点不动，再点一下才正常」。
    // 托盘点恢复与预警置顶那条路径（main.cpp 的 presentWindow）一直是
    // show + raise + requestActivate，冷启动这条一直漏着。
    //
    // 不要改成「等首帧/等布局完成再显示窗口」：实测从未 show 过的窗口 contentItem
    // 恒为 0x0（Qt 不给隐藏窗口派发布局），而 mapLoader 又绑在 window.visible 上，
    // 等就绪是死路。窗口必须照常立刻显示，只是要顺手把焦点拿过来。
    function claimFocus() {
        if (!visible) return
        window.raise()
        window.requestActivate()
    }
    Component.onCompleted: {
        centerOnScreen()
        applyDefaultView()
        app.setWindowVisible(window.visible)
        claimFocus()
    }
    Connections {
        target: app
        function onLocationChanged() { window.applyDefaultView(); }
        function onCenterMapRequested(latitude, longitude) {
            // 显式聚焦/选中（含再次选中同一事件）应恢复 HUD。
            window.wavesDoneId = "";
            // 显式聚焦：波前一淡出也要把镜头带过去（见 MapView.focusEvent）。
            // 必须同步启动：事件切换会同时改变 HUD 尺寸，布局重取景的 Qt.callLater
            // 排在入场补间之前，会把镜头先硬切到新震中，补间就只剩零距离。
            if (map) map.focusEvent();
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
        backdropLive: !!map && map.cameraMoving
        event: window.hudEvent; active: window.hudActive
        selected: !!event && app.isMapFocused(event.id)
        pageIndex: app.hudIndex
        pageCount: app.hudCount
        onPrevPage: app.hudPrev()
        onNextPage: app.hudNext()
    }
    // 左下角数据源状态：逐源展示各自的连接状态，不再把多个源名合并成一条同色。
    // 之前用聚合状态给整串源名上色，会出现「模拟源没连上、却因别的源在线而被染绿」。
    // 位置：**贴屏幕下沿**，时钟徽章叠在它上面（用户要求两者互换位置）。
    GlassCard {
        id: sourceBadge
        objectName: "sourceBadge"
        anchors.left: parent.left; anchors.bottom: parent.bottom
        anchors.leftMargin: 16; anchors.bottomMargin: 16
        theme: appTheme; backdropSource: map ? map.renderLayer : null
        backdropLive: !!map && map.cameraMoving
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
        // 时钟在上、数据源在下：两者互换位置后，靠屏幕下沿的是数据源徽章。
        anchors.left: parent.left; anchors.leftMargin: 16
        anchors.bottom: sourceBadge.top; anchors.bottomMargin: 8
        theme: appTheme; backdropSource: map ? map.renderLayer : null
        backdropLive: !!map && map.cameraMoving
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
    // 设置页包含全部分区，构造一次要几十毫秒；只在第一次打开时才建，之后常驻隐藏。
    Loader {
        anchors.fill: parent
        active: window.showSettings || window.settingsVisited
        visible: window.showSettings
        sourceComponent: SettingsPage {
            anchors.fill: parent
            theme: appTheme
            onBack: window.showSettings = false
            onPickLocationRequested: { window.showSettings = false; if (map) map.pickingLocation = true; globalToast.show("点击地图设置基准位置"); }
        }
    }
    // 左列的最下沿（窗口坐标）：取景要让开这一整列，否则地图会把震中藏到卡片底下。
    // 三个卡片的可见性各自裁决，这里只做几何汇总，不参与任何显示判断。
    readonly property real leftColumnBottom: {
        let bottom = 0
        if (hud.visible) bottom = Math.max(bottom, hud.y + hud.height)
        if (warningHud.visible) bottom = Math.max(bottom, warningHud.y + warningHud.height)
        if (sourceEstimate.visible) bottom = Math.max(bottom, sourceEstimate.y + sourceEstimate.height)
        return bottom
    }
    // 倒计时卡默认贴在 HUD 右侧；窄窗口放不下时退回 HUD 下方 —— 宁可换位，
    // 也不要一张被窗口右缘裁掉的倒计时。
    readonly property real mapAreaWidth: map ? map.width : window.width
    readonly property bool countdownBesideHud: hud.visible
        && (mapAreaWidth - hud.x - hud.width - 24) >= 220
    // 本地倒计时：主 HUD 右侧的紧凑卡。它比 HUD 窄、数字更小，不再另起一列纵向堆叠，
    // 从而把左列纵向空间让给下方的震源推算卡。
    WarningHud {
        id: warningHud
        anchors.left: window.countdownBesideHud ? hud.right : (hud.visible ? hud.left : parent.left)
        anchors.leftMargin: window.countdownBesideHud ? 8 : (hud.visible ? 0 : 16)
        // 贴右侧时与 HUD 顶对齐；退回左列时仍挂在 HUD 之下（不能压在同一处）。
        anchors.top: hud.visible
            ? (window.countdownBesideHud ? hud.top : hud.bottom)
            : parent.top
        anchors.topMargin: hud.visible && !window.countdownBesideHud ? 8 : (hud.visible ? 0 : 16)
        width: window.countdownBesideHud
            ? Math.min(264, window.mapAreaWidth - hud.x - hud.width - 24)
            : (hud.visible ? hud.width : Math.min(352, window.mapAreaWidth - 88))
        visible: !!app.activeWarning && app.alertEligible && (hud.visible || window.hasProtectedWarning)
        theme: appTheme
        backdropSource: map ? map.renderLayer : null
        backdropLive: !!map && map.cameraMoving
        event: app.activeWarning
        countdown: app.countdown
        onMuted: app.muteWarning()
        onStopped: app.stopWarning()
    }
    // 本地震源推算（Beta）：独占一张卡，位于左列 HUD 之下（与预警卡、测站卡都不共用）。
    // 收到解才开始占位；只显示、不产事件、不参与告警与去重。
    // 卡内自带 n/N 分页（一区一页），翻页状态留在卡内，不需要上层联动。
    SourceEstimateCard {
        id: sourceEstimate
        objectName: "sourceEstimateCard"
        // 设置页是全屏面板；推算卡不是保护性元素，留在上面只会压住设置正文。
        visible: !window.showSettings && hasSignal
        anchors.left: hud.visible ? hud.left : parent.left
        anchors.leftMargin: hud.visible ? 0 : 16
        // 起点取左列所有已显示卡片的下沿：倒计时卡贴右侧时不影响本卡，退回左列时则以它为准。
        anchors.top: hud.visible || warningHud.visible
            ? (warningHud.visible && !window.countdownBesideHud
                ? warningHud.bottom
                : (hud.visible ? hud.bottom : warningHud.bottom))
            : parent.top
        anchors.topMargin: hud.visible || warningHud.visible ? 8 : 16
        width: hud.visible ? hud.width : Math.min(352, window.mapAreaWidth - 88)
        theme: appTheme
        backdropSource: map ? map.renderLayer : null
        backdropLive: !!map && map.cameraMoving
        info: app.sourceEstimateInfo
    }
    Toast { id: globalToast; theme: appTheme }
}
