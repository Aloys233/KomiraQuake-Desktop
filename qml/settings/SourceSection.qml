import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「数据源」分区：每个数据源一张独立卡片。
//
// 卡片化而非单张大卡内的列表行，是因为各源的可用性差异很大——有的常年在线、有的默认关闭
// 且需要凭据。一源一卡后卡片本身就是分组边界：开关、连接状态、目录刷新、需要凭据时的
// 密钥管理都收在同一张卡里。
Column {
    id: root
    objectName: "settingsPanel-source"
    property Theme theme: Theme {}
    width: parent.width
    spacing: 12

    /// 把持久化的震级阈值映射回下拉选项下标；非标准值取最近档。
    function infoFilterIndex(value) {
        if (value < 0) return 1;
        if (value <= 0) return 0;
        return Math.max(2, Math.min(10, Math.round((value - 1.0) / 0.5) + 2));
    }

    Text {
        width: parent.width
        text: "多个数据源互为备份；关闭某个源后应用会改用其余源。"
        wrapMode: Text.Wrap; lineHeight: 1.4
        color: root.theme.outline; font.pixelSize: 12
    }

    // 逐数据源渲染：新增源自动出现，无需改本页。
    Repeater {
        model: app.sources
        delegate: GlassCard {
            id: card
            required property var modelData
            width: parent.width
            implicitHeight: body.implicitHeight + 32
            opacity: card.modelData.enabled ? 1 : 0.6
            // 模拟源仅开发自测：开发者模式关闭时整张卡片不出现。
            visible: card.modelData.id !== "simulated" || app.settings.developerMode

            Column {
                id: body
                x: 16; y: 16
                width: parent.width - 32
                spacing: 10

                GlassSwitch {
                    objectName: card.modelData.id + "Switch"
                    width: parent.width; theme: root.theme
                    text: card.modelData.name
                    checked: app.settings.disabledSources.indexOf(card.modelData.id) < 0
                    onToggled: app.settings.setSourceEnabled(card.modelData.id, checked)
                }

                // 连接状态点 + 文案 + 延迟。状态点用连接状态配色，一眼可辨。
                RowLayout {
                    width: parent.width; spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        // statusTag 是连接态标签（CONNECTED/CONNECTING/…），须走 connectionColor；
                        // 若误用 severity()（它只认 NORMAL/WATCH/WARNING/CRITICAL），
                        // 所有状态都会落到默认中性色，圆点永远看不出在线/断开。
                        color: root.theme.connectionColor(card.modelData.statusTag)
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.modelData.status
                        color: root.theme.textPrimary; font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Text {
                        // 只在有实测延迟时显示；未连接不占位（「— ms」对用户是噪声）。
                        visible: card.modelData.latency !== "— ms"
                        text: card.modelData.latency
                        color: root.theme.outline; font.pixelSize: 12
                        font.family: root.theme.numberFamily
                    }
                }

                // 目录刷新只在出问题时才占一行：成功/未刷新与上一行的连接态重复，
                // 失败原因（如令牌失效）才是用户需要看到的信息。
                RowLayout {
                    width: parent.width; spacing: 8
                    visible: card.modelData.directoryStatusTag === "WARNING"
                             || card.modelData.directoryStatusTag === "WATCH"
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        color: root.theme.severity(card.modelData.directoryStatusTag)
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.modelData.directory
                        color: root.theme.outline; font.pixelSize: 12
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }

                // Jian 需登录：填登录密钥（邮件获取）换取刷新令牌。密钥管理收在本源自己的卡片里。
                // 未配置时始终显示，配置后仅在启用 Jian 时显示。
                Column {
                    width: parent.width; spacing: 6
                    visible: card.modelData.id === "jian"
                        && (!app.settings.jianConfigured || app.settings.disabledSources.indexOf("jian") < 0)
                    Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
                    Text { text: "密钥管理"; color: root.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                    GlassTextField {
                        id: jianKeyField; objectName: "jianLoginField"
                        width: parent.width; theme: root.theme
                        text: ""; placeholderText: "粘贴登录密钥 lk_…（邮件获取）"
                        Accessible.name: "Jian 登录密钥"
                    }
                    GlassButton {
                        objectName: "jianLoginButton"
                        theme: root.theme; text: "登录"; iconName: "check"
                        enabled: jianKeyField.text.trim().length > 0
                        onClicked: { app.loginJian(jianKeyField.text.trim()); jianKeyField.text = ""; }
                    }
                }

                // 模拟源（仅开发自测）：填自建服务端的地址。留空即视为未配置，不会连接。
                Column {
                    width: parent.width; spacing: 6
                    visible: card.modelData.id === "simulated"
                    Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
                    Text { text: "模拟源地址"; color: root.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                    GlassTextField {
                        id: simulatedUrlField; objectName: "simulatedUrlField"
                        width: parent.width; theme: root.theme
                        // 绑定当前生效值，便于确认已保存的地址。
                        text: app.settings.simulatedUrl
                        placeholderText: "如 ws://127.0.0.1:8080/ws（留空视为未配置）"
                        Accessible.name: "模拟数据源地址"
                    }
                    GlassButton {
                        objectName: "simulatedUrlSaveButton"
                        theme: root.theme; text: "保存"; iconName: "check"
                        enabled: simulatedUrlField.text.trim() !== app.settings.simulatedUrl
                        onClicked: app.setSimulatedUrl(simulatedUrlField.text.trim())
                    }
                }

                // Whews 需令牌：直接在 auth.beecld.com 申请 wat_…，粘贴即生效（无需换票）。
                Column {
                    width: parent.width; spacing: 6
                    visible: card.modelData.id === "whews"
                        && (!app.settings.whewsConfigured || app.settings.disabledSources.indexOf("whews") < 0)
                    Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }
                    Text { text: "密钥管理"; color: root.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                    GlassTextField {
                        id: whewsTokenField; objectName: "whewsTokenField"
                        width: parent.width; theme: root.theme
                        text: ""; placeholderText: "粘贴令牌 wat_…（auth.beecld.com 个人中心申请）"
                        Accessible.name: "Whews 令牌"
                    }
                    GlassButton {
                        objectName: "whewsTokenSaveButton"
                        theme: root.theme; text: "保存"; iconName: "check"
                        enabled: whewsTokenField.text.trim().length > 0
                        onClicked: { app.setWhewsToken(whewsTokenField.text.trim()); whewsTokenField.text = ""; }
                    }
                }
            }
        }
    }

    // 测站显示源：只画地图、不产事件，故与上面的事件源分开管理。
    SettingsSection {
        width: parent.width; theme: root.theme; title: "测站显示源"; iconName: "activity"
        GlassSwitch {
            objectName: "kmoniStationSwitch"
            width: parent.width; theme: root.theme
            text: "NIED 强震监测"
            checked: app.settings.isStationSourceEnabled("kmoni")
            onToggled: app.settings.setStationSourceEnabled("kmoni", checked)
        }
        GlassSwitch {
            objectName: "snetStationSwitch"
            width: parent.width; theme: root.theme
            text: "S-Net 海底观测网"
            checked: app.settings.isStationSourceEnabled("snet")
            onToggled: app.settings.setStationSourceEnabled("snet", checked)
        }
        GlassSwitch {
            objectName: "kmaStationSwitch"
            width: parent.width; theme: root.theme
            text: "KMA PEWS 测站"
            checked: app.settings.isStationSourceEnabled("kma_station")
            onToggled: app.settings.setStationSourceEnabled("kma_station", checked)
        }
        GlassSwitch {
            objectName: "pancakesSeisSwitch"
            width: parent.width; theme: root.theme
            text: "PancakesSeis 加速度台站"
            checked: app.settings.isStationSourceEnabled("pancakes_seis")
            onToggled: app.settings.setStationSourceEnabled("pancakes_seis", checked)
        }
        GlassSwitch {
            objectName: "wolfxSeisSwitch"
            width: parent.width; theme: root.theme
            text: "Wolfx SeisJS 台站"
            checked: app.settings.isStationSourceEnabled("wolfx_seis")
            onToggled: app.settings.setStationSourceEnabled("wolfx_seis", checked)
        }
    }

    // 逐源信息事件震级过滤：只过滤目录情报，不影响实时预警。
    SettingsSection {
        width: parent.width; theme: root.theme; title: "信息事件震级过滤"; iconName: "sliders-horizontal"
        Text {
            width: parent.width
            text: "低于所选震级的地震信息不显示。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 12
        }
        Repeater {
            model: app.sources
            delegate: RowLayout {
                required property var modelData
                width: parent.width; spacing: 12
                visible: modelData.id !== "simulated" || app.settings.developerMode
                Text {
                    Layout.fillWidth: true
                    text: modelData.name
                    color: root.theme.textPrimary; font.pixelSize: 14
                    elide: Text.ElideRight
                }
                GlassComboBox {
                    objectName: "infoFilter-" + modelData.id
                    Layout.preferredWidth: 132
                    theme: root.theme
                    Accessible.name: "信息事件震级过滤 · " + modelData.name
                    model: ["不过滤", "不接收", "M 1.0", "M 1.5", "M 2.0", "M 2.5", "M 3.0", "M 3.5", "M 4.0", "M 4.5", "M 5.0"]
                    currentIndex: root.infoFilterIndex(app.settings.infoMagnitudeFilter(modelData.id))
                    onActivated: index => app.settings.setInfoMagnitudeFilter(
                        modelData.id, [0, -1, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0][index])
                }
            }
        }
    }
}
