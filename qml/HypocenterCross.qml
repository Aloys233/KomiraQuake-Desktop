import QtQuick

// 震中「X 十字」：参考 kanameishi `eqlistCross.svg`，10 px 米黄打底 + 6 px 红描边。
//
// 官方震中与本地推算的**推定震中**共用这一份画法。用户明确要求「震源推算的震中换成和
// 其他震中一样的 x」—— 两处各写一份 Canvas 迟早会漂（颜色、线宽、臂长任何一处改动都
// 会让两边长得不一样），所以抽成组件，谁也别再复制第二份。
//
// 形状一致之后，区分官方/推定不再靠图形，而靠文字：推定震中旁边有常驻标签（首行写
// 「区域 · 精度 · 第 N 报」），官方震中只有悬停时的信息块。
//
// 重画时机：Canvas.onPaint 只在尺寸变化与 `requestPaint()` 时触发。颜色是 QML 属性，
// 值变了**不会**自动重画 —— 切深浅主题时十字会停在旧颜色上，所以颜色各挂一个处理器。
Canvas {
    id: root
    property Theme theme: Theme {}
    /// 打底描边色（米黄）与主描边色（红）。
    property color haloColor: theme.hypocenterHalo
    property color crossColor: theme.hypocenterCross

    // 40 px 见方、臂端内缩 5 px：与参考 SVG 的 10/6 px 线宽同比例。
    implicitWidth: 40
    implicitHeight: 40
    width: implicitWidth
    height: implicitHeight
    readonly property real armInset: 5
    readonly property real haloWidth: 10
    readonly property real crossWidth: 6

    onHaloColorChanged: requestPaint()
    onCrossColorChanged: requestPaint()
    onArmInsetChanged: requestPaint()
    onHaloWidthChanged: requestPaint()
    onCrossWidthChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        ctx.lineCap = "round";
        const stroke = (color, lineWidth) => {
            ctx.strokeStyle = color;
            ctx.lineWidth = lineWidth;
            ctx.beginPath();
            ctx.moveTo(root.armInset, root.armInset);
            ctx.lineTo(root.width - root.armInset, root.height - root.armInset);
            ctx.moveTo(root.width - root.armInset, root.armInset);
            ctx.lineTo(root.armInset, root.height - root.armInset);
            ctx.stroke();
        };
        // 先宽后窄：米黄打底从红线两侧各露 2 px，测站方标底色跨度大时也不会糊掉。
        stroke(root.haloColor, root.haloWidth);
        stroke(root.crossColor, root.crossWidth);
    }
}
