import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "settingsPage"
    property Theme theme: Theme {}
    property int currentSection: 0
    readonly property bool compact: width < 760
    readonly property var sections: [
        { key: "appearance", title: "界面与地图", icon: "layers", description: "选择舒适的外观，调整地图的显示方式。" },
        { key: "location", title: "定位与基准地", icon: "map-pin", description: "设置用于估算本地烈度、距离与预计到时的位置。" },
        { key: "warning", title: "预警策略", icon: "shield", description: "决定何时提醒，以及如何显示本地预警。" },
        { key: "audio", title: "音效与语音", icon: "volume-2", description: "管理警报声音、音量与语音播报。" },
        { key: "source", title: "数据源与授时", icon: "radio", description: "查看实时预警连接与网络校时状态。" }
    ]
    signal back()
    signal pickLocationRequested()
    onCurrentSectionChanged: flick.contentY = 0
    onVisibleChanged: if (visible) root.forceActiveFocus()
    Keys.onEscapePressed: root.back()

    Rectangle { objectName: "settingsBackground"; anchors.fill: parent; color: root.theme.surface }
    MouseArea { anchors.fill: parent }

    RowLayout {
        id: toolbar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.leftMargin: root.compact ? 16 : 24; anchors.rightMargin: root.compact ? 16 : 24
        height: 72; spacing: 16
        GlassButton { theme: root.theme; text: "返回地图"; iconName: "arrow-left"; flat: true; onClicked: root.back() }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; color: root.theme.glassBorder }
        Text { text: "设置"; color: root.theme.textPrimary; font.pixelSize: 20; font.weight: Font.Medium }
        Item { Layout.fillWidth: true }
        Text { visible: !root.compact; text: "修改即时生效"; color: root.theme.outline; font.pixelSize: 12 }
        GlassButton {
            objectName: "resetSettingsButton"
            theme: root.theme; flat: true; iconName: "refresh-cw"
            text: root.compact ? "" : "恢复默认设置"; accessibleName: "恢复默认设置"
            onClicked: resetDialog.open()
        }
    }
    Rectangle { anchors.top: toolbar.bottom; width: parent.width; height: 1; color: root.theme.glassBorder }

    RowLayout {
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.top: toolbar.bottom; anchors.topMargin: 1
        spacing: 0
        Rectangle {
            id: sidebar
            objectName: "settingsSidebar"
            visible: !root.compact
            Layout.preferredWidth: 220; Layout.fillHeight: true
            color: root.theme.surfaceContainerLow
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: root.theme.glassBorder }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16
                spacing: 6
                Text { Layout.margins: 12; text: "偏好设置"; color: root.theme.outline; font.pixelSize: 11 }
                Repeater {
                    model: root.sections
                    AbstractButton {
                        id: category
                        required property var modelData
                        required property int index
                        objectName: "settingsNav-" + modelData.key
                        Layout.fillWidth: true; Layout.preferredHeight: 48
                        text: modelData.title
                        checked: root.currentSection === index
                        hoverEnabled: true; focusPolicy: Qt.StrongFocus
                        Accessible.name: text
                        onClicked: root.currentSection = index
                        background: Rectangle {
                            radius: 10
                            color: category.checked ? root.theme.accentContainer
                                 : category.down ? root.theme.glassCardPressed
                                 : category.hovered ? root.theme.glassCardHover : "transparent"
                            border.width: category.visualFocus ? 2 : 0
                            border.color: root.theme.accent
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            AppIcon { Layout.leftMargin: 14; name: category.modelData.icon; size: 18; color: category.checked ? root.theme.accent : root.theme.outline }
                            Text { Layout.fillWidth: true; text: category.text; color: category.checked ? root.theme.accent : root.theme.textPrimary; font.pixelSize: 13; font.weight: category.checked ? Font.Medium : Font.Normal }
                            Rectangle { Layout.rightMargin: 12; width: 4; height: 16; radius: 2; color: root.theme.accent; visible: category.checked }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Rectangle { Layout.fillWidth: true; Layout.margins: 12; height: 1; color: root.theme.glassBorder }
                Text { Layout.leftMargin: 12; text: "KomiraQuake"; color: root.theme.textPrimary; font.pixelSize: 12; font.weight: Font.Medium }
                Text { Layout.leftMargin: 12; Layout.bottomMargin: 8; text: "2.0 · 原生桌面版"; color: root.theme.outline; font.pixelSize: 11 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0
            GlassComboBox {
                objectName: "settingsCategoryCombo"
                visible: root.compact
                Layout.fillWidth: true; Layout.margins: 16; Layout.bottomMargin: 0
                theme: root.theme; Accessible.name: "设置分类"
                model: root.sections.map(section => section.title)
                currentIndex: root.currentSection
                onActivated: index => root.currentSection = index
            }
            Flickable {
                id: flick
                objectName: "settingsScroll"
                Layout.fillWidth: true; Layout.fillHeight: true
                contentWidth: width
                contentHeight: stack.implicitHeight + 56
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Column {
                    id: stack
                    width: Math.min(820, flick.width - (root.compact ? 32 : 64))
                    x: (flick.width - width) / 2; y: 28
                    spacing: 24
                    Column {
                        width: parent.width; spacing: 8
                        Text { text: root.sections[root.currentSection].title; color: root.theme.textPrimary; font.pixelSize: root.compact ? 24 : 28; font.weight: Font.Medium }
                        Text { width: parent.width; text: root.sections[root.currentSection].description; wrapMode: Text.Wrap; color: root.theme.outline; font.pixelSize: 13; lineHeight: 1.4 }
                    }

                    Column {
                        objectName: "settingsPanel-appearance"
                        width: parent.width; spacing: 20; visible: root.currentSection === 0
                        SettingsSection {
                            width: parent.width; theme: root.theme; title: "外观"; iconName: "sun"
                            RowLayout {
                                width: parent.width; spacing: 12
                                Repeater {
                                    model: [{ title: "浅色", icon: "sun", dark: false }, { title: "深色", icon: "moon", dark: true }]
                                    RadioButton {
                                        id: themeChoice
                                        required property var modelData
                                        objectName: modelData.dark ? "darkThemeButton" : "lightThemeButton"
                                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 76
                                        text: modelData.title
                                        checked: app.darkMode === modelData.dark
                                        hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: text + "主题"
                                        onClicked: app.darkMode = modelData.dark
                                        indicator: Item {}
                                        background: Rectangle {
                                            radius: 12
                                            color: themeChoice.checked ? root.theme.accentContainer : themeChoice.hovered ? root.theme.glassCardHover : root.theme.surface
                                            border.width: themeChoice.visualFocus || themeChoice.checked ? 2 : 1
                                            border.color: themeChoice.checked || themeChoice.visualFocus ? root.theme.accent : root.theme.glassBorder
                                        }
                                        contentItem: RowLayout {
                                            spacing: 8
                                            AppIcon { Layout.leftMargin: 12; name: themeChoice.modelData.icon; size: 22; color: themeChoice.checked ? root.theme.accent : root.theme.outline }
                                            Text { Layout.fillWidth: true; text: themeChoice.text; font.pixelSize: 14; font.weight: Font.Medium; color: root.theme.textPrimary }
                                            AppIcon { Layout.rightMargin: 10; name: "circle-check"; size: 16; opacity: themeChoice.checked ? 1 : 0; color: root.theme.accent }
                                        }
                                    }
                                }
                            }
                            Text { width: parent.width; text: "默认使用浅色。选择会自动保存，并应用到整个界面。"; wrapMode: Text.Wrap; color: root.theme.outline; font.pixelSize: 12 }
                            GlassSwitch {
                                objectName: "backgroundBlurSwitch"
                                width: parent.width; theme: root.theme; text: "背景模糊"
                                description: root.GraphicsInfo.api === GraphicsInfo.Software
                                    ? "当前为软件渲染，不支持背景模糊；即使开启也会使用实色表面。"
                                    : root.GraphicsInfo.api === GraphicsInfo.Unknown
                                    ? "渲染后端尚未就绪，暂用实色表面；就绪后按此开关启用地图浮层模糊。"
                                    : "模糊地图浮层后方，保持文字清晰；列表和设置页使用实色表面。"
                                checked: app.settings.backgroundBlur
                                onToggled: app.settings.backgroundBlur = checked
                            }
                            GlassSwitch { objectName: "reduceMotionSwitch"; width: parent.width; theme: root.theme; text: "减少动态效果"; description: "停用装饰过渡。真实波前、预计倒计时和数据更新不受影响。"; checked: app.settings.reduceMotion; onToggled: app.settings.reduceMotion = checked }
                        }
                        SettingsSection {
                            width: parent.width; theme: root.theme; title: "地图显示"; iconName: "map"
                            Column {
                                width: parent.width; spacing: 10
                                Text { text: "底图"; color: root.theme.textPrimary; font.pixelSize: 14 }
                                GlassComboBox {
                                    objectName: "basemapCombo"
                                    width: parent.width; theme: root.theme; Accessible.name: "底图"
                                    model: ["高德标准 · GCJ-02", "高德卫星 · GCJ-02", "OpenStreetMap · WGS-84", "自定义底图"]
                                    currentIndex: Math.max(0, ["amap_vector","amap_satellite","osm","custom"].indexOf(app.settings.basemapId))
                                    onActivated: index => app.settings.basemapId = ["amap_vector","amap_satellite","osm","custom"][index]
                                }
                            }
                            Column {
                                width: parent.width; spacing: 10; visible: app.settings.basemapId === "custom"
                                GlassTextField { width: parent.width; theme: root.theme; placeholderText: "瓦片 URL 模板：{z}/{x}/{y}"; Accessible.name: "自定义瓦片 URL"; text: app.settings.customBasemapUrl; onEditingFinished: app.settings.customBasemapUrl = text }
                                GlassComboBox { width: parent.width; theme: root.theme; Accessible.name: "自定义底图坐标系"; model: ["WGS-84", "GCJ-02"]; currentIndex: app.settings.customBasemapDatum; onActivated: index => app.settings.customBasemapDatum = index }
                            }
                        }
                    }

                    SettingsSection {
                        objectName: "settingsPanel-location"
                        width: parent.width; theme: root.theme; title: "基准位置"; iconName: "map-pin"; visible: root.currentSection === 1
                        Column {
                            width: parent.width; spacing: 6
                            Text { width: parent.width; text: app.locationName; color: root.theme.textPrimary; font.pixelSize: 16; wrapMode: Text.Wrap }
                            Text { width: parent.width; text: app.locationStatusText + " · " + app.userLatitude.toFixed(4) + "°, " + app.userLongitude.toFixed(4) + "°"; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap }
                            Text { width: parent.width; text: "本地烈度、距离与预计到时以此位置估算。未定位时仍可接收地震事件。"; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4 }
                        }
                        Flow {
                            width: parent.width; spacing: 8
                            GlassButton { theme: root.theme; text: "自动获取 IP 位置"; iconName: "globe"; onClicked: { app.requestLocation(); toast.show("已发起 IP 定位请求"); } }
                            GlassButton { objectName: "pickLocationButton"; theme: root.theme; text: "在地图上点选位置"; iconName: "map-pin"; primary: true; onClicked: root.pickLocationRequested() }
                        }
                        Column {
                            width: parent.width; spacing: 10
                            Text { text: "常用城市"; color: root.theme.outline; font.pixelSize: 12 }
                            Flow {
                                width: parent.width; spacing: 8
                                Repeater {
                                    model: [{name:"北京",lat:39.9042,lon:116.4074}, {name:"上海",lat:31.2304,lon:121.4737}, {name:"成都",lat:30.5728,lon:104.0668}, {name:"昆明",lat:25.0453,lon:102.7097}, {name:"西安",lat:34.3416,lon:108.9398}, {name:"广州",lat:23.1291,lon:113.2644}, {name:"武汉",lat:30.5928,lon:114.3055}, {name:"台北",lat:25.0330,lon:121.5654}]
                                    GlassButton {
                                        required property var modelData
                                        theme: root.theme; text: modelData.name; implicitWidth: 62; implicitHeight: 34
                                        primary: Math.abs(app.userLatitude - modelData.lat) < 0.1 && Math.abs(app.userLongitude - modelData.lon) < 0.1
                                        onClicked: { app.setManualLocation(modelData.lat, modelData.lon, modelData.name); manualLat.text = modelData.lat.toFixed(4); manualLon.text = modelData.lon.toFixed(4); toast.show("基准位置已切换至 " + modelData.name); }
                                    }
                                }
                            }
                        }
                        GridLayout {
                            width: parent.width
                            columns: width > 510 ? 3 : 2
                            columnSpacing: 10; rowSpacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: "纬度"; font.pixelSize: 12; color: root.theme.outline }
                                GlassTextField { id: manualLat; objectName: "manualLatitude"; Layout.fillWidth: true; Layout.minimumWidth: 0; theme: root.theme; text: app.userLatitude.toFixed(4); placeholderText: "−90 ~ 90"; Accessible.name: "纬度" }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: "经度"; font.pixelSize: 12; color: root.theme.outline }
                                GlassTextField { id: manualLon; objectName: "manualLongitude"; Layout.fillWidth: true; Layout.minimumWidth: 0; theme: root.theme; text: app.userLongitude.toFixed(4); placeholderText: "−180 ~ 180"; Accessible.name: "经度" }
                            }
                            GlassButton {
                                Layout.alignment: Qt.AlignBottom; theme: root.theme; text: "保存经纬度"; iconName: "check"
                                onClicked: {
                                    const la = Number(manualLat.text), lo = Number(manualLon.text);
                                    if (manualLat.text.trim() && manualLon.text.trim() && isFinite(la) && isFinite(lo) && la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
                                        app.setManualLocation(la, lo, "自定义坐标"); toast.show("基准坐标已应用");
                                    } else toast.show("请输入有效的经纬度", false);
                                }
                            }
                        }
                    }
                    SettingsSection {
                        objectName: "settingsPanel-warning"
                        width: parent.width; theme: root.theme; title: "提醒条件"; iconName: "shield"; visible: root.currentSection === 2
                        GlassSwitch { width: parent.width; theme: root.theme; text: "预警卡片"; description: "满足提醒条件时，在主 HUD 下方显示本地预计到时与避险操作。"; checked: app.settings.enableFullScreenWarning; onToggled: app.settings.enableFullScreenWarning = checked }
                        Column {
                            width: parent.width; spacing: 10
                            Text { text: "烈度标准"; color: root.theme.textPrimary; font.pixelSize: 14 }
                            GlassComboBox { width: parent.width; theme: root.theme; model: ["中国烈度 · CSIS", "日本震度 · JMA"]; Accessible.name: "烈度标准"; currentIndex: app.settings.intensityStandard; onActivated: index => app.settings.intensityStandard = index }
                        }
                        Column {
                            width: parent.width; spacing: 6
                            RowLayout { width: parent.width; Text { Layout.fillWidth: true; text: "本地烈度过滤"; color: root.theme.textPrimary; font.pixelSize: 14 } Text { text: app.settings.localIntensityFilter <= 0 ? "关闭" : app.settings.localIntensityFilter.toFixed(1) + " 度"; color: root.theme.textPrimary; font.family: root.theme.numberFamily; font.pixelSize: 14 } }
                            GlassSlider { width: parent.width; theme: root.theme; from: 0; to: 8; stepSize: 0.5; value: app.settings.localIntensityFilter; Accessible.name: "本地烈度过滤"; onMoved: app.settings.localIntensityFilter = value }
                            Text { width: parent.width; text: "仅当本地预估烈度达到该值时提醒；设为 0 表示不作筛选。"; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4 }
                        }
                    }
                    SettingsSection {
                        objectName: "settingsPanel-audio"
                        width: parent.width; theme: root.theme; title: "声音与播报"; iconName: "volume-2"; visible: root.currentSection === 3
                        GlassSwitch { width: parent.width; theme: root.theme; text: "警报音效"; description: "播放预警音效与倒计时提示音。"; checked: app.settings.enableSoundAlert; onToggled: app.settings.enableSoundAlert = checked }
                        GlassSwitch { width: parent.width; theme: root.theme; text: "全局静音"; description: "停止声音与语音，保留视觉提醒。"; checked: app.settings.isMuted; onToggled: app.settings.isMuted = checked }
                        Column {
                            width: parent.width; spacing: 6
                            RowLayout { width: parent.width; Text { Layout.fillWidth: true; text: "警报音量"; color: root.theme.textPrimary; font.pixelSize: 14 } Text { text: Math.round(app.settings.alertVolume * 100) + "%"; color: root.theme.textPrimary; font.family: root.theme.numberFamily; font.pixelSize: 14 } }
                            GlassSlider { width: parent.width; theme: root.theme; from: 0; to: 1; stepSize: 0.05; value: app.settings.alertVolume; Accessible.name: "警报音量"; onMoved: app.settings.alertVolume = value }
                        }
                        GlassSwitch { width: parent.width; theme: root.theme; text: "语音播报"; description: app.speechAvailable ? "朗读震中、震级与预估烈度。" : "系统未安装可用的语音引擎。"; enabled: app.speechAvailable; checked: app.settings.enableSpeech; onToggled: app.settings.enableSpeech = checked }
                        GlassSwitch { width: parent.width; theme: root.theme; text: "播报预计倒计时"; description: "预计 30、20、10 秒时播报。"; enabled: app.speechAvailable && app.settings.enableSpeech; checked: app.settings.speakCountdown; onToggled: app.settings.speakCountdown = checked }
                        GlassSwitch { width: parent.width; theme: root.theme; text: "播报后续更新"; description: "报次递增时播报最新信息。"; enabled: app.speechAvailable && app.settings.enableSpeech; checked: app.settings.speakUpdates; onToggled: app.settings.speakUpdates = checked }
                        Column {
                            width: parent.width; spacing: 6; visible: app.speechAvailable && app.settings.enableSpeech
                            RowLayout { width: parent.width; Text { Layout.fillWidth: true; text: "播报语速"; color: root.theme.textPrimary; font.pixelSize: 14 } Text { text: (app.settings.speechRate * 2).toFixed(1) + "×"; color: root.theme.textPrimary; font.pixelSize: 14 } }
                            GlassSlider { width: parent.width; theme: root.theme; from: 0.1; to: 1; stepSize: 0.05; value: app.settings.speechRate; Accessible.name: "播报语速"; onMoved: app.settings.speechRate = value }
                        }
                        GlassButton { theme: root.theme; text: "试听预警语音"; iconName: "play"; enabled: app.speechAvailable && app.settings.enableSpeech; onClicked: app.sampleSpeech() }
                    }
                    SettingsSection {
                        objectName: "settingsPanel-source"
                        width: parent.width; theme: root.theme; title: "连接与校时"; iconName: "radio"; visible: root.currentSection === 4
                        GlassSwitch { width: parent.width; theme: root.theme; text: "Wolfx 实时预警"; description: app.sourceInfo.name + " · " + app.sourceInfo.status + " · 延迟 " + app.sourceInfo.latency; checked: app.settings.enabledWolfx; onToggled: app.settings.enabledWolfx = checked }
                        Text { width: parent.width; text: app.sourceInfo.description; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4 }
                        GlassSwitch { width: parent.width; theme: root.theme; text: "网络时间校准"; description: "使用 SNTP，失败时回退 HTTP。校正本机时间偏差，改善到时估算。"; checked: app.settings.enableNtpSync; onToggled: app.settings.enableNtpSync = checked }
                        Text { width: parent.width; text: "校时状态 · " + app.clockInfo.state + "\n" + app.clockInfo.detail; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.5 }
                    }
                }
            }
        }
    }

    Dialog {
        id: resetDialog
        objectName: "resetSettingsDialog"
        anchors.centerIn: parent
        width: Math.min(420, root.width - 32)
        modal: true; focus: true; padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle { radius: 16; color: root.theme.surfaceContainerLow; border.width: 1; border.color: root.theme.glassBorder }
        Overlay.modal: Rectangle { color: "#66000000" }
        contentItem: ColumnLayout {
            spacing: 16
            Text { text: "恢复默认设置？"; color: root.theme.textPrimary; font.pixelSize: 20; font.weight: Font.Medium }
            Text { Layout.fillWidth: true; text: "外观将恢复为浅色，地图、预警和声音等偏好也将重置。此操作无法撤销。"; color: root.theme.outline; font.pixelSize: 13; wrapMode: Text.Wrap; lineHeight: 1.5 }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 8
                Item { Layout.fillWidth: true }
                GlassButton { objectName: "cancelResetButton"; theme: root.theme; text: "取消"; onClicked: resetDialog.reject() }
                GlassButton { objectName: "confirmResetButton"; theme: root.theme; text: "恢复默认"; danger: true; onClicked: resetDialog.accept() }
            }
        }
        onAccepted: { app.settings.resetToDefaults(); toast.show("设置已恢复默认值"); }
    }
    Toast { id: toast; theme: root.theme }
}
