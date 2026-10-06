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
            description: "登录系统后自动启动 KomiraQuake，保持预警连接。"
            checked: app.autoStart.enabled
            onToggled: app.autoStart.enabled = checked
        }
        GlassSwitch {
            objectName: "silentStartSwitch"
            width: parent.width; theme: root.theme; text: "静默启动"
            description: "启动时不显示主窗口，只保留托盘图标；可从托盘菜单随时打开（等同于关闭窗口）。"
            checked: app.settings.silentStart
            onToggled: app.settings.silentStart = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "开发者"; iconName: "sliders-horizontal"
        GlassSwitch {
            objectName: "devModeSwitch"
            width: parent.width; theme: root.theme; text: "开发者模式"
            description: "显出「数据源」页的模拟数据源卡片，用于在没有真实地震时演练告警链路。"
            checked: app.settings.developerMode
            onToggled: app.settings.developerMode = checked
        }
        Text {
            width: parent.width
            text: "模拟源需要另填一个 WebSocket 地址（默认 ws://127.0.0.1:8080/ws），"
                + "并从「数据源」页启用它。此外地震预警总开关默认为关，"
                + "要测试声音与全屏预警需单独打开。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 11
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
            description: "启动时在后台静默检查一次新版本，发现更新后在此提示。"
            checked: app.settings.autoCheckUpdates
            onToggled: app.settings.autoCheckUpdates = checked
        }
    }
}
