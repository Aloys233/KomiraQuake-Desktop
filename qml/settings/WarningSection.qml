import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「预警提醒」分区：先决定「什么值得提醒」，再逐级决定「用什么方式提醒」。
// 提醒方式按事件级别拆成矩阵：警报级 / 任意预警 / 地震信息，各自可独立选择
// 系统通知与置顶主窗口；声音在「声音」分区单独细分。
Column {
    id: root
    objectName: "settingsPanel-warning"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "提醒条件"; iconName: "shield"
        GlassSwitch {
            objectName: "warningsSwitch"
            width: parent.width; theme: root.theme; text: "地震预警"
            description: app.settings.enableWarnings ? "" : "已关闭，地震只在界面上展示。"
            checked: app.settings.enableWarnings
            onToggled: app.settings.enableWarnings = checked
        }
        Column {
            width: parent.width; spacing: 10
            enabled: app.settings.enableWarnings
            opacity: enabled ? 1 : 0.55
            Text {
                text: "烈度标准"
                color: root.theme.textPrimary
                font.pixelSize: 14
            }
            GlassComboBox {
                width: parent.width; theme: root.theme
                model: ["中国烈度 · CSIS", "日本震度 · JMA"]
                Accessible.name: "烈度标准"
                currentIndex: app.settings.intensityStandard
                onActivated: index => app.settings.intensityStandard = index
            }
        }
        SettingSlider {
            width: parent.width; theme: root.theme
            title: "本地烈度过滤"
            valueText: app.settings.localIntensityFilter <= 0
                ? "关闭"
                : app.settings.localIntensityFilter.toFixed(1) + " 度"
            description: "低于该烈度不提醒；0 表示不筛选。"
            from: 0; to: 8; stepSize: 0.5
            value: app.settings.localIntensityFilter
            active: app.settings.enableWarnings
            onMoved: value => app.settings.localIntensityFilter = value
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "提醒方式"; iconName: "bell"

        // 警报级：通过本地烈度过滤，会触发倒计时与声音。
        Column {
            width: parent.width; spacing: 8
            // 只让开关自己置灰（GlassSwitch 内置 enabled 处理），说明文字保持可读。
            enabled: app.settings.enableWarnings
            Text { text: "警报级预警"; color: root.theme.textPrimary; font.pixelSize: 14; font.weight: Font.Medium }
            Text {
                width: parent.width
                text: "达到烈度过滤阈值。"
                wrapMode: Text.Wrap; lineHeight: 1.35
                color: root.theme.outline; font.pixelSize: 12
            }
            RowLayout {
                width: parent.width; spacing: 12
                GlassSwitch {
                    objectName: "notifyAlertSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "系统通知"
                    checked: app.settings.notifyAlert
                    onToggled: app.settings.notifyAlert = checked
                }
                GlassSwitch {
                    objectName: "raiseAlertSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "置顶主窗口"
                    checked: app.settings.raiseAlert
                    onToggled: app.settings.raiseAlert = checked
                }
            }
        }

        // 任意预警：未达本地烈度阈值的实时预警与更新。
        Column {
            width: parent.width; spacing: 8
            enabled: app.settings.enableWarnings
            Text { text: "任意预警"; color: root.theme.textPrimary; font.pixelSize: 14; font.weight: Font.Medium }
            Text {
                width: parent.width
                text: "未达阈值的预警与更新报。"
                wrapMode: Text.Wrap; lineHeight: 1.35
                color: root.theme.outline; font.pixelSize: 12
            }
            RowLayout {
                width: parent.width; spacing: 12
                GlassSwitch {
                    objectName: "notifyEewSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "系统通知"
                    checked: app.settings.notifyEew
                    onToggled: app.settings.notifyEew = checked
                }
                GlassSwitch {
                    objectName: "raiseEewSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "置顶主窗口"
                    checked: app.settings.raiseEew
                    onToggled: app.settings.raiseEew = checked
                }
            }
        }

        // 地震信息：目录中的情报，独立于预警总开关。
        Column {
            width: parent.width; spacing: 8
            Text { text: "地震信息"; color: root.theme.textPrimary; font.pixelSize: 14; font.weight: Font.Medium }
            Text {
                width: parent.width
                text: "目录中的地震信息。"
                wrapMode: Text.Wrap; lineHeight: 1.35
                color: root.theme.outline; font.pixelSize: 12
            }
            RowLayout {
                width: parent.width; spacing: 12
                GlassSwitch {
                    objectName: "notifyInfoSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "系统通知"
                    checked: app.settings.notifyInfo
                    onToggled: app.settings.notifyInfo = checked
                }
                GlassSwitch {
                    objectName: "raiseInfoSwitch"
                    Layout.fillWidth: true; theme: root.theme; text: "置顶主窗口"
                    checked: app.settings.raiseInfo
                    onToggled: app.settings.raiseInfo = checked
                }
            }
        }
    }
}
