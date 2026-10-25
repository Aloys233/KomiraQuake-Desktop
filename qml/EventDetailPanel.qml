import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 右侧事件详情：基本信息 / 数据源 / 报数（默认折叠）/ 周边区县烈度。
// 由 EventListSidebar 在列表位置切换显示，左上角返回列表。
Item {
    id: root
    objectName: "eventDetailPanel"
    property Theme theme: Theme {}
    property var detail: ({})
    signal back()

    readonly property var d: detail && detail.id ? detail : ({})
    readonly property var sources: d.sources ? d.sources : []
    readonly property var reports: d.reports ? d.reports : []
    readonly property var districts: d.districts ? d.districts : []
    /// 报数区的**折叠态摘要**：用户不展开也要能看到「这一报是自动测定还是正式测定」。
    /// 此前测定状态只印在展开后的每一行里，折叠时标题只剩「N 报」——同一场地震的自动
    /// 测定与正式测定在列表里毫无区别（用户反馈「自动，你在报数那没有标注」）。
    ///
    /// 口径与行内文案同源（都读 C++ 成型的 `determinationText` / `numText`），不在 QML
    /// 里再判断一次 `reviewed`/`automatic`：两处各写一遍必然有一处忘了加。
    readonly property string reportSummary: {
        const list = reports
        if (!list || list.length === 0) return ""
        const parts = []
        const push = (text) => {
            if (text && text.length && parts.indexOf(text) < 0) parts.push(text)
        }
        for (let i = 0; i < list.length; ++i) {
            const report = list[i]
            if (!report) continue
            push(report.determinationText ? String(report.determinationText) : "")
            // 单条时报种类（情报 / 第 N 报）本身就是摘要；多条时不列，避免一行塞满。
            if (list.length === 1)
                push(report.numText ? String(report.numText) : "")
        }
        // 「共 N 报」只是计数器，标题左边已经有「N 报」，不重复写进摘要。
        return parts.join(" · ")
    }
    property bool reportsExpanded: false
    property string openDetailId: ""
    // 详情载荷会随目录/报次更新反复重建：只在切换到另一场地震时收起报数，
    // 否则正在看报次时来一条新报就会被弹回折叠态。
    onDChanged: {
        if (d.id && d.id !== root.openDetailId) {
            root.openDetailId = d.id
            root.reportsExpanded = false
            flick.contentY = 0
        }
    }
    Keys.onEscapePressed: root.back()
    // 侧栏被收起时详情面板只是隐藏：浮层属于窗口 overlay，不主动关闭会留在屏幕上。
    onVisibleChanged: if (!visible) rawPopup.close()

    readonly property var basicRows: {
        const info = root.d
        const rows = []
        if (info.magnitudeText) rows.push({ label: "震级", value: "M " + info.magnitudeText })
        if (info.depthText) rows.push({ label: "深度", value: info.depthText + " km" })
        if (info.hasDistance) rows.push({ label: "距您", value: info.distanceText + " km" })
        if (info.maxIntensity) rows.push({ label: "最大烈度", value: info.maxIntensity })
        if (info.hasLocalIntensity) rows.push({ label: "本地预估", value: info.localIntensity })
        if (info.latitudeText) rows.push({ label: "纬度", value: info.latitudeText })
        if (info.longitudeText) rows.push({ label: "经度", value: info.longitudeText })
        return rows
    }

    function sourceTimeText(source) {
        const first = source.firstReceivedText ? source.firstReceivedText : "--"
        const last = source.lastReceivedText ? source.lastReceivedText : "--"
        let text = first === last ? ("接收 " + first) : ("首次 " + first + " · 最近 " + last)
        if (source.reportCount > 0) text += " · " + source.reportCount + " 报"
        return text
    }

    function openRaw(source) {
        rawPopup.sourceTag = source.tag ? source.tag : ""
        const raw = source.rawJson ? source.rawJson : ""
        // 原始报文是紧凑 JSON；展示前统一缩进，高亮按格式化后的文本处理。
        try {
            rawPopup.body = raw.length ? JSON.stringify(JSON.parse(raw), null, 2) : ""
        } catch (error) {
            rawPopup.body = raw
        }
        rawPopup.open()
    }

    Column {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            width: parent.width
            height: 36
            spacing: 8
            GlassButton {
                theme: root.theme
                iconName: "arrow-left"
                flat: true
                implicitWidth: 36
                implicitHeight: 32
                Layout.alignment: Qt.AlignVCenter
                accessibleName: "返回事件列表"
                onClicked: root.back()
            }
            // 20px 标题和 11px 的状态共用一条基线：两者都不写 alignment 时按行顶对齐，
            // 而字重/字形的 ascent 不同，实测基线差 4.4px ——「实时告警中」会明显偏高。
            // 这是同一类错误的第四处（另外三处在数据源行、报数标题、报次抬头）。
            Text {
                Layout.alignment: Qt.AlignBaseline
                text: "地震详情"
                color: root.theme.textPrimary
                font.pixelSize: 20
                font.weight: Font.Medium
            }
            Item { Layout.fillWidth: true }
            Text {
                Layout.alignment: Qt.AlignBaseline
                visible: root.d.active === true
                text: "实时告警中"
                color: root.theme.accent
                font.pixelSize: 11
                font.weight: Font.Medium
            }
        }

        Flickable {
            id: flick
            objectName: "eventDetailScroll"
            width: parent.width
            height: parent.height - 48
            contentWidth: width
            contentHeight: detailColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            clip: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Column {
                id: detailColumn
                width: flick.width
                spacing: 12

                // ── 概览 ───────────────────────────────────────────────
                GlassCard {
                    width: parent.width
                    implicitHeight: hero.implicitHeight + 40
                    RowLayout {
                        id: hero
                        x: 20; y: 20
                        width: parent.width - 40
                        spacing: 14
                        IntensityBadge {
                            Layout.alignment: Qt.AlignVCenter
                            badgeSize: 72
                            label: root.d.intensityLabel ? root.d.intensityLabel : "最大烈度"
                            value: root.d.intensity ? root.d.intensity : "--"
                            badgeColor: root.d.intensityColor ? root.d.intensityColor : "#9F9F9F"
                            textColor: root.theme.on(badgeColor)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 5
                            Text {
                                Layout.fillWidth: true
                                text: root.d.location ? root.d.location : "--"
                                color: root.theme.textPrimary
                                font.pixelSize: 18
                                font.weight: Font.Medium
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: (root.d.timeText ? root.d.timeText : "--") + "  UTC+8"
                                color: root.theme.outline
                                font.family: root.theme.numberFamily
                                font.pixelSize: 11
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 4
                                height: Math.max(18, childrenRect.height)
                                Repeater {
                                    model: root.d.sourceTags ? root.d.sourceTags : []
                                    delegate: Rectangle {
                                        required property string modelData
                                        height: 18
                                        width: Math.min(sourceLabel.implicitWidth, 150) + 14
                                        radius: 5
                                        color: root.theme.surfaceContainerHigh
                                        border.width: 1
                                        border.color: root.theme.outlineVariant
                                        Text {
                                            id: sourceLabel
                                            anchors.centerIn: parent
                                            width: parent.width - 14
                                            text: modelData
                                            color: root.theme.outline
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ── 基本信息 ───────────────────────────────────────────
                SettingsSection {
                    width: parent.width
                    theme: root.theme
                    title: "基本信息"
                    iconName: "info"
                    Column {
                        width: parent.width
                        spacing: 9
                        Repeater {
                            model: root.basicRows
                            delegate: RowLayout {
                                required property var modelData
                                width: parent.width
                                spacing: 12
                                Text {
                                    Layout.alignment: Qt.AlignBaseline
                                    text: modelData.label
                                    color: root.theme.outline
                                    font.pixelSize: 12
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    Layout.alignment: Qt.AlignBaseline
                                    text: modelData.value
                                    color: root.theme.textPrimary
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    font.family: root.theme.numberFamily
                                }
                            }
                        }
                    }
                }

                // ── 数据源 ─────────────────────────────────────────────
                SettingsSection {
                    width: parent.width
                    theme: root.theme
                    title: "数据源"
                    iconName: "radio"
                    Column {
                        width: parent.width
                        spacing: 10
                        Text {
                            visible: root.sources.length === 0
                            text: "暂无来源记录"
                            color: root.theme.outline
                            font.pixelSize: 12
                        }
                        Repeater {
                            model: root.sources
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "eventDetailSourceRow"
                                width: parent.width
                                // 高度由内容撑开，而不是写死 52：52 减掉上下各 12 的边距只剩
                                // 28px，左侧「来源名(14) + 3 + 接收时间(15)」需要 32px，溢出的
                                // 4px 被布局器居中后上下各裁 2px，右侧按钮又按 30px 居中 ——
                                // 两边参照系不同，看起来就是「原始 JSON 和左边文字没对齐」
                                // （用户反馈的第一处）。边距、行高都由内容算，改字号不会再塌。
                                height: Math.max(52, sourceRow.implicitHeight + 24)
                                radius: 10
                                color: root.theme.surfaceContainer
                                RowLayout {
                                    id: sourceRow
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 10
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.tag
                                            color: root.theme.textPrimary
                                            font.pixelSize: 13
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: root.sourceTimeText(modelData)
                                            color: root.theme.outline
                                            font.family: root.theme.numberFamily
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                    }
                                    GlassButton {
                                        objectName: "eventDetailSourceRawButton"
                                        visible: modelData.hasRaw === true
                                        theme: root.theme
                                        text: "原始 JSON"
                                        iconName: "eye"
                                        flat: true
                                        implicitHeight: 30
                                        // 显式声明垂直居中，不依赖布局器的默认对齐：
                                        // 行高不够时（见上面的 height 说明）布局器压根没法居中，
                                        // 会把超高的子项钉在行顶，按钮就比左边文字高出一截。
                                        // 写死意图，以后再有人压行高也会立刻在这里看出来。
                                        Layout.alignment: Qt.AlignVCenter
                                        accessibleName: "查看 " + modelData.tag + " 的原始 JSON"
                                        onClicked: root.openRaw(modelData)
                                    }
                                    // 旧库/未归档的来源没有原始报文：说明原因，别放一个永远点不动的灰按钮。
                                    Text {
                                        visible: modelData.hasRaw !== true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: root.d.reportsSynthesized === true
                                              ? "旧数据未保存原文" : "无原始数据"
                                        color: root.theme.outline
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }
                    }
                }

                // ── 报数（默认折叠）────────────────────────────────────
                GlassCard {
                    visible: root.reports.length > 0
                    width: parent.width
                    implicitHeight: reportBody.implicitHeight + 40
                    Column {
                        id: reportBody
                        x: 20; y: 20
                        width: parent.width - 40
                        spacing: 12
                        Item {
                            width: parent.width
                            height: 26
                            RowLayout {
                                anchors.fill: parent
                                spacing: 10
                                AppIcon {
                                    name: "activity"
                                    size: 18
                                    color: root.theme.accent
                                }
                                Text {
                                    Layout.alignment: Qt.AlignBaseline
                                    text: "报数"
                                    color: root.theme.textPrimary
                                    font.pixelSize: 16
                                    font.weight: Font.Medium
                                }
                                Text {
                                    Layout.alignment: Qt.AlignBaseline
                                    text: root.reports.length + " 报"
                                    color: root.theme.outline
                                    font.pixelSize: 11
                                }
                                // 折叠态也要能看出测定状态：不加这一行，用户必须先展开才
                                // 知道这一条是「自动测定」还是「正式测定」（用户反馈的正是这点）。
                                Text {
                                    objectName: "eventDetailReportSummary"
                                    Layout.alignment: Qt.AlignBaseline
                                    visible: root.reportSummary.length > 0
                                    text: root.reportSummary
                                    color: root.theme.accent
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Item { Layout.fillWidth: true }
                                // 「展开 + 箭头」是一个整体，必须当成**一个**布局项交给基线对齐。
                                // 拆成两个布局项时：文字按基线摆，箭头只能按 26px 的整行居中
                                // （Qt.AlignVCenter），而 15px 图标盒与 16px 文字盒各自居中会差
                                // 1.3px，箭头看着偏高（用户反馈的第二处）。装进一个盒子后，
                                // 箭头是相对文字盒居中，中心严格重合，图标/字号怎么改都不会再错。
                                Item {
                                    id: reportToggle
                                    Layout.alignment: Qt.AlignBaseline
                                    implicitWidth: toggleLabel.implicitWidth + 4 + toggleArrow.width
                                    implicitHeight: toggleLabel.height
                                    // 交给 RowLayout 的基线就是文字基线：整簇和「报数」「N 报」
                                    // 共用一条线，右侧不会再飘。
                                    baselineOffset: toggleLabel.baselineOffset
                                    Text {
                                        id: toggleLabel
                                        objectName: "eventDetailReportToggleLabel"
                                        anchors.left: parent.left
                                        text: root.reportsExpanded ? "收起" : "展开"
                                        color: root.theme.outline
                                        font.pixelSize: 11
                                    }
                                    // 用 anchors.verticalCenter 而不是在 Row 里排：定位器会接管
                                    // 子项坐标，把 anchors 顶掉（这正是原来看起来偏的来历）。
                                    AppIcon {
                                        id: toggleArrow
                                        objectName: "eventDetailReportToggleIcon"
                                        anchors.left: toggleLabel.right
                                        anchors.leftMargin: 4
                                        anchors.verticalCenter: toggleLabel.verticalCenter
                                        name: "chevron-down"
                                        size: 15
                                        color: root.theme.outline
                                        rotation: root.reportsExpanded ? 180 : 0
                                        Behavior on rotation {
                                            NumberAnimation { duration: root.theme.motionDuration; easing.type: Easing.OutCubic }
                                        }
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.reportsExpanded = !root.reportsExpanded
                            }
                        }
                        Text {
                            visible: root.d.reportsSynthesized === true
                            width: parent.width
                            text: "旧数据仅保留最后一报，原始报文未保存。"
                            color: root.theme.outline
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                            lineHeight: 1.35
                        }
                        Column {
                            visible: root.reportsExpanded
                            width: parent.width
                            spacing: 8
                            Repeater {
                                model: root.reports
                                delegate: Rectangle {
                                    required property var modelData
                                    objectName: "eventDetailReportRow"
                                    width: parent.width
                                    height: reportColumn.implicitHeight + 20
                                    radius: 9
                                    color: root.theme.surfaceContainer
                                    Column {
                                        id: reportColumn
                                        x: 12; y: 10
                                        width: parent.width - 24
                                        spacing: 6
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                // 必须是 RowLayout + 基线对齐：`Row` 只顶对齐，
                                                // 12px 的「情报」盒高 18、10px 的「正式测定」盒高 15，
                                                // 顶对顶之后两条基线差 2.3px，「正式测定」看起来明显偏上
                                                // （用户反馈的第三处）。同步/测定/状态是同一句话，
                                                // 只有共用一条基线才读得成一句。
                                                RowLayout {
                                                    spacing: 6
                                                    Text {
                                                        Layout.alignment: Qt.AlignBaseline
                                                        text: modelData.numText
                                                        color: root.theme.textPrimary
                                                        font.pixelSize: 12
                                                        font.weight: Font.Medium
                                                    }
                                                    // 情报的测定状态（正式/自动）：同一场地震会先后
                                                    // 出两条情报，没有这个标签会看起来像重复条目。
                                                    Text {
                                                        Layout.alignment: Qt.AlignBaseline
                                                        visible: modelData.determinationText !== undefined
                                                                 && String(modelData.determinationText).length > 0
                                                        text: modelData.determinationText
                                                        color: root.theme.outline
                                                        font.pixelSize: 10
                                                    }
                                                    Text {
                                                        Layout.alignment: Qt.AlignBaseline
                                                        visible: modelData.statusText.length > 0
                                                        text: modelData.statusText
                                                        color: root.theme.accent
                                                        font.pixelSize: 10
                                                    }
                                                }
                                                Text {
                                                    text: modelData.timeText + "  UTC+8"
                                                    color: root.theme.outline
                                                    font.family: root.theme.numberFamily
                                                    font.pixelSize: 10
                                                }
                                            }
                                            Flow {
                                                Layout.preferredWidth: Math.min(170, parent.width * 0.5)
                                                Layout.preferredHeight: Math.max(18, childrenRect.height)
                                                Layout.alignment: Qt.AlignTop
                                                // 从右向左排：第一枚标签贴住卡片右缘，也就是「右上角」。
                                                layoutDirection: Qt.RightToLeft
                                                spacing: 4
                                                Repeater {
                                                    model: modelData.sourceTags
                                                    delegate: Rectangle {
                                                        required property string modelData
                                                        height: 18
                                                        width: Math.min(chipLabel.implicitWidth, 110) + 12
                                                        radius: 5
                                                        color: root.theme.surfaceContainerHigh
                                                        border.width: 1
                                                        border.color: root.theme.outlineVariant
                                                        Text {
                                                            id: chipLabel
                                                            anchors.centerIn: parent
                                                            width: parent.width - 12
                                                            text: modelData
                                                            color: root.theme.outline
                                                            font.pixelSize: 9
                                                            elide: Text.ElideRight
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        Text {
                                            width: parent.width
                                            text: "M " + modelData.magnitudeText + "  ·  深度 "
                                                  + modelData.depthText + " km"
                                                  + (modelData.maxIntensityText.length
                                                     ? "  ·  最大烈度 " + modelData.maxIntensityText : "")
                                            color: root.theme.outline
                                            font.family: root.theme.numberFamily
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ── 周边区县烈度（只显示 ≥1 度）──────────────────────────
                SettingsSection {
                    width: parent.width
                    theme: root.theme
                    title: "周边区县烈度"
                    iconName: "map"
                    Column {
                        width: parent.width
                        spacing: 8
                        Text {
                            visible: root.districts.length === 0
                            text: "周边区县烈度均低于 1 度"
                            color: root.theme.outline
                            font.pixelSize: 12
                        }
                        RowLayout {
                            visible: root.districts.length > 0
                            width: parent.width
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: "区县"
                                color: root.theme.outline
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 30
                                horizontalAlignment: Text.AlignHCenter
                                text: "烈度"
                                color: root.theme.outline
                                font.pixelSize: 11
                            }
                        }
                        Repeater {
                            model: root.districts
                            delegate: RowLayout {
                                required property var modelData
                                width: parent.width
                                spacing: 8
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: root.theme.textPrimary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    width: 30; height: 20; radius: 5
                                    color: modelData.color
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.intensityText
                                        color: root.theme.on(modelData.color)
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                    }
                                }
                            }
                        }
                    }
                }

                Item { width: 1; height: 4 }
            }
        }
    }

    // 原始 JSON：每路数据源各自最近一帧报文，按需打开，避免详情页一次渲染大量文本。
    Popup {
        id: rawPopup
        objectName: "rawJsonPopup"
        property string sourceTag: ""
        property string body: ""
        // 明确挂到窗口 overlay：Popup 默认相对声明它的 Item 定位，在右侧栏里会把
        // 「居中」算成「侧栏中心」，窗口一窄就跑到屏幕外。
        parent: Overlay.overlay
        modal: true
        focus: true
        padding: 0
        width: Math.min(760, (Overlay.overlay ? Overlay.overlay.width : 900) - 64)
        height: Math.min(620, (Overlay.overlay ? Overlay.overlay.height : 800) - 96)
        x: Overlay.overlay ? (Overlay.overlay.width - width) / 2 : 0
        y: Overlay.overlay ? (Overlay.overlay.height - height) / 2 : 0
        background: Rectangle {
            color: root.theme.surfaceContainerLow
            radius: 14
            border.width: 1
            border.color: root.theme.glassBorder
        }
        contentItem: ColumnLayout {
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: "原始 JSON"
                        color: root.theme.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.Medium
                    }
                    Text {
                        Layout.fillWidth: true
                        text: rawPopup.sourceTag
                        color: root.theme.outline
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
                GlassButton {
                    theme: root.theme
                    text: "关闭"
                    flat: true
                    implicitHeight: 32
                    onClicked: rawPopup.close()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: root.theme.glassBorder
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 12
                JsonView {
                    anchors.fill: parent
                    theme: root.theme
                    text: rawPopup.body
                }
            }
        }
    }
}
