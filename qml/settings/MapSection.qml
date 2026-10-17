import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「地图与图层」分区：底图与叠加层。把原先混在「界面与地图」里的地图项独立出来，
// 图层开关按实际渲染内容逐项列出，避免出现「开了却不知道开了什么」。
Column {
    id: root
    objectName: "settingsPanel-map"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "底图"; iconName: "map"
        Column {
            width: parent.width; spacing: 10
            GlassComboBox {
                objectName: "basemapCombo"
                width: parent.width; theme: root.theme; Accessible.name: "底图"
                model: ["高德标准 · GCJ-02", "Petal · GCJ-02", "OpenStreetMap · WGS-84", "矢量 · WGS-84", "自定义底图"]
                readonly property var ids: ["amap_vector", "petal", "osm", "vector", "custom"]
                // 未知 id（旧版残留，如已下线的卫星图）回落到第一项而不是空白。
                currentIndex: Math.max(0, ids.indexOf(app.settings.basemapId))
                onActivated: index => app.settings.basemapId = ids[index]
            }
            Text {
                width: parent.width
                text: "栅格底图可叠加矢量层；选择「矢量」时矢量本身就是底图。"
                wrapMode: Text.Wrap; lineHeight: 1.4
                color: root.theme.outline; font.pixelSize: 12
            }
        }
        Column {
            width: parent.width; spacing: 10
            visible: app.settings.basemapId === "custom"
            GlassTextField {
                width: parent.width; theme: root.theme
                placeholderText: "瓦片 URL 模板：{z}/{x}/{y}"
                Accessible.name: "自定义瓦片 URL"
                text: app.settings.customBasemapUrl
                onEditingFinished: app.settings.customBasemapUrl = text
            }
            GlassComboBox {
                width: parent.width; theme: root.theme; Accessible.name: "自定义底图坐标系"
                model: ["WGS-84", "GCJ-02"]
                currentIndex: app.settings.customBasemapDatum
                onActivated: index => app.settings.customBasemapDatum = index
            }
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "叠加图层"; iconName: "layers"
        // 栅格底图上叠加矢量层：选纯矢量底图时本就画矢量，无需再叠一层。
        GlassSwitch {
            objectName: "vectorOverlaySwitch"
            width: parent.width; theme: root.theme
            visible: app.settings.basemapId !== "vector"
            text: "叠加矢量层"
            description: "在栅格底图之上叠加省界与区县，预警时按区县烈度着色。"
            checked: app.settings.vectorOverlay
            onToggled: app.settings.vectorOverlay = checked
        }
        GlassSwitch {
            objectName: "provinceBorderSwitch"
            width: parent.width; theme: root.theme
            text: "省界描边"
            description: "矢量层中的省级分界线；关闭后只保留区县填色。低缩放级别本就不画省界。"
            checked: app.settings.showProvinceBorders
            enabled: app.settings.basemapId === "vector" || app.settings.vectorOverlay
            onToggled: app.settings.showProvinceBorders = checked
        }
        GlassSwitch {
            objectName: "stationLayerSwitch"
            width: parent.width; theme: root.theme
            text: "测站强度图层"
            description: "在地图上绘制各测站源的实时强度点，并显示右上角测站最大值卡片。数据连接不受影响。"
            checked: app.settings.showStationLayer
            onToggled: app.settings.showStationLayer = checked
        }
        GlassSwitch {
            objectName: "wavefrontSwitch"
            width: parent.width; theme: root.theme
            text: "P/S 波前圆"
            description: "按走时表绘制 P 波与 S 波的扩散范围；关闭后只保留倒计时与预警卡。"
            checked: app.settings.showWavefront
            onToggled: app.settings.showWavefront = checked
        }
    }
}
