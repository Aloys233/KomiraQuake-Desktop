import QtQuick

// 烈度徽章。《NATIVE_PORT_SPEC》 §12。外观对齐 kanameishi 的 .intensity 构成块：
// 顶部标题 + 底部大数字；JMA 修饰值首位数字特大、修饰字（弱/强）较小；罗马数字按长度收缩。
// CJK 字体的行高（ascent/descent）远大于字面，直接堆叠文本框会留出巨大间隙。这里改按
// FontMetrics 的墨迹边界排版：标题墨迹底与数字墨迹顶保持固定间隙，整块墨迹在徽章内垂直居中。
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
    readonly property real valueSize: {
        if (modifierValue) return badgeSize * 0.56;
        const n = value.length;
        if (n <= 2) return badgeSize * 0.55;
        if (n === 3) return badgeSize * 0.50;
        return badgeSize * 0.46;
    }
    readonly property real modifierSize: valueSize * 0.72
    readonly property real labelSize: Math.max(9, Math.min(14, badgeSize * 0.17))
    // 标题墨迹底与数字墨迹顶之间的目标间隙。
    readonly property real inkGap: badgeSize * 0.06

    FontMetrics { id: labelFm; font: labelText.font }
    FontMetrics { id: valueFm; font: valueText.font }
    FontMetrics { id: modifierFm; font: modifierChar.font }

    // 各段文字的墨迹（相对基线）。
    readonly property rect labelInk: labelFm.tightBoundingRect(root.label)
    readonly property rect valueInk: valueFm.tightBoundingRect(root.value)
    readonly property rect firstInk: valueFm.tightBoundingRect(root.value.charAt(0))
    readonly property rect modifierInk: modifierFm.tightBoundingRect(root.value.substring(1))
    // 修饰行内两字顶对齐但基线不同，分别换算成相对行顶的墨迹位置。
    readonly property real firstInkTop: firstChar.baselineOffset + firstInk.y
    readonly property real firstInkBottom: firstChar.baselineOffset + firstInk.y + firstInk.height
    readonly property real modifierInkTop: modifierChar.baselineOffset + modifierInk.y
    readonly property real modifierInkBottom: modifierChar.baselineOffset + modifierInk.y + modifierInk.height
    readonly property real valueInkHeight: modifierValue
        ? Math.max(firstInkBottom, modifierInkBottom) - Math.min(firstInkTop, modifierInkTop)
        : valueInk.height

    readonly property real blockHeight: labelInk.height + inkGap + valueInkHeight
    readonly property real labelInkTop: (badgeSize - blockHeight) / 2
    readonly property real valueInkTop: labelInkTop + labelInk.height + inkGap

    width: badgeSize
    height: badgeSize
    radius: Math.round(badgeSize * 0.18)
    color: badgeColor

    Text {
        id: labelText
        visible: root.showLabel
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.labelInkTop - root.labelInk.y - baselineOffset
        text: root.label
        color: root.textColor
        font.pixelSize: root.labelSize
        font.weight: Font.Normal
    }

    // 单字符或罗马数字。
    Text {
        id: valueText
        visible: !root.modifierValue
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.valueInkTop - root.valueInk.y - baselineOffset
        text: root.value
        color: root.textColor
        font.pixelSize: root.valueSize
        font.weight: Font.DemiBold
        font.letterSpacing: root.value.length >= 3 ? -root.valueSize * 0.08 : 0
    }

    // JMA 修饰值：首位数字特大，修饰字较小并顶对齐。
    Row {
        id: modifierRow
        visible: root.modifierValue
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.valueInkTop - Math.min(root.firstInkTop, root.modifierInkTop)
        spacing: 0

        Text {
            id: firstChar
            text: root.value.charAt(0)
            color: root.textColor
            font.pixelSize: root.valueSize
            font.weight: Font.DemiBold
        }
        Text {
            id: modifierChar
            text: root.value.substring(1)
            color: root.textColor
            font.pixelSize: root.modifierSize
            font.weight: Font.DemiBold
        }
    }
}
