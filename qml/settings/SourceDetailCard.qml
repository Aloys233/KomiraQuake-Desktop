import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 单个事件源的详情（数据源分栏的右列）。**不是卡片**：它住在 `SourceSection` 那张
// 「事件源」卡里，自己再画一层边框就会和外面的卡形成「卡中卡」的双层边。
// 旧实现是独立 GlassCard + 独立左栏卡，两张卡各自长高，接缝处两条描边对不齐、
// 内容底边也不等高 —— 那正是「分栏没弄好」的观感来源。
Item {
    id: card
    /// 主题必须由父级注入：本文件不是 GlassCard，没有自己的 theme。
    property Theme theme: Theme {}
    property var source: null
    /// 子数据源面板是否展开。只在真正换源时收起：app.sources 会随链路状态频繁重建，
    /// 同一个源的快照对象被替换时不能把面板收掉。
    property bool subExpanded: false
    readonly property string sourceId: source ? source.id : ""
    /// 内容自然高度（不含内边距）。父级据此决定这张卡整体多高。
    readonly property real naturalHeight: body.implicitHeight
    /// 被父级压到比内容矮 → 列内滚动。窄屏时父级按自然高度给高度，这里为 false，
    /// 内容由整页的滚动条负责，此列不再出现第二层滚动。
    /// **只读 height 与 naturalHeight**：body 的宽度刻意**不**依赖本属性（滚动条槽位
    /// 恒占 12 px），否则 `naturalHeight → body.width → naturalHeight` 会形成绑定环。
    readonly property bool scrollable: height + 0.5 < naturalHeight
    /// 滚动条槽位恒占：出现/消失滚动条时文字宽度不变，不会整段回流。
    readonly property int gutter: 12
    /// 延迟文案的**唯一实现**由父级注入（`SourceSection.sourceLatencyText`）。
    /// 这里不再自己拼一遍：左列行与右列状态行必须口径一致，两份实现迟早会漂。
    /// 注入的是函数对象（QML 里 `property var` 可以持有 JS 函数）。
    property var latencyFormatter: null
    readonly property string latencyText: latencyFormatter && source
        ? latencyFormatter(source) : ""

    onSourceIdChanged: subExpanded = false

    visible: !!source

    /// 把持久化的震级阈值映射回下拉选项下标；非标准值取最近档。
    function infoFilterIndex(value) {
        if (value < 0) return 1;
        if (value <= 0) return 0;
        return Math.max(2, Math.min(10, Math.round((value - 1.0) / 0.5) + 2));
    }

    Flickable {
        id: scroller
        objectName: "sourceDetailScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: body.implicitHeight
        clip: true
        // 只有被父级高度截断时才允许内部滚动。窄屏时由整页滚动条负责，这里再套一层
        // 会让内容被裁掉一半却看不出为什么。
        // 用 interactive 而不是 enabled：enabled=false 会连同子控件一起禁用，
        // 卡片里的开关与折叠头就点不动了。
        interactive: card.scrollable
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        // 内容被截断时必须有可见的滚动条：Whews 有 41 个子源，只靠滚轮没有任何
        // 「下面还有内容」的提示。
        ScrollBar.vertical: ScrollBar {
            objectName: "sourceDetailScrollBar"
            policy: card.scrollable ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            width: 6
            padding: 1
            background: Item {}
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: card.theme.outlineVariant
                opacity: parent.pressed || parent.hovered ? 1.0 : 0.65
            }
        }

        Column {
            id: body
            width: scroller.width - card.gutter
            spacing: 10

            // 源自己的抬头：开关文案就是源名，避免「Wolfx / Wolfx」重复两行。
            GlassSwitch {
                objectName: card.sourceId + "Switch"
                width: parent.width; theme: card.theme
                text: card.source ? card.source.name : ""
                checked: app.settings.disabledSources.indexOf(card.sourceId) < 0
                onToggled: app.settings.setSourceEnabled(card.sourceId, checked)
            }

            // 关闭态只在这里说明一次：整列不再像旧版那样整体降到 0.6 透明度
            // （那会让开关、文字一起发灰，看起来像渲染坏了）。
            Text {
                width: parent.width
                visible: !!card.source && !card.source.enabled
                text: "已关闭，不会建立任何连接。"
                color: card.theme.outline; font.pixelSize: 12
                wrapMode: Text.Wrap; lineHeight: 1.3
            }

            // 概况：这个源收哪些机构的报文、覆盖哪里 —— 用户选择源时要看的就是这些。
            Text {
                width: parent.width
                visible: text.length > 0 && !!card.source
                text: card.source ? card.source.description : ""
                color: card.theme.outline; font.pixelSize: 12
                wrapMode: Text.Wrap; lineHeight: 1.3
            }
            Row {
                spacing: 6
                visible: !!card.source && card.source.region.length > 0
                Rectangle {
                    width: regionLabel.implicitWidth + 16; height: 22; radius: 11
                    color: card.theme.accentContainer
                    Text {
                        id: regionLabel
                        anchors.centerIn: parent
                        text: card.source ? card.source.region : ""
                        color: card.theme.accent; font.pixelSize: 11
                    }
                }
            }

            // 连接状态 + 延迟。延迟是「链路此刻有多快」，所以和状态同一行：断开时它
            // 自然为空（源的 setStatus 会把 latencyMs 置 -1），不会留下一个过时的数。
            RowLayout {
                objectName: "sourceStatusRow"
                width: parent.width; spacing: 8
                Rectangle {
                    Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                    // statusTag 是连接态标签（CONNECTED/CONNECTING/…），须走 connectionColor；
                    // 若误用 severity()（它只认 NORMAL/WATCH/WARNING/CRITICAL），
                    // 所有状态都会落到默认中性色，圆点永远看不出在线/断开。
                    color: card.theme.connectionColor(card.source ? card.source.statusTag : "DISCONNECTED")
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: card.source ? card.source.status : ""
                    color: card.theme.textPrimary; font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "sourceLatencyLabel"
                    // 只在真测到延迟时出现；未测出不占位（「— ms」对用户是噪声）。
                    visible: card.latencyText.length > 0
                    text: card.latencyText
                    color: card.theme.outline; font.pixelSize: 11
                    font.family: card.theme.numberFamily
                }
            }

            // 目录刷新只在出问题时才占一行：成功/未刷新与上一行的连接态重复，
            // 失败原因（如令牌失效）才是用户需要看到的信息。
            RowLayout {
                width: parent.width; spacing: 8
                visible: !!card.source && (card.source.directoryStatusTag === "WARNING"
                                            || card.source.directoryStatusTag === "WATCH")
                Rectangle {
                    Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                    color: card.theme.severity(card.source ? card.source.directoryStatusTag : "NORMAL")
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: card.source ? card.source.directory : ""
                    color: card.theme.outline; font.pixelSize: 12
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }

            // 情报震级过滤：只作用于目录情报，不影响实时预警，所以归到源自己的详情里
            // （以前是分区底部的独立卡片，横向铺满会把左右分栏切断）。
            Column {
                width: parent.width
                spacing: 6
                visible: card.sourceId.length > 0

                Rectangle { width: parent.width; height: 1; color: card.theme.glassBorder }

                RowLayout {
                    width: parent.width; spacing: 12
                    Column {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: "地震情报震级过滤"
                            color: card.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium
                        }
                        Text {
                            width: parent.width
                            text: "低于所选震级的情报事件不显示；实时预警不受影响。"
                            color: card.theme.outline; font.pixelSize: 11
                            wrapMode: Text.Wrap; lineHeight: 1.3
                        }
                    }
                    GlassComboBox {
                        objectName: "infoFilter-" + card.sourceId
                        Layout.preferredWidth: 132
                        theme: card.theme
                        Accessible.name: "地震情报震级过滤 · " + (card.source ? card.source.name : "")
                        model: ["不过滤", "不接收", "M 1.0", "M 1.5", "M 2.0", "M 2.5", "M 3.0", "M 3.5", "M 4.0", "M 4.5", "M 5.0"]
                        currentIndex: card.infoFilterIndex(app.settings.infoMagnitudeFilter(card.sourceId))
                        onActivated: index => app.settings.setInfoMagnitudeFilter(
                            card.sourceId, [0, -1, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0][index])
                    }
                }
            }

            // 子数据源：聚合源内部的频道级启停。默认全部启用；关闭后该频道不再解析，
            // 对应的目录端点也不再请求（全部关闭时整轮目录轮询都会跳过）。
            Column {
                id: subBlock
                width: parent.width
                spacing: 6
                visible: !!card.source && card.source.subSources.length > 0

                Rectangle { width: parent.width; height: 1; color: card.theme.glassBorder }

                // 折叠头：标题 + 启用计数 + 展开箭头。整行可点，命中区覆盖整行。
                Item {
                    objectName: card.sourceId + "SubHeader"
                    signal clicked()
                    width: parent.width
                    height: 28
                    Accessible.role: Accessible.Button
                    Accessible.name: "子数据源，展开或收起"
                    onClicked: card.subExpanded = !card.subExpanded
                    RowLayout {
                        anchors.fill: parent
                        spacing: 8
                        Text {
                            text: "子数据源"
                            color: card.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium
                        }
                        Text {
                            Layout.fillWidth: true
                            text: {
                                const list = card.source ? card.source.subSources : []
                                let on = 0
                                for (let i = 0; i < list.length; ++i) if (list[i].enabled) on++
                                return on + "/" + list.length + " 已启用"
                            }
                            color: card.theme.outline; font.pixelSize: 12
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                        AppIcon {
                            name: card.subExpanded ? "chevron-down" : "chevron-right"
                            color: card.theme.outline; size: 16
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: parent.clicked()
                    }
                }

                Column {
                    width: parent.width
                    spacing: 2
                    visible: card.subExpanded

                    // 按 kind 分组：实时预警（告警链路）与地震情报（仅列表/历史）。
                    Repeater {
                        model: card.source ? card.source.subSources : []
                        delegate: Column {
                            id: subRow
                            required property var modelData
                            required property int index
                            width: parent.width
                            spacing: 2

                            readonly property var allSubs: card.source.subSources
                            readonly property bool firstOfKind: index === 0
                                || allSubs[index - 1].kind !== modelData.kind
                            readonly property string kindLabel: modelData.kind === "live"
                                ? "实时预警" : "地震情报"

                            function idsOfKind() {
                                const out = []
                                for (let i = 0; i < allSubs.length; ++i)
                                    if (allSubs[i].kind === modelData.kind) out.push(allSubs[i].id)
                                return out
                            }
                            function enabledOfKind() {
                                let on = 0
                                for (let i = 0; i < allSubs.length; ++i)
                                    if (allSubs[i].kind === modelData.kind && allSubs[i].enabled) on++
                                return on
                            }

                            RowLayout {
                                width: parent.width
                                visible: subRow.firstOfKind
                                spacing: 12

                                Text {
                                    Layout.fillWidth: true
                                    text: subRow.kindLabel + "  "
                                          + subRow.enabledOfKind() + "/" + subRow.idsOfKind().length
                                    color: card.theme.outline; font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                                Item {
                                    objectName: card.sourceId + "SubAll-" + subRow.modelData.kind
                                    signal clicked()
                                    implicitWidth: allLabel.implicitWidth + 6
                                    implicitHeight: 24
                                    Accessible.role: Accessible.Button
                                    Accessible.name: subRow.kindLabel + "全开"
                                    onClicked: app.settings.setSubSourcesEnabled(
                                        card.sourceId, subRow.idsOfKind(), true)
                                    Text {
                                        id: allLabel
                                        anchors.centerIn: parent
                                        text: "全开"; color: card.theme.accent; font.pixelSize: 12
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: parent.clicked()
                                    }
                                }
                                Item {
                                    objectName: card.sourceId + "SubNone-" + subRow.modelData.kind
                                    signal clicked()
                                    implicitWidth: noneLabel.implicitWidth + 6
                                    implicitHeight: 24
                                    Accessible.role: Accessible.Button
                                    Accessible.name: subRow.kindLabel + "全不选"
                                    onClicked: app.settings.setSubSourcesEnabled(
                                        card.sourceId, subRow.idsOfKind(), false)
                                    Text {
                                        id: noneLabel
                                        anchors.centerIn: parent
                                        text: "全不选"; color: card.theme.outline; font.pixelSize: 12
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: parent.clicked()
                                    }
                                }
                            }

                            GlassSwitch {
                                objectName: card.sourceId + "Sub-" + subRow.modelData.id
                                width: parent.width
                                theme: card.theme
                                text: subRow.modelData.name
                                checked: subRow.modelData.enabled
                                onToggled: app.settings.setSubSourceEnabled(
                                    card.sourceId, subRow.modelData.id, checked)
                            }
                        }
                    }
                }
            }

            // Jian 需登录：填登录密钥（邮件获取）换取刷新令牌。密钥管理常驻本源详情，
            // 便于随时更换 / 重新登录。
            Column {
                width: parent.width; spacing: 6
                visible: card.sourceId === "jian"
                Rectangle { width: parent.width; height: 1; color: card.theme.glassBorder }
                Text { text: "密钥管理"; color: card.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                GlassTextField {
                    id: jianKeyField; objectName: "jianLoginField"
                    width: parent.width; theme: card.theme
                    text: ""; placeholderText: "粘贴登录密钥 lk_…（邮件获取）"
                    Accessible.name: "Jian 登录密钥"
                }
                GlassButton {
                    objectName: "jianLoginButton"
                    theme: card.theme; text: "登录"; iconName: "check"
                    enabled: jianKeyField.text.trim().length > 0
                    onClicked: { app.loginJian(jianKeyField.text.trim()); jianKeyField.text = ""; }
                }
            }

            // 模拟源（仅开发自测）：填自建服务端的地址。留空即视为未配置，不会连接。
            Column {
                width: parent.width; spacing: 6
                visible: card.sourceId === "simulated"
                Rectangle { width: parent.width; height: 1; color: card.theme.glassBorder }
                Text { text: "模拟源地址"; color: card.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                GlassTextField {
                    id: simulatedUrlField; objectName: "simulatedUrlField"
                    width: parent.width; theme: card.theme
                    // 绑定当前生效值，便于确认已保存的地址。
                    text: app.settings.simulatedUrl
                    placeholderText: "如 ws://127.0.0.1:8080/ws（留空视为未配置）"
                    Accessible.name: "模拟数据源地址"
                }
                GlassButton {
                    objectName: "simulatedUrlSaveButton"
                    theme: card.theme; text: "保存"; iconName: "check"
                    enabled: simulatedUrlField.text.trim() !== app.settings.simulatedUrl
                    onClicked: app.setSimulatedUrl(simulatedUrlField.text.trim())
                }
            }

            // Whews 需令牌：直接在 auth.beecld.com 申请 wat_…，粘贴即生效（无需换票）。
            // 密钥管理常驻本源详情，便于随时更换。
            Column {
                width: parent.width; spacing: 6
                visible: card.sourceId === "whews"
                Rectangle { width: parent.width; height: 1; color: card.theme.glassBorder }
                Text { text: "密钥管理"; color: card.theme.textPrimary; font.pixelSize: 13; font.weight: Font.Medium }
                GlassTextField {
                    id: whewsTokenField; objectName: "whewsTokenField"
                    width: parent.width; theme: card.theme
                    text: ""; placeholderText: "粘贴令牌 wat_…（auth.beecld.com 个人中心申请）"
                    Accessible.name: "Whews 令牌"
                }
                GlassButton {
                    objectName: "whewsTokenSaveButton"
                    theme: card.theme; text: "保存"; iconName: "check"
                    enabled: whewsTokenField.text.trim().length > 0
                    onClicked: { app.setWhewsToken(whewsTokenField.text.trim()); whewsTokenField.text = ""; }
                }
            }
        }
    }
}
