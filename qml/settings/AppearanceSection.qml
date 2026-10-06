import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「界面与地图」分区：外观（主题 / 背景模糊 / 减少动效）与地图底图。
// 由 SettingsPage 实例化并按 currentSection 切换 visible。
Column {
    id: root
    objectName: "settingsPanel-appearance"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "外观"; iconName: "sun"
        Flow {
            width: parent.width; spacing: 10
            Repeater {
                model: [
                    { title: "浅色", icon: "sun", mode: "light" },
                    { title: "深色", icon: "moon", mode: "dark" },
                    { title: "跟随系统", icon: "sliders-horizontal", mode: "system" }
                ]
                RadioButton {
                    id: themeChoice
                    required property var modelData
                    // objectName 保留给 GUI 测试：light/dark 沿用旧名，新增 system。
                    objectName: modelData.mode === "dark" ? "darkThemeButton"
                                : modelData.mode === "light" ? "lightThemeButton" : "systemThemeButton"
                    width: (root.width - 40 - 20) / 3
                    height: 76
                    text: modelData.title
                    checked: app.settings.themeMode === modelData.mode
                    hoverEnabled: true; focusPolicy: Qt.StrongFocus
                    Accessible.name: text + "主题"
                    onClicked: app.settings.themeMode = modelData.mode
                    indicator: Item {}
                    background: Rectangle {
                        radius: 12
                        color: themeChoice.checked ? root.theme.accentContainer
                             : themeChoice.hovered ? root.theme.glassCardHover : root.theme.surface
                        border.width: themeChoice.visualFocus || themeChoice.checked ? 2 : 1
                        border.color: themeChoice.checked || themeChoice.visualFocus ? root.theme.accent : root.theme.glassBorder
                    }
                    contentItem: ColumnLayout {
                        spacing: 6
                        AppIcon {
                            Layout.alignment: Qt.AlignHCenter
                            name: themeChoice.modelData.icon
                            size: 22
                            color: themeChoice.checked ? root.theme.accent : root.theme.outline
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: themeChoice.text
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: root.theme.textPrimary
                        }
                    }
                }
            }
        }
        Text {
            width: parent.width
            text: "「跟随系统」随桌面环境的明暗设置自动切换。选择后立即应用到整个界面。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 12
        }
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
        GlassSwitch {
            objectName: "reduceMotionSwitch"
            width: parent.width; theme: root.theme; text: "减少动态效果"
            description: "停用装饰过渡。真实波前、预计倒计时和数据更新不受影响。"
            checked: app.settings.reduceMotion
            onToggled: app.settings.reduceMotion = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "地图底图"; iconName: "map"
        Column {
            width: parent.width; spacing: 10
            Text { text: "底图"; color: root.theme.textPrimary; font.pixelSize: 14 }
            GlassComboBox {
                objectName: "basemapCombo"
                width: parent.width; theme: root.theme; Accessible.name: "底图"
                model: ["高德标准 · GCJ-02", "Petal · GCJ-02", "OpenStreetMap · WGS-84", "自定义底图"]
                readonly property var ids: ["amap_vector", "petal", "osm", "custom"]
                // 未知 id（旧版残留，如已下线的卫星图）回落到第一项而不是空白。
                currentIndex: Math.max(0, ids.indexOf(app.settings.basemapId))
                onActivated: index => app.settings.basemapId = ids[index]
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
}
