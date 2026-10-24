import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 「数据源」分区（桌面端）：左侧事件源列表、右侧选中源详情的主从分栏。
//
// 为什么分栏：源在增长，逐个纵向铺开时每屏只能看到一两个源，各源的子源与凭据还会被推到
// 很远的位置。分栏后源列表始终可见，与某个源有关的全部设置（启停 / 状态 / 子源 / 凭据 /
// 情报震级过滤）都收在右列。
//
// **整块分栏是一张卡，而且与其它分区同节奏**：卡头（图标 + 标题 + 计数）→ 分割线 → 内容，
// 外边距与 spacing 都对齐 `SettingsSection`。左右两列住在同一张卡里，靠一条 1 px 竖线分隔。
//
// 旧版的两个问题：①「独立的左栏卡 + 独立的右栏卡 + 下面再一张铺满的测站源卡」——三张卡的
// 圆角/描边/内边距各自为政，左栏被单独撑高、下方留一截参差缺口，右栏内容一变就换高度，
// 读起来像「分栏只画了一半」；②这张卡没有卡头，上面还飘着一行裸文字，与其它分区的卡片
// 节奏对不上。现在分栏是一整块，右列内容再长也只是在卡内滚动。
//
// 窄屏放不下两列：列表收进下拉框，详情仍完整展示在下方（不再套第二层滚动）。
Column {
    id: root
    objectName: "settingsPanel-source"
    property Theme theme: Theme {}
    width: parent.width
    // 与其它分区一致（它们都是 spacing: 20）。旧值是 12，数据源页的块间距比别的页明显紧。
    spacing: 20

    /// 当前选中的数据源 id。存的是 id 而非列表下标：app.sources 会随链路状态频繁重建，
    /// 下标可能漂移，而 id 稳定。
    property string selectedSourceId: "wolfx"
    /// 820 宽的设置栏下左列 232 + 分隔线 1 + 列间 20 后，右列仍有 ~500 放密钥与子源开关。
    readonly property bool splitLayout: width >= 700
    /// 两列内容区的高度上限：再高就靠右列内部滚动，避免一屏只剩一个源。
    readonly property int maxSplitHeight: 500
    /// 卡的内边距，与 SettingsSection 的 20 一致。
    readonly property int pad: 20
    readonly property int navWidth: 232
    readonly property var visibleSources: app.sources.filter(function (s) {
        return s.id !== "simulated" || app.settings.developerMode
    })
    readonly property var selectedSource: {
        const list = visibleSources
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === selectedSourceId) return list[i]
        return list.length > 0 ? list[0] : null
    }
    readonly property int selectedIndex: {
        for (let i = 0; i < visibleSources.length; ++i)
            if (visibleSources[i].id === selectedSourceId) return i
        return 0
    }

    /// 两列的内容高度：取较高一列，分栏时封顶 maxSplitHeight。
    /// 窄屏只有一列，且整页本身就在滚动，不该再套一层内部滚动：高度按内容自然展开，
    /// 只用页面滚动条。
    readonly property real splitHeight: root.splitLayout
        ? Math.min(root.maxSplitHeight,
                   Math.max(navColumn.implicitHeight, detailCard.naturalHeight))
        : detailCard.naturalHeight

    /// 左栏第二行文案：关闭状态优先，其次是「缺凭据」，最后才是链路状态。
    function sourceStateText(item) {
        if (!item.enabled) return "已关闭"
        if (!item.configured && (item.id === "jian" || item.id === "whews"))
            return "未配置密钥"
        return item.status
    }

    /// 链路延迟文案（实时 WS / 目录 HTTP 两路各取数值，未测出的那一路省略）。
    /// **判空用数值而不是文本**：`latencyMs` 为 -1 表示未测出，此时整段为空、界面不占位。
    /// 不能显示「— ms」—— 那对用户只是噪声，还会把「已关闭」的行白撑高一行。
    function sourceLatencyText(item) {
        if (!item) return ""
        const parts = []
        if (item.latencyMs !== undefined && item.latencyMs >= 0)
            parts.push("实时 " + item.latencyMs + " ms")
        if (item.directoryLatencyMs !== undefined && item.directoryLatencyMs >= 0)
            parts.push("目录 " + item.directoryLatencyMs + " ms")
        return parts.join(" · ")
    }

    // 窄屏：源列表收进下拉框，避免左栏把详情挤到不可用宽度。
    GlassComboBox {
        objectName: "sourceSelectorCombo"
        width: parent.width
        visible: !root.splitLayout
        theme: root.theme
        Accessible.name: "数据源"
        model: root.visibleSources.map(function (s) { return s.name })
        currentIndex: root.selectedIndex
        onActivated: index => root.selectedSourceId = root.visibleSources[index].id
    }

    GlassCard {
        id: splitCard
        objectName: "sourceSplitCard"
        width: parent.width
        implicitHeight: cardColumn.implicitHeight + root.pad * 2
        theme: root.theme

        Column {
            id: cardColumn
            x: root.pad; y: root.pad
            width: parent.width - root.pad * 2
            spacing: 16

            // 卡头：与 SettingsSection 同款节奏（图标 + 标题，右侧给计数）。
            RowLayout {
                width: parent.width
                spacing: 10
                AppIcon {
                    name: "radio"; size: 18
                    color: root.theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    text: "事件源"
                    color: root.theme.textPrimary; font.pixelSize: 16; font.weight: Font.Medium
                }
                Text {
                    objectName: "sourceCountLabel"
                    text: root.visibleSources.length + " 个"
                    color: root.theme.outline; font.pixelSize: 11
                    Layout.alignment: Qt.AlignVCenter
                }
            }
            Rectangle { width: parent.width; height: 1; color: root.theme.glassBorder }

            Row {
                id: splitRow
                objectName: "sourceSplitRow"
                width: parent.width
                height: root.splitHeight
                spacing: 0

                // ── 左列：事件源列表 ─────────────────────────────────────────
                Item {
                    id: navCard
                    objectName: "sourceNavCard"
                    visible: root.splitLayout
                    // 窄屏置 0：不可见项不要再从行里分走宽度。
                    width: root.splitLayout ? root.navWidth : 0
                    height: parent.height

                    Flickable {
                        id: navScroller
                        objectName: "sourceNavScroll"
                        // 右侧留 12 给分隔线，列表不贴到线上。
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        contentWidth: width
                        contentHeight: navColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        // 用 interactive 而不是 enabled：enabled=false 会连同子控件一起禁用。
                        interactive: navColumn.implicitHeight > height
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        Column {
                            id: navColumn
                            width: navScroller.width
                            spacing: 2

                            Repeater {
                                model: root.visibleSources
                                delegate: Item {
                                    id: navRow
                                    required property var modelData
                                    objectName: "sourceNav-" + modelData.id
                                    signal clicked()
                                    readonly property bool current: root.selectedSourceId === modelData.id
                                    readonly property string latencyText: root.sourceLatencyText(modelData)
                                    width: parent.width
                                    // 有延迟时多一行：行高随内容走，固定 54 会把第三行挤掉。
                                    height: Math.max(52, navLabels.implicitHeight + 14)
                                    Accessible.role: Accessible.Button
                                    Accessible.name: modelData.name
                                    Accessible.description: root.sourceStateText(modelData)
                                    onClicked: root.selectedSourceId = modelData.id

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 10
                                        color: navRow.current ? root.theme.accentContainer
                                             : hoverArea.containsMouse ? root.theme.glassCardHover
                                             : "transparent"
                                    }
                                    // 选中指示条：与设置页左侧分类导航同一套语言。
                                    Rectangle {
                                        visible: navRow.current
                                        width: 3; height: parent.height - 18; radius: 1.5
                                        x: 0; anchors.verticalCenter: parent.verticalCenter
                                        color: root.theme.accent
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 8
                                        spacing: 8

                                        Rectangle {
                                            Layout.preferredWidth: 8
                                            Layout.preferredHeight: 8
                                            radius: 4
                                            color: root.theme.connectionColor(navRow.modelData.statusTag)
                                            Layout.alignment: Qt.AlignTop
                                            Layout.topMargin: 8
                                        }
                                        Column {
                                            id: navLabels
                                            Layout.fillWidth: true
                                            Layout.topMargin: 7
                                            Layout.bottomMargin: 7
                                            spacing: 2
                                            Text {
                                                width: parent.width
                                                text: navRow.modelData.name
                                                color: navRow.modelData.enabled
                                                       ? root.theme.textPrimary : root.theme.outline
                                                font.pixelSize: 13
                                                font.weight: navRow.current ? Font.Medium : Font.Normal
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                width: parent.width
                                                text: root.sourceStateText(navRow.modelData)
                                                color: root.theme.outline
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                            // 延迟：只有真测到才占这一行（未测出/已关闭时整行消失）。
                                            Text {
                                                objectName: "sourceNavLatency-" + navRow.modelData.id
                                                width: parent.width
                                                visible: navRow.latencyText.length > 0
                                                text: navRow.latencyText
                                                color: root.theme.outline
                                                opacity: 0.85
                                                font.pixelSize: 10
                                                font.family: root.theme.numberFamily
                                                elide: Text.ElideRight
                                            }
                                        }
                                        AppIcon {
                                            visible: navRow.current
                                            name: "chevron-right"
                                            color: root.theme.accent
                                            size: 14
                                            Layout.alignment: Qt.AlignVCenter
                                        }
                                    }

                                    MouseArea {
                                        id: hoverArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: parent.clicked()
                                    }
                                }
                            }
                        }
                    }
                }

                // 竖分隔线：两列在同一张卡里，靠它而不是靠「两张卡的描边」划出边界。
                Rectangle {
                    id: divider
                    objectName: "sourceSplitDivider"
                    visible: root.splitLayout
                    width: 1
                    height: parent.height
                    color: root.theme.glassBorder
                }

                // ── 右列：选中源的全部设置 ───────────────────────────────────
                Item {
                    id: detailHost
                    objectName: "sourceDetailCard"
                    // splitLayout 时宽度 = 卡内宽 - 左列 - 分隔线；窄屏独占整行。
                    width: root.splitLayout
                        ? splitRow.width - navCard.width - divider.width
                        : splitRow.width
                    height: parent.height

                    SourceDetailCard {
                        id: detailCard
                        anchors.fill: parent
                        // 与分隔线之间留 20：文字不贴线。窄屏没有分隔线，不需要这份偏移。
                        anchors.leftMargin: root.splitLayout ? 20 : 0
                        source: root.selectedSource
                        theme: root.theme
                        // 延迟文案只在这里生成一次，左列行与右列状态行共用同一份实现。
                        latencyFormatter: root.sourceLatencyText
                    }
                }
            }
        }
    }

    // 测站显示源：只画地图、不产事件，故与上面的事件源分开管理。
    SettingsSection {
        width: parent.width; theme: root.theme; title: "测站显示源"; iconName: "activity"
        Text {
            width: parent.width
            text: "只在地图上显示台站实时强度，不产生地震事件、不参与预警与去重。"
            wrapMode: Text.Wrap; lineHeight: 1.4
            color: root.theme.outline; font.pixelSize: 12
        }
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
}
