import QtQuick
import QtQuick.Layouts

// 本地震源推算（Beta）卡片 —— **外观与普通地震 HUD（`HudCard.qml`）同构**：
// 左侧烈度色块，右侧「推算来源 / 震中地名 / 发震时刻 / 震级·深度」，卡底同款 n/N 分页。
//
// 与普通 HUD 只差两处：
//   ① 顶行右侧写「第 N 报」（解变了才加一，判据见 `SourceEstimationService::sameReport`）；
//   ② 右下角原本的数据源标注（`Wolfx·CENC`）换成「精度 S/A/B/C/D」——等级即质量，
//      来自 §17.2 的站数 + 质量分双门槛，单站粗解会被钉在 D。
//
// 地名不查行政区划，走参考实现 kanameishi 的 `getFEName`：一张 1° × 1° 的震源区网格
// （`core/region_names.*`，资产 `assets/region_names.bin`）。USGS / FSSN 这类没有中文
// 地名的报文在参考产品里正是用它补震中名，本地震源推算同样只有坐标，用同一张表最一致。
//
// **刻意不再有**（按用户要求全部去掉）：收集中「N / M 站已触发」、置信度条与百分比、
// 负证据/更新时间/合并半径脚注、单站粗解标签、区域分隔线与「另有 N 处触发区」。
// 四个推算区域各自出解，靠卡底的分页切换，一区一页，互不覆盖。
GlassCard {
    id: root
    objectName: "sourceEstimateCard"
    /// app.sourceEstimateInfo（见 SourceEstimationService::publish 的字段说明）。
    /// 逐区明细在 info.regions（顺序固定为 中国 / 日本 / 韩国 / 台湾）。
    property var info: ({})

    // 命名避开 Item.enabled（内置属性）：重声明会触发 propertyCache 覆盖警告。
    readonly property bool estimationEnabled: info.enabled === true
    /// 有解的区域行（没出解的区不占位，也不翻页）。
    readonly property var rowsModel: {
        const list = info.regions;
        if (!list || !list.length) return [];
        const out = [];
        for (let i = 0; i < list.length; ++i) {
            if (list[i] && list[i].visible === true) out.push(list[i]);
        }
        return out;
    }
    /// 卡片是否出现：至少一区已给出解。收集中的区不显示任何提示，因此也不占位。
    readonly property bool hasSignal: estimationEnabled && rowsModel.length > 0
    /// 当前页（0 基）。只有一区时恒为 0。
    property int pageIndex: 0
    /// 当前页那一区的 id（用于「刷新后别把用户翻到的页顶回去」）。
    readonly property string pageId: current && current.id !== undefined
        ? String(current.id) : ""
    /// 上一次稳定显示过的区。载荷每秒刷新会让 `rowsModel` 变成新数组，
    /// 但**区域集合没变时不能重排分页**，否则用户刚翻到日本区，下一秒就被顶回第 1 页。
    property string lastPageId: ""
    /// 区域集合签名：只有它变了（有新解出现 / 某区解消失）才重新选页。
    readonly property string rowsKey: {
        const parts = [];
        for (let i = 0; i < rowsModel.length; ++i) parts.push(String(rowsModel[i].id));
        return parts.join(",");
    }
    /// 最强的一区（服务顶层 `id` 就是置信度最高的一区），首次出现时默认停在它那一页。
    readonly property string primaryId: info.id !== undefined ? String(info.id) : ""
    /// 用户是否手动翻过页。翻过之后就不许「新解出现」把页面抢走。
    property bool userPaged: false
    readonly property var current: rowsModel.length
        ? rowsModel[Math.max(0, Math.min(pageIndex, rowsModel.length - 1))] : null
    readonly property bool hasPager: rowsModel.length > 1

    function indexOfRegion(id) {
        for (let i = 0; i < rowsModel.length; ++i) {
            if (String(rowsModel[i].id) === id) return i
        }
        return -1
    }
    onPageIdChanged: if (pageId.length > 0) lastPageId = pageId
    // 区域集合变化（不是每秒的数值刷新）时才重新选页：
    //   · 用户没翻过页 → 停在最强的一区（多区逐步出解时跟着「最可信的那个」走）；
    //   · 用户翻过页 → 原来那一区还在就留在原页；它消失了才回到最强的一区。
    // 关键是**只在集合变化时**重选：每秒 tick 只是数值刷新，不能把页顶回去。
    onRowsKeyChanged: {
        if (rowsModel.length === 0) {
            pageIndex = 0
            userPaged = false
            return
        }
        let index = -1
        if (userPaged) index = indexOfRegion(lastPageId)
        if (index < 0) index = indexOfRegion(primaryId)
        if (index < 0) index = Math.min(pageIndex, rowsModel.length - 1)
        pageIndex = index
    }

    implicitWidth: 352
    implicitHeight: mainRow.implicitHeight + 28 + (hasPager ? pager.implicitHeight + 6 : 0)
    // 与主 HUD 同一套玻璃材质：它就在 HUD 正下方，材质不能分叉。
    color: backdropSource ? (blurActive ? theme.backdropTint : theme.backdropFallback) : theme.glassCard
    border.color: theme.glassBorder
    radius: 16
    visible: hasSignal

    /// 推定震中处的烈度色块颜色：按该区的强度制式取色阶（与测站方标/卡片同源）。
    function familyColor(family, raw) {
        if (family === "jma") return theme.niedStationColor(raw)
        if (family === "kma") return theme.kmaStationColor(raw)
        return theme.intensityColor(raw)
    }

    RowLayout {
        id: mainRow
        x: 14; y: 14
        width: parent.width - 28
        spacing: 12

        IntensityBadge {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 72
            Layout.preferredHeight: 72
            badgeSize: 72
            // 与普通 HUD 的「预估烈度 / 最大烈度」同位：这里给推定震中处的烈度。
            label: "推定烈度"
            standard: root.current && root.current.familyLabel !== undefined
                ? String(root.current.familyLabel) : ""
            source: root.current && root.current.label !== undefined
                ? String(root.current.label) : ""
            value: root.current && root.current.epicentralText !== undefined
                ? String(root.current.epicentralText) : "--"
            badgeColor: root.current
                ? root.familyColor(root.current.family,
                                   root.current.epicentralIntensity === undefined
                                       ? 0 : root.current.epicentralIntensity)
                : root.theme.surfaceContainer
            textColor: root.theme.on(badgeColor)
        }

        Column {
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                width: parent.width
                spacing: 6
                AppIcon {
                    name: "locate-fixed"; size: 14
                    color: root.theme.accent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    // 普通 HUD 这一行是报文名（如「四川地震预警」）；推算没有报文，
                    // 写产品名 + 推算区域，翻页时才知道当前看的是哪一片台网的结论。
                    text: root.current
                        ? "本地震源推算 · " + (root.current.label === undefined
                                              ? "" : String(root.current.label))
                        : ""
                    color: root.theme.accent
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "sourceEstimateReportNum"
                    // 与普通 HUD 的「第 N 报」同一位置、同一字号。
                    text: root.current && root.current.reportNum !== undefined
                        ? "第 " + root.current.reportNum + " 报" : ""
                    color: root.theme.outline; font.pixelSize: 11
                }
            }

            Text {
                objectName: "sourceEstimateLocation"
                width: parent.width
                // 震源区名（「中国四川附近」），来自参考实现的 1° 网格；表缺失时是坐标文本。
                text: root.current && root.current.location !== undefined
                    ? String(root.current.location) : ""
                color: root.theme.textPrimary
                font.pixelSize: 19
                font.weight: Font.Medium
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            // 发震时刻独占一行：不参与任何伸缩，始终完整显示。没有反推出发震时刻时整行隐藏。
            Text {
                objectName: "sourceEstimateOrigin"
                width: parent.width
                visible: text.length > 0
                text: root.current && root.current.originText !== undefined
                      && String(root.current.originText).length > 0
                    ? String(root.current.originText) + "  UTC+8" : ""
                color: root.theme.outline
                font.family: root.theme.numberFamily
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            RowLayout {
                width: parent.width
                spacing: 8
                Text {
                    objectName: "sourceEstimateMagnitude"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBaseline
                    text: root.current
                        ? "M " + (root.current.magnitudeText === undefined
                                  ? "--" : root.current.magnitudeText)
                          + " · 深度 " + (root.current.depthText === undefined
                                          ? "--" : root.current.depthText) + " km"
                        : ""
                    color: root.theme.outline
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                // 普通 HUD 这里是数据源标注（`Wolfx·CENC`）；推算版换成精度等级。
                Text {
                    objectName: "sourceEstimatePrecision"
                    Layout.alignment: Qt.AlignBaseline
                    text: root.current && root.current.grade !== undefined
                        ? "精度 " + String(root.current.grade) : ""
                    color: root.theme.outline
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
        }
    }

    // 多区域分页：与普通 HUD 的分页器同款（紧凑一行，避免占用过多纵向空间）。
    RowLayout {
        id: pager
        objectName: "sourceEstimatePager"
        visible: root.hasPager
        x: 14; y: 14 + mainRow.implicitHeight + 6
        width: parent.width - 28
        spacing: 4
        GlassButton {
            objectName: "sourceEstimatePrevPage"
            theme: root.theme; flat: true; iconName: "chevron-left"; customRadius: 8
            implicitWidth: 24; implicitHeight: 24
            accessibleName: "上一个推算区"
            onClicked: {
                root.userPaged = true
                root.pageIndex = (root.pageIndex - 1 + root.rowsModel.length)
                                 % root.rowsModel.length
            }
        }
        Text {
            objectName: "sourceEstimatePagerText"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: (Math.min(root.pageIndex, root.rowsModel.length - 1) + 1) + " / "
                  + root.rowsModel.length
            color: root.theme.outline
            font.family: root.theme.numberFamily
            font.pixelSize: 11
        }
        GlassButton {
            objectName: "sourceEstimateNextPage"
            theme: root.theme; flat: true; iconName: "chevron-right"; customRadius: 8
            implicitWidth: 24; implicitHeight: 24
            accessibleName: "下一个推算区"
            onClicked: {
                root.userPaged = true
                root.pageIndex = (root.pageIndex + 1) % root.rowsModel.length
            }
        }
    }
}
