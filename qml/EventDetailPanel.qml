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
                accessibleName: "返回事件列表"
                onClicked: root.back()
            }
            Text {
                text: "地震详情"
                color: root.theme.textPrimary
                font.pixelSize: 20
                font.weight: Font.Medium
            }
            Item { Layout.fillWidth: true }
            Text {
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
                                width: parent.width
                                height: 52
                                radius: 10
                                color: root.theme.surfaceContainer
                                RowLayout {
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
                                        visible: modelData.hasRaw === true
                                        theme: root.theme
                                        text: "原始 JSON"
                                        iconName: "eye"
                                        flat: true
                                        implicitHeight: 30
                                        accessibleName: "查看 " + modelData.tag + " 的原始 JSON"
                                        onClicked: root.openRaw(modelData)
                                    }
                                    // 旧库/未归档的来源没有原始报文：说明原因，别放一个永远点不动的灰按钮。
                                    Text {
                                        visible: modelData.hasRaw !== true
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
                                Item { Layout.fillWidth: true }
                                Text {
                                    Layout.alignment: Qt.AlignBaseline
                                    text: root.reportsExpanded ? "收起" : "展开"
                                    color: root.theme.outline
                                    font.pixelSize: 11
                                }
                                AppIcon {
                                    Layout.alignment: Qt.AlignVCenter
                                    name: "chevron-down"
                                    size: 15
                                    color: root.theme.outline
                                    rotation: root.reportsExpanded ? 180 : 0
                                    Behavior on rotation {
                                        NumberAnimation { duration: root.theme.motionDuration; easing.type: Easing.OutCubic }
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
                                                Row {
                                                    spacing: 6
                                                    Text {
                                                        text: modelData.numText
                                                        color: root.theme.textPrimary
                                                        font.pixelSize: 12
                                                        font.weight: Font.Medium
                                                    }
                                                    // 情报的测定状态（正式/自动）：同一场地震会先后
                                                    // 出两条情报，没有这个标签会看起来像重复条目。
                                                    Text {
                                                        visible: modelData.determinationText !== undefined
                                                                 && String(modelData.determinationText).length > 0
                                                        text: modelData.determinationText
                                                        color: root.theme.outline
                                                        font.pixelSize: 10
                                                    }
                                                    Text {
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
