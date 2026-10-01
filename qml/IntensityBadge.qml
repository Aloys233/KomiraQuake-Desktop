import QtQuick

// 烈度徽章。《NATIVE_PORT_SPEC》 §12。外观对齐 kanameishi 的 .intensity 构成块：
// 顶部标题 + 底部大数字；JMA 修饰值首位数字特大、修饰字（弱/强）较小；罗马数字按长度收缩。
// 数字区域由锚点显式限定高度并贴底对齐，避免 CJK 字体行距把标题与数字挤在一起。
Rectangle {
    id: root
    property color badgeColor: "#9F9F9F"
    property color textColor: "#FFFFFF"
    property string value: "--"
    property real badgeSize: 56
    property bool showLabel: true
    property string label: "预估烈度"
    property string standard: ""
    property string source: ""
    Accessible.name: [label, value, standard, source].filter(v => v.length > 0).join(" · ")

    // JMA 细分震度（如 5弱 / 5强 / 5- / 5+）走首位放大的排版。
    readonly property bool modifierValue: /^[0-9]+[弱强+\-]$/.test(value)
    readonly property real pad: badgeSize * 0.08
    readonly property real valueSize: {
        if (modifierValue) return badgeSize * 0.60;
        const n = value.length;
        if (n <= 2) return badgeSize * 0.60;
        if (n === 3) return badgeSize * 0.52;
        return badgeSize * 0.46;
    }
    readonly property real labelSize: Math.max(9, Math.min(14, badgeSize * 0.17))

    width: badgeSize
    height: badgeSize
    radius: Math.round(badgeSize * 0.18)
    color: badgeColor

    Text {
        id: titleText
        visible: root.showLabel
        anchors.top: parent.top
        anchors.topMargin: root.pad
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.label
        color: root.textColor
        font.pixelSize: root.labelSize
        font.weight: Font.Normal
        lineHeight: root.labelSize
        lineHeightMode: Text.FixedHeight
    }

    // 数字区域：标题之下到底部，数字贴底居中。
    Item {
        id: valueArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.showLabel ? titleText.bottom : parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: root.pad * 0.3
        anchors.bottomMargin: root.pad

        // 单字符或罗马数字：整体贴底。
        Text {
            visible: !root.modifierValue
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            height: parent.height
            verticalAlignment: Text.AlignBottom
            horizontalAlignment: Text.AlignHCenter
            text: root.value
            color: root.textColor
            font.pixelSize: root.valueSize
            font.weight: Font.DemiBold
            font.letterSpacing: root.value.length >= 3 ? -root.valueSize * 0.08 : 0
            lineHeight: root.valueSize
            lineHeightMode: Text.FixedHeight
        }

        // JMA 修饰值：首位数字特大，修饰字较小并顶对齐。
        Row {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 0
            visible: root.modifierValue

            Text {
                id: firstChar
                text: root.value.charAt(0)
                color: root.textColor
                font.pixelSize: root.valueSize
                font.weight: Font.DemiBold
                lineHeight: root.valueSize
                lineHeightMode: Text.FixedHeight
            }
            Text {
                id: modifierChar
                text: root.value.substring(1)
                color: root.textColor
                font.pixelSize: root.valueSize * 0.72
                font.weight: Font.DemiBold
                lineHeight: root.valueSize * 0.72
                lineHeightMode: Text.FixedHeight
            }
        }
    }
}
