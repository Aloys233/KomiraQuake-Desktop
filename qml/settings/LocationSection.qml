import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「定位与基准地」分区。
Column {
    id: root
    objectName: "settingsPanel-location"
    property Theme theme: Theme {}
    /// 由 SettingsPage 透传：在地图上点选位置。
    /// 由 SettingsPage 转发给外壳的 Toast（分区自身不在窗口层，无法直接弹 Toast）。
    signal notify(string message, bool success)
    signal pickLocationRequested()
    width: parent.width
    spacing: 20

    // 外部定位变化（IP 回退、地图点选、重新定位）时刷新输入框。
    // locationChanged 覆盖坐标与名称的全部变化。
    Connections {
        target: app
        function onLocationChanged() {
            manualLat.syncFromLocation();
            manualLon.syncFromLocation();
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "基准位置"; iconName: "map-pin"
        Column {
            width: parent.width; spacing: 6
            Text {
                width: parent.width
                text: app.locationName
                color: root.theme.textPrimary; font.pixelSize: 16; wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: app.locationStatusText + " · " + app.userLatitude.toFixed(4) + "°, " + app.userLongitude.toFixed(4) + "°"
                color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                text: "本地烈度、距离与预计到时以此位置估算。未定位时仍可接收地震事件。"
                color: root.theme.outline; font.pixelSize: 12; wrapMode: Text.Wrap; lineHeight: 1.4
            }
        }
        Flow {
            width: parent.width; spacing: 8
            GlassButton {
                theme: root.theme; text: "重新定位"; iconName: "locate-fixed"
                onClicked: { app.requestLocation(); root.notify("已发起定位请求", true); }
            }
            GlassButton {
                objectName: "pickLocationButton"; theme: root.theme
                text: "在地图上点选位置"; iconName: "map-pin"; primary: true
                onClicked: root.pickLocationRequested()
            }
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
                        onClicked: {
                            app.setManualLocation(modelData.lat, modelData.lon, modelData.name);
                            // 立刻回填输入框：否则它要等下一次 locationChanged 才更新。
                            manualLat.text = modelData.lat.toFixed(4);
                            manualLon.text = modelData.lon.toFixed(4);
                            root.notify("基准位置已切换至 " + modelData.name, true);
                        }
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
                GlassTextField {
                    id: manualLat; objectName: "manualLatitude"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    theme: root.theme; placeholderText: "−90 ~ 90"; Accessible.name: "纬度"
                    // 不用 `text: app.userLatitude...` 绑定：那会在每次外部定位变化时
                    // 覆盖用户正在输入的内容。改为命令式同步，且编辑期间不打扰。
                    Component.onCompleted: text = app.userLatitude.toFixed(4)
                    function syncFromLocation() { if (!activeFocus) text = app.userLatitude.toFixed(4); }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Text { text: "经度"; font.pixelSize: 12; color: root.theme.outline }
                GlassTextField {
                    id: manualLon; objectName: "manualLongitude"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    theme: root.theme; placeholderText: "−180 ~ 180"; Accessible.name: "经度"
                    Component.onCompleted: text = app.userLongitude.toFixed(4)
                    function syncFromLocation() { if (!activeFocus) text = app.userLongitude.toFixed(4); }
                }
            }
            GlassButton {
                Layout.alignment: Qt.AlignBottom
                theme: root.theme; text: "保存经纬度"; iconName: "check"
                onClicked: {
                    const la = Number(manualLat.text), lo = Number(manualLon.text);
                    if (manualLat.text.trim() && manualLon.text.trim() && isFinite(la) && isFinite(lo) && la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
                        app.setManualLocation(la, lo, "自定义坐标");
                        root.notify("基准坐标已应用", true);
                    } else root.notify("请输入有效的经纬度", false);
                }
            }
        }
    }
}
