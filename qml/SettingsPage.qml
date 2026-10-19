import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 设置页外壳：工具条 + 侧栏（窄屏退化为下拉）+ 单一分区面板。
// 分区内容拆到 settings/*Section.qml；此处只负责导航与「恢复默认」。
Item {
    id: root
    objectName: "settingsPage"
    property Theme theme: Theme {}
    property int currentSection: 0
    readonly property bool compact: width < 760
    // 任务式分区：先决定「什么值得提醒」，再拆声音、地图、外观、定位、数据源与系统行为。
    readonly property var sections: [
        { key: "warning", title: "预警提醒", icon: "bell", description: "提醒条件与分级提醒方式。" },
        { key: "audio", title: "声音", icon: "volume-2", description: "警报音量与播报内容分轨。" },
        { key: "map", title: "地图与图层", icon: "map", description: "底图，以及地图上的矢量、测站与波前叠加层。" },
        { key: "appearance", title: "外观", icon: "sun", description: "主题、背景模糊与动态效果。" },
        { key: "location", title: "定位", icon: "map-pin", description: "设置用于估算本地烈度、距离与预计到时的位置。" },
        { key: "sourceEstimation", title: "震源推算", icon: "locate-fixed", description: "只用实时测站强度反推震中、震级与深度（Beta）。" },
        { key: "source", title: "数据源", icon: "radio", description: "逐源启停、测站显示与信息事件震级过滤。" },
        { key: "clock", title: "时间校准", icon: "clock", description: "网络授时：开关、自定义 NTP 服务器与校准状态。" },
        { key: "about", title: "启动与更新", icon: "refresh-cw", description: "开机自启、静默启动、关闭行为与版本更新。" }
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
                Text {
                    objectName: "settingsVersionLabel"
                    Layout.leftMargin: 12
                    Layout.bottomMargin: updateHint.visible ? 2 : 8
                    text: "v" + app.updater.currentVersion
                    color: root.theme.outline; font.pixelSize: 11
                }
                // 仅在有新版本时出现在版本号下方；无更新/检查中/失败都不占位。
                AbstractButton {
                    id: updateHint
                    objectName: "settingsUpdateHint"
                    visible: app.updater.state === "updateAvailable"
                    Layout.fillWidth: true
                    Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.bottomMargin: 10
                    Layout.preferredHeight: visible ? 30 : 0
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: "发现新版本 v" + app.updater.latestVersion + "，打开下载页"
                    ToolTip.visible: hovered
                    ToolTip.text: app.updater.message
                    onClicked: app.updater.openReleasePage()
                    background: Rectangle {
                        radius: 8
                        color: updateHint.down ? root.theme.accent : root.theme.accentContainer
                        border.width: updateHint.visualFocus ? 2 : 1
                        border.color: updateHint.hovered || updateHint.down ? root.theme.accent : root.theme.accentBorder
                    }
                    contentItem: RowLayout {
                        spacing: 6
                        AppIcon {
                            Layout.leftMargin: 8
                            name: "refresh-cw"; size: 13
                            color: updateHint.down ? root.theme.accentForeground : root.theme.accent
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "新版本 v" + app.updater.latestVersion
                            color: updateHint.down ? root.theme.accentForeground : root.theme.accent
                            font.pixelSize: 11; font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        AppIcon {
                            Layout.rightMargin: 8
                            name: "arrow-right"; size: 12
                            color: updateHint.down ? root.theme.accentForeground : root.theme.accent
                        }
                    }
                }
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

                    // 全部分区都实例化，仅按 currentSection 切换 visible。
                    WarningSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 0; height: visible ? implicitHeight : 0
                    }
                    AudioSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 1; height: visible ? implicitHeight : 0
                    }
                    MapSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 2; height: visible ? implicitHeight : 0
                    }
                    AppearanceSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 3; height: visible ? implicitHeight : 0
                    }
                    LocationSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 4; height: visible ? implicitHeight : 0
                        onPickLocationRequested: root.pickLocationRequested()
                        onNotify: (message, success) => toast.show(message, success)
                    }
                    SourceEstimationSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 5; height: visible ? implicitHeight : 0
                    }
                    SourceSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 6; height: visible ? implicitHeight : 0
                    }
                    ClockSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 7; height: visible ? implicitHeight : 0
                    }
                    AboutSection {
                        width: parent.width; theme: root.theme
                        visible: root.currentSection === 8; height: visible ? implicitHeight : 0
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
            Text { Layout.fillWidth: true; text: "外观、地图、预警和声音等偏好都将重置为初始值。此操作无法撤销。"; color: root.theme.outline; font.pixelSize: 13; wrapMode: Text.Wrap; lineHeight: 1.5 }
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
