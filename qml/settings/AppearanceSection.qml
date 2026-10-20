import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「外观」分区：主题与渲染效果。地图底图/图层已独立到「地图与图层」，避免
// 一个分区同时承担两件互不相关的事。
Column {
    id: root
    objectName: "settingsPanel-appearance"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "主题"; iconName: "sun"
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
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "显示效果"; iconName: "eye"
        GlassSwitch {
            objectName: "backgroundBlurSwitch"
            width: parent.width; theme: root.theme; text: "背景模糊"
            // 渲染后端不支持模糊时只给一句说明；开关仍可写回设置，换到支持的
            // 后端（或下次启动拿到 GPU）后即刻生效。
            readonly property bool supported: root.GraphicsInfo.api !== GraphicsInfo.Software
                                              && root.GraphicsInfo.api !== GraphicsInfo.Unknown
            description: supported ? "" : "当前渲染环境不支持。"
            checked: app.settings.backgroundBlur
            onToggled: app.settings.backgroundBlur = checked
        }
        GlassSwitch {
            objectName: "reduceMotionSwitch"
            width: parent.width; theme: root.theme; text: "减少动态效果"
            description: "减弱过渡动画。"
            checked: app.settings.reduceMotion
            onToggled: app.settings.reduceMotion = checked
        }
    }
}
