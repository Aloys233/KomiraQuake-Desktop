import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "settingsPage"
    property Theme theme: Theme {}
    signal back()
    signal pickLocationRequested()
    Rectangle { anchors.fill: parent; color: root.theme.surface }
    MouseArea { anchors.fill: parent }
    Flickable {
        id: flick
        objectName: "settingsScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: stack.implicitHeight + 64
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        Column {
            id: stack
            width: Math.min(760, flick.width - (flick.width < 600 ? 32 : 64))
            x: (flick.width - width) / 2
            y: 28
            spacing: 20
            RowLayout {
                width: parent.width
                GlassButton { theme: root.theme; text: "返回地图"; iconName: "arrow-left"; flat: true; onClicked: root.back() }
                Item { Layout.fillWidth: true }
                AppIcon { name: "settings"; color: root.theme.outline; size: 20 }
            }
            Column {
                width: parent.width
                spacing: 8
                Text { text: "设置"; color: root.theme.textPrimary; font.pixelSize: 30; font.weight: Font.Normal }
                Text { width: parent.width; text: "按你的所在地与使用习惯，调整预警与地图。"; wrapMode: Text.Wrap; color: root.theme.outline; font.pixelSize: 13 }
            }
            SettingsSection {
                width: parent.width; theme: root.theme; title: "定位与基准地"; iconName: "map-pin"
                Column {
                    width: parent.width; spacing: 6
                    Text { width: parent.width; text: app.locationName; color: root.theme.textPrimary; font.pixelSize: 16; wrapMode: Text.Wrap }
                    Text { width: parent.width; text: app.locationStatusText + " · " + app.userLatitude.toFixed(4) + "°, " + app.userLongitude.toFixed(4) + "°"; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap }
                    Text { width: parent.width; text: "本地烈度、距离与预计到时以此位置估算。未定位时仍可接收地震事件。"; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4 }
                }
                Flow {
                    width: parent.width; spacing: 8
                    GlassButton { theme: root.theme; text: "自动获取 IP 位置"; iconName: "globe"; onClicked: { app.requestLocation(); toast.show("已发起 IP 定位请求"); } }
                    GlassButton { theme: root.theme; text: "在地图上点选位置"; iconName: "map-pin"; primary: true; onClicked: root.pickLocationRequested() }
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
                width: parent.width; theme: root.theme; title: "界面与地图"; iconName: "layers"
                GlassSwitch { width: parent.width; theme: root.theme; text: "深色模式"; description: "石墨灰低眩光表面；关闭后使用柔和浅色。"; checked: app.darkMode; onToggled: app.setDarkMode(checked) }
                GlassSwitch { objectName: "backgroundBlurSwitch"; width: parent.width; theme: root.theme; text: "背景模糊"; description: "轻柔模糊地图浮层后方，保持文字清晰。软件渲染时自动使用实色表面。"; checked: app.settings.backgroundBlur; onToggled: app.settings.backgroundBlur = checked }
                GlassSwitch { objectName: "reduceMotionSwitch"; width: parent.width; theme: root.theme; text: "减少动态效果"; description: "停用装饰过渡。真实波前、预计倒计时和数据更新不受影响。"; checked: app.settings.reduceMotion; onToggled: app.settings.reduceMotion = checked }
                Column {
                    width: parent.width; spacing: 10
                    Text { text: "底图"; color: root.theme.textPrimary; font.pixelSize: 14 }
                    GlassComboBox {
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
            SettingsSection {
                width: parent.width; theme: root.theme; title: "预警策略"; iconName: "shield"
                GlassSwitch { width: parent.width; theme: root.theme; text: "预警卡片"; description: "满足提醒条件时，在主 HUD 下方显示本地预计到时与避险操作。"; checked: app.settings.enableFullScreenWarning; onToggled: app.settings.enableFullScreenWarning = checked }
                Column {
                    width: parent.width; spacing: 10
                    Text { text: "烈度标准"; color: root.theme.textPrimary; font.pixelSize: 14 }
                    GlassComboBox { width: parent.width; theme: root.theme; model: ["中国烈度 · CSIS", "日本震度 · JMA"]; Accessible.name: "烈度标准"; currentIndex: app.settings.intensityStandard; onActivated: index => app.settings.intensityStandard = index }
                }
                Column {
                    width: parent.width; spacing: 6
                    RowLayout { width: parent.width; Text { Layout.fillWidth: true; text: "最小预警震级"; color: root.theme.textPrimary; font.pixelSize: 14 } Text { text: "M " + app.settings.minWarningMagnitude.toFixed(1); color: root.theme.textPrimary; font.family: root.theme.numberFamily; font.pixelSize: 14 } }
                    GlassSlider { width: parent.width; theme: root.theme; from: 2; to: 7; stepSize: 0.5; value: app.settings.minWarningMagnitude; Accessible.name: "最小预警震级"; onMoved: app.settings.minWarningMagnitude = value }
                }
                Column {
                    width: parent.width; spacing: 6
                    RowLayout { width: parent.width; Text { Layout.fillWidth: true; text: "最小本地预估烈度"; color: root.theme.textPrimary; font.pixelSize: 14 } Text { text: app.settings.minWarningIntensity.toFixed(1) + " 度"; color: root.theme.textPrimary; font.family: root.theme.numberFamily; font.pixelSize: 14 } }
                    GlassSlider { width: parent.width; theme: root.theme; from: 1; to: 6; stepSize: 0.5; value: app.settings.minWarningIntensity; Accessible.name: "最小本地预估烈度"; onMoved: app.settings.minWarningIntensity = value }
                }
            }
            SettingsSection {
                width: parent.width; theme: root.theme; title: "音效与语音"; iconName: "volume-2"
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
                width: parent.width; theme: root.theme; title: "数据源与授时"; iconName: "radio"
                GlassSwitch { width: parent.width; theme: root.theme; text: "Wolfx 实时预警"; description: app.sourceInfo.name + " · " + app.sourceInfo.status + " · 延迟 " + app.sourceInfo.latency; checked: app.settings.enabledWolfx; onToggled: app.settings.enabledWolfx = checked }
                Text { width: parent.width; text: app.sourceInfo.description; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4 }
                GlassSwitch { width: parent.width; theme: root.theme; text: "网络时间校准"; description: "使用 SNTP，失败时回退 HTTP。校正本机时间偏差，改善到时估算。"; checked: app.settings.enableNtpSync; onToggled: app.settings.enableNtpSync = checked }
                Text { width: parent.width; text: "校时状态 · " + app.clockInfo.state + "\n" + app.clockInfo.detail; color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.5 }
            }
            Flow {
                width: parent.width; spacing: 16
                GlassButton { theme: root.theme; text: "恢复默认设置"; iconName: "refresh-cw"; onClicked: { app.settings.resetToDefaults(); toast.show("设置已恢复默认值"); } }
                Text { height: 40; verticalAlignment: Text.AlignVCenter; text: "KomiraQuake 2.0 · 原生桌面版"; font.pixelSize: 11; color: root.theme.outline }
            }
        }
    }
    Toast { id: toast; theme: root.theme }
}
