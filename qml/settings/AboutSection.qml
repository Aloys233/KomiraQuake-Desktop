import QtQuick
import QtQuick.Layouts

// 「自启与更新」分区。
Column {
    id: root
    objectName: "settingsPanel-about"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "启动"; iconName: "activity"
        GlassSwitch {
            objectName: "autoStartSwitch"
            width: parent.width; theme: root.theme; text: "开机自启"
            checked: app.autoStart.enabled
            onToggled: app.autoStart.enabled = checked
        }
        GlassSwitch {
            objectName: "silentStartSwitch"
            width: parent.width; theme: root.theme; text: "静默启动"
            description: "启动时只显示托盘图标。"
            checked: app.settings.silentStart
            onToggled: app.settings.silentStart = checked
        }
        GlassSwitch {
            objectName: "closeToTraySwitch"
            width: parent.width; theme: root.theme; text: "关闭窗口时收进托盘"
            description: "关闭后继续在后台运行。"
            checked: app.settings.closeToTray
            onToggled: app.settings.closeToTray = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "开发者"; iconName: "sliders-horizontal"
        GlassSwitch {
            objectName: "devModeSwitch"
            width: parent.width; theme: root.theme; text: "开发者模式"
            description: "显示模拟数据源。"
            checked: app.settings.developerMode
            onToggled: app.settings.developerMode = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "版本"; iconName: "refresh-cw"
        RowLayout {
            width: parent.width; spacing: 12
            Text {
                Layout.fillWidth: true
                text: "当前版本 v" + app.updater.currentVersion
                color: root.theme.textPrimary; font.pixelSize: 14
                font.family: root.theme.numberFamily
            }
            GlassButton {
                objectName: "checkUpdatesButton"
                theme: root.theme; text: "检查更新"; iconName: "refresh-cw"
                enabled: app.updater.state !== "checking"
                onClicked: app.updater.check(false)
            }
            GlassButton {
                objectName: "openReleaseButton"
                visible: app.updater.state === "updateAvailable"
                theme: root.theme; text: "打开下载页"; iconName: "external-link"; primary: true
                onClicked: app.updater.openReleasePage()
            }
        }
        Text {
            width: parent.width; visible: app.updater.message !== ""
            text: app.updater.message; wrapMode: Text.Wrap; font.pixelSize: 12
            color: app.updater.state === "updateAvailable" ? root.theme.accent
                 : app.updater.state === "error" ? root.theme.severity("WARNING")
                 : root.theme.outline
        }
        GlassSwitch {
            objectName: "autoCheckUpdatesSwitch"
            width: parent.width; theme: root.theme; text: "自动检查更新"
            checked: app.settings.autoCheckUpdates
            onToggled: app.settings.autoCheckUpdates = checked
        }
    }
}
