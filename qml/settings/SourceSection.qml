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

    Text {
        width: parent.width
        text: "各数据源平级、互为备份；关闭某个源后应用会改用其余源。"
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

                // 连接状态点 + 文案 + 延迟。状态点用 severity 配色，一眼可辨。
                RowLayout {
                    width: parent.width; spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        color: root.theme.severity(card.modelData.statusTag)
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
                        text: card.modelData.latency
                        color: root.theme.outline; font.pixelSize: 12
                        font.family: root.theme.numberFamily
                    }
                }

                // 目录刷新。点按展开/收起完整说明。
                Column {
                    id: dirBlock
                    width: parent.width; spacing: 6
                    /// 说明默认单行省略，点按展开完整说明。
                    property bool expanded: false
                    Item {
                        width: parent.width; height: dirRow.implicitHeight
                        RowLayout {
                            id: dirRow
                            width: parent.width; spacing: 8
                            Rectangle {
                                Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                                color: root.theme.severity(card.modelData.directoryStatusTag)
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                Layout.fillWidth: true
                                text: card.modelData.directory
                                  + (card.modelData.directoryLatency === "— ms" ? "" : " · " + card.modelData.directoryLatency)
                                color: root.theme.outline; font.pixelSize: 12
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }
                            AppIcon {
                                name: dirBlock.expanded ? "chevron-down" : "chevron-right"
                                size: 14; color: root.theme.outline
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dirBlock.expanded = !dirBlock.expanded
                        }
                    }
                    Text {
                        width: parent.width
                        visible: dirBlock.expanded
                        text: card.modelData.description
                        wrapMode: Text.Wrap; lineHeight: 1.4
                        color: root.theme.outline; font.pixelSize: 11
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
                    Text {
                        width: parent.width
                        text: "登录成功后刷新令牌会保存在本机，访问令牌由应用自动续取。设备有连接数上限，请避免频繁重连。"
                        color: root.theme.outline; font.pixelSize: 11; wrapMode: Text.Wrap; lineHeight: 1.4
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
                    Text {
                        width: parent.width
                        text: "报文机构固定为 SIM，不会与真实地震合并。同一地址重放会被终态墓碑压制，"
                            + "请在服务端控制台用「新一轮」换 id。"
                        color: root.theme.outline; font.pixelSize: 11; wrapMode: Text.Wrap; lineHeight: 1.4
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
                    Text {
                        width: parent.width
                        text: "令牌保存后本机直连，不经第三方。单令牌最多 20 条并发连接，本应用只用 1 条聚合连接。"
                        color: root.theme.outline; font.pixelSize: 11; wrapMode: Text.Wrap; lineHeight: 1.4
                    }
                }
            }
        }
    }
}
