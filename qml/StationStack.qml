import QtQuick

// 地图右上角「测站源当前最大值」的堆叠容器。《NATIVE_PORT_SPEC》 §16。
//
// 形态：收起时各路源叠成一摞 —— **前排**卡片完整可见，其余卡片**沿左下方向**各错开
// 几像素（像一叠纸），只露出一圈边；指针放上去约 80 ms 后向左展开成一排
// （每个源都完整可读），离开 320 ms 后收回。展开/收回只驱动一条 `progress` 0→1 动画：
// 卡片位置、容器宽高、可悬停区域全部从它推出（容器右缘固定，宽度增长只向左延伸，
// 前排卡因此不会跟着漂）。`reduceMotion` 打开时时长归零（与 Theme.motionDuration 同语义）。
//
// 收起态**最多露 3 张**（`peekCount`）：一摞纸要「看得出是叠着的」就够了，5 张全错开
// 只会让右上角又宽又高、盖掉更多地图。第 4 张起在收起态透明度归零，展开时再淡入。
//
// 收起态只看得见前排，所以排序只决定「谁站前排」，按三级排：
//   ① **读数变过的优先**：20 s 内数值变化过的源排在前面 —— 地震刚到、某一路刚起跳时，
//      它最值得看，哪怕读数还不高；
//   ② 同级内**读数高的优先**：读数按「中国烈度等效值」折算 —— JMA / MMI / CSIS 三个
//      制式的原始值不能直接比大小，系数与 `core/source_estimator.cpp` 的 `csisEquivalent`
//      同源（JMA: (raw - 0.2) / 0.75、KMA: raw - 1.0、CSIS 原值）。**静稳不算「有读数」**：
//      等效值 < 0.5（NIED 的 0 震度 ≈ -0.27、KMA 的 MMI 1 = 0、CSIS 0）一律视作「没有数值」，
//      否则安静时某一路会靠自身基线（例如 KMA 恒报 MMI 1）长期霸占前排；
//   ③ 仍然并列、或谁都没有数值时，按数据源注册顺序：NIED / S-Net / KMA / PancakesSeis /
//      WolfxSeisJS —— 于是「都没有变化、也都没有读数」时前排自然就是 NIED。
// 换前排带 0.1 度量化 + 0.3 度迟滞：读数在阈值附近抖动时不会每 100 ms 换一张卡。
Item {
    id: root
    objectName: "stationStack"
    property Theme theme: Theme {}
    property Item backdropSource: null
    /// 背景是否逐帧重采样（透传给玻璃卡，见 BackdropBlur.live）。
    property bool backdropLive: true
    /// 逐源卡片数据（`app.stationSources`）：
    /// `[{ id, name, caption, hasData, raw, text, standard, stationCount, frameTime }]`。
    property var sources: []
    /// 「读数正在变」的判定窗口（ms）。可写以便单测把它压短，不必真等 20 秒。
    property int hotWindowMs: 20000
    /// 数值变化的最小判定幅度（原生单位）。小于它的抖动不算「变过」：
    /// NIED 连续震度在静稳时会在 0 附近持续抖 ±0.01，逐位比较会让它永远「热」。
    readonly property real changeEpsilon: 0.05

    // ---- 展开 / 收回 -------------------------------------------------------
    /// 指针是否在堆叠范围内（HoverHandler 原始态）。/ 展开态（带去抖）。
    property bool pointerInside: false
    property bool expanded: false
    /// 展开/收回动画时长（ms）。`durationOverride >= 0` 时以它为准 —— 仅供取证/回归
    /// 把动画拉慢到足以逐帧抓图（GUI 测试里 `grabWindow` 要十几毫秒，220 ms 的动画
    /// 常常一抓就已经落定）。生产代码不写这个属性，`-1` 即跟随 `reduceMotion`。
    property int durationOverride: -1
    readonly property int expandDuration: durationOverride >= 0 ? durationOverride
                                         : (theme.reduceMotion ? 0 : 220)
    /// 是否正处在展开/收回的过渡中（含落定后的短暂余辉，见 `settleDelay`）。
    ///
    /// 这个状态唯一的用途是**临时打开背景模糊的逐帧重采样**（见下面的 cardBackdropLive）。
    /// 卡片的玻璃底（`BackdropBlur`）默认 `live: false`：静止时只每 500 ms 补一帧，
    /// 省得场景图永不空闲。但卡片在动画里每帧都在移动、容器宽度也在变，非 live 的
    /// 采样纹理不会跟着更新 —— 表现为「模糊跟着卡一起被拖走、然后某一帧突然跳回去」。
    /// 所以过渡期间必须 live（约 220 ms × 5 张卡，成本可接受），落定 180 ms 后再回到
    /// 低频补帧：`live` 一关的那一帧纹理还是上一帧的，立刻关会在终点闪一下。
    property bool animating: false
    readonly property bool cardBackdropLive: backdropLive || animating
    Timer {
        id: settleDelay
        interval: 180
        onTriggered: root.animating = false
    }
    // 展开/收回一被要求就立刻开 live：等 `progress` 动了才开的话，首帧卡片已经挪了
    // 一点、纹理却还是旧的 —— 这一帧就是「跳一下」的来源。
    onExpandedChanged: {
        settleDelay.stop();
        root.animating = true;
        // 没有真正的动画时（`reduceMotion`、或者根本没有卡片）progress 不会变，
        // 这里必须自己安排收尾，否则 animating 会永远停在 true（5 张卡一直逐帧重采样）。
        if (root.progress <= 0.0001 || root.progress >= 0.9999) settleDelay.restart();
    }
    onProgressChanged: {
        // 终点（0 或 1）都要等一拍再关：真实动画的最后一帧可能仍是 0.999…；
        // 而且关掉 live 的那一帧纹理仍是旧的，立刻关会在终点闪一下。
        if (root.progress <= 0.0001 || root.progress >= 0.9999) settleDelay.restart();
        else { settleDelay.stop(); root.animating = true; }
        // 收回动画彻底结束才按最新数据重排（见文件里「展开期间不重排」的说明）。
        if (!root.expanded && root.progress <= 0.0001) root.applyOrder();
    }

    function setPointerInside(inside) {
        if (root.pointerInside === inside) return;
        root.pointerInside = inside;
        if (inside && root.visible && root.count > 0) {
            collapseDelay.stop();
            expandDelay.restart();
        } else {
            expandDelay.stop();
            collapseDelay.restart();
        }
    }
    function hideStack() {
        pointerInside = false;
        expanded = false;
        expandDelay.stop();
        collapseDelay.stop();
    }
    onVisibleChanged: if (!visible) root.hideStack()
    // 展开期间不动卡片顺序：指针正压在某一列上时把卡片换位，会让指针落到别的卡上,
    // 观感像是「刚展开就错位」。重排推迟到**收回动画结束**（progress 回到 0）才做 ——
    // 在 onExpandedChanged 里做会让卡片在收回途中就换槽位，看起来是横着飞过去。
    // （实现合并在上面的 onProgressChanged 里：同一个信号只能有一个处理器。）

    Timer {
        id: expandDelay
        // 指针扫过时不展开：停稳 80 ms 才算「放上去」。
        interval: 80
        onTriggered: if (root.pointerInside && root.count > 0) root.expanded = true
    }
    Timer {
        id: collapseDelay
        // 收起比展开慢一拍：指针掠过卡片缝隙、或落到展开区边缘时不会立刻塌回去。
        interval: 320
        onTriggered: if (!root.pointerInside) root.expanded = false
    }
    // 「热」是按时间窗口算的，但没有新帧时没人重算排序：每秒兜一次，
    // 让变热的源浮上来、冷却的源沉回去（5 个元素的排序，成本可忽略）。
    Timer {
        interval: 1000
        repeat: true
        running: root.count > 0 && root.visible
        onTriggered: root.applyOrder()
    }

    // ---- 尺寸与展开进度 ---------------------------------------------------
    readonly property int count: root.sources ? root.sources.length : 0
    /// 单张卡尺寸：由 delegate 的 implicit 尺寸回填（StationIntensityCard 固定宽 90，
    /// 高随字体/缩放变化），首帧用默认值兜底。
    property real cardWidth: 90
    property real cardHeight: 74
    /// 收起时相邻两张卡错开的距离（**左下方向**的一小步）——露出的这一圈纸边就是
    /// 「后面还有几张」的暗示。两个方向都取小值：只露 3 张，所以往下总共只长 2 个 peekY。
    readonly property real peekX: 9
    readonly property real peekY: 9
    /// 收起态最多露出几张（前排 + 2 张纸条边）。其余卡在收起态透明度 0。
    readonly property int peekCount: 3
    /// 展开时卡片之间的间距。
    readonly property real gap: 8
    /// 展开进度 0 → 1。**所有几何都从它算**，这样展开/收回只需要一条动画：
    /// 位置、容器宽高、可悬停区域全部同步过渡，不会出现「容器先跳、卡片后动」
    /// 或前排卡被容器的宽度变化甩出去（容器的右缘是固定的，宽度增长只向左延伸）。
    property real progress: root.expanded ? 1 : 0
    Behavior on progress {
        NumberAnimation { duration: root.expandDuration; easing.type: Easing.OutCubic }
    }
    /// 收起态实际露出的张数（最多 `peekCount`；源少于 3 路时就是全部）。
    readonly property int collapsedCount: Math.min(count, peekCount)
    /// 展开后的整排宽度 / 整列高度（progress == 1 的目标值）。
    readonly property real expandedRowWidth:
        cardWidth * Math.max(1, count) + gap * Math.max(0, count - 1)
    readonly property real expandedColumnHeight:
        cardHeight * Math.max(1, count) + gap * Math.max(0, count - 1)
    /// 窗口太窄、向左展开会顶出地图左缘时退回竖向展开（宁可下压也不出界）。
    readonly property bool expandVertical:
        expandedRowWidth + 32 > (root.parent ? root.parent.width : 0)
    /// 槽位间距。收起态沿**左下**各错开 `peekX` / `peekY`；展开态横向排成一排
    /// （`y` 同时收回 0），窗口太窄退回竖向展开时把 `x` 收回 0、`y` 拉开成卡高 + gap。
    readonly property real stepX: expandVertical
        ? peekX * (1 - progress)
        : peekX + progress * (cardWidth + gap - peekX)
    readonly property real stepY: expandVertical
        ? peekY + progress * (cardHeight + gap - peekY)
        : peekY * (1 - progress)
    /// 最远槽位的等效下标：收起态是 `peekCount - 1`（只铺 3 张纸条边），展开态是
    /// `count - 1`。**随 progress 连续变化**，不是到点才跳档 —— 卡 4/5 因此能一边
    /// 被淡出一边滑回第 3 张后面，不会在收回的第一帧就被硬塞进去。
    readonly property real maxSlotOffset: {
        const base = Math.max(0, Math.min(root.peekCount, root.count) - 1);
        const target = Math.max(0, root.count - 1);
        return base + root.progress * (target - base);
    }
    readonly property real layoutWidth: cardWidth + stepX * maxSlotOffset
    readonly property real layoutHeight: cardHeight + stepY * maxSlotOffset
    // 没有卡片时零尺寸：不被 HoverHandler 占住一块不可见的区域（右上角要给地图让路）。
    width: count > 0 ? layoutWidth : 0
    height: count > 0 ? layoutHeight : 0

    HoverHandler {
        id: hoverHandler
        onHoveredChanged: root.setPointerInside(hovered)
    }

    // ---- 排序 -------------------------------------------------------------
    /// 显示顺序：元素是 `sources` 的下标，第 0 个就是前排。
    property var order: []
    /// 每个源的上次读数与「上次读数变化时刻」（`Date.now()` 毫秒），按 **id** 记账：
    /// 启用/停用某一路会让下标整体位移，按 id 才不会把状态错记到别的源上。
    property var lastRawById: ({})
    property var lastTextById: ({})
    property var lastChangeById: ({})

    function sourceId(index) {
        const s = root.sources ? root.sources[index] : null;
        return s && s.id !== undefined ? String(s.id) : ("#" + index);
    }
    /// 跨制式可比的「中国烈度等效值」；无数据给远低于任何真实值的哨兵。
    function equivalent(index) {
        const s = root.sources ? root.sources[index] : null;
        if (!s || s.hasData !== true) return -1000;
        const raw = Number(s.raw);
        if (!isFinite(raw)) return -1000;
        let value;
        if (s.standard === "jma") value = (raw - 0.2) / 0.75;
        else if (s.standard === "kma") value = raw - 1.0;
        else value = raw;
        // 静稳值（NIED 0 震度 ≈ -0.27、KMA MMI 1 = 0）统一夹到 0：各源的「常态基线」
        // 本来就不是同一个数，不夹平的话安静时前排会随机落到 KMA / NIED 之间。
        return Math.max(0, value);
    }
    /// 「有读数」：有数据且折算后 ≥ 0.5（CSIS 1 / MMI 2 / JMA 1 起）。
    /// 静稳基线（NIED 0 震度、KMA MMI 1、CSIS 0）不算 —— 见文件头的 ②。
    function hasReading(index) {
        return root.equivalent(index) >= 0.5;
    }
    /// 排序用的等效读数（量化到 0.1 度）：连续震度持续小幅跳动，逐位比较会让并列的两路
    /// 反复交换。没有读数的源给 -1（低于任何真实读数，但仍按数据源顺序互相比）。
    function priority(index) {
        if (!root.hasReading(index)) return -1;
        return Math.round(root.equivalent(index) * 10) / 10;
    }
    /// 「读数刚变过」：只有真的有读数的那几路才算（静稳抖动不算「变化」）。
    function isHot(index) {
        if (!root.hasReading(index)) return false;
        const at = root.lastChangeById[root.sourceId(index)] || 0;
        return at > 0 && (Date.now() - at) <= root.hotWindowMs;
    }
    /// 排序比较：变化过 → 读数高 → 数据源注册顺序。
    function compare(a, b) {
        const hotA = root.isHot(a) ? 1 : 0;
        const hotB = root.isHot(b) ? 1 : 0;
        if (hotA !== hotB) return hotB - hotA;
        const valueA = root.priority(a);
        const valueB = root.priority(b);
        if (valueA !== valueB) return valueB - valueA;
        return a - b;
    }
    function desiredOrder() {
        const list = [];
        for (let i = 0; i < root.count; ++i) list.push(i);
        list.sort(root.compare);
        return list;
    }
    function sameOrder(a, b) {
        if (!a || !b || a.length !== b.length) return false;
        for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
        return true;
    }
    function applyOrder() {
        // 收起态才重排（见 onExpandedChanged 的注释）；但源的数量变了必须立刻重排，
        // 否则 order 与下标对不上、slotOf() 全体回落成下标（启用/停用一路时会出现错位）。
        if (root.expanded && root.order.length === root.count) return;
        const desired = root.desiredOrder();
        const current = root.order || [];
        let next = desired;
        const front = current[0];
        if (current.length === desired.length && desired.length > 0
                && front !== undefined && desired[0] !== front
                && current.indexOf(front) >= 0
                // 迟滞只在「两路都有真实读数」时生效：前排已经没读数了（震完回落）就必须
                // 立刻按顺序让位，否则一个已静稳的源会长期占着前排、盖住 NIED。
                && root.hasReading(front) && root.hasReading(desired[0])
                && root.isHot(front) === root.isHot(desired[0])
                && root.priority(desired[0]) - root.priority(front) < 0.3) {
            // 迟滞：前排只有在被「明显」超过（或刚从冷变热 / 从热变冷）时才让位，
            // 否则收起态每 100 ms 交换一次，看起来像在闪。
            const rest = [];
            for (let i = 0; i < desired.length; ++i) {
                if (desired[i] !== front) rest.push(desired[i]);
            }
            next = [front].concat(rest);
        }
        if (!root.sameOrder(current, next)) root.order = next;
    }
    /// 数据帧到达：先记录「读数变没变」，再重排。
    function sync() {
        const now = Date.now();
        const raws = {};
        const texts = {};
        const changes = {};
        for (let i = 0; i < root.count; ++i) {
            const s = root.sources[i] || ({});
            const id = root.sourceId(i);
            const text = s.hasData === true ? String(s.text) : "--";
            const raw = s.hasData === true ? Number(s.raw) : NaN;
            const previousRaw = root.lastRawById[id];
            const previousText = root.lastTextById[id];
            const previousChange = root.lastChangeById[id] || 0;
            let changed = false;
            if (s.hasData === true) {
                if (previousRaw === undefined) changed = true;   // 首帧已带数值 = 刚变化
                else if (!isFinite(previousRaw) || Math.abs(raw - previousRaw) > root.changeEpsilon)
                    changed = true;
                if (previousText !== undefined && previousText !== text) changed = true;
            }
            changes[id] = changed ? now : previousChange;
            raws[id] = raw;
            texts[id] = text;
        }
        root.lastRawById = raws;
        root.lastTextById = texts;
        root.lastChangeById = changes;
        // 首次装填时 order 还是空的：即使正在展开也必须立刻排一次。
        if (!root.expanded || root.order.length !== root.count) root.applyOrder();
    }
    onSourcesChanged: root.sync()
    Component.onCompleted: root.sync()

    /// 某张卡在堆叠里的槽位（0 = 前排）。
    function slotOf(index) {
        const list = root.order || [];
        const slot = list.indexOf(index);
        return slot >= 0 ? slot : index;
    }
    function noteCardSize(w, h) {
        if (w > 1 && Math.abs(w - root.cardWidth) > 0.5) root.cardWidth = w;
        if (h > 1 && Math.abs(h - root.cardHeight) > 0.5) root.cardHeight = h;
    }
    /// 供测试/调试：前排那张卡的数据源 id。
    readonly property string frontId: count > 0 ? root.sourceId(root.order.length > 0
                                                              ? root.order[0] : 0) : ""

    Repeater {
        // model 用**长度**而不是数组本身：stationSources 每 100 ms 全量重建，
        // 若直接当 model，Repeater 会把每张玻璃卡（含模糊层）反复销毁重建。
        model: root.count
        delegate: StationIntensityCard {
            required property int index
            readonly property int slot: root.slotOf(index)
            objectName: "stationCard-" + root.sourceId(index)
            theme: root.theme
            backdropSource: root.backdropSource
            backdropLive: root.cardBackdropLive
            // 收起态里靠后的卡把玻璃底衬调薄：64 px 模糊 + 一层厚底衬的结果就是
            // 「一片均值」，9 px 的纸边看上去像纯色（用户：「底下的两张没有模糊」）。
            // 调薄之后，模糊过的地图颜色能真正透出来，纸边才是透明的玻璃。
            //
            // **只在浅色主题下这么做**：深色主题的底衬是深色的，调薄会让浅色地图大片
            // 透上来，卡片直接变成灰色板子。深色主题靠发丝边 + 那层白色提亮分层次。
            tintOpacity: slot === 0 || root.theme.dark ? 1.0
                         : (0.62 + 0.38 * root.progress)
            maxInfo: (root.sources && root.sources[index]) || ({})
            sourceName: maxInfo.name
            caption: maxInfo.caption
            standard: maxInfo.standard

            // 前排（slot 0）永远贴住容器右缘：容器宽度是从 progress 推出来的，
            // 右缘固定，所以展开时只有后面的卡向左/向下让位，前排卡不会跟着漂。
            // 槽位超过「最远纸条」的那些卡（收起态的卡 4/5）贴在最后一格上，
            // 于是它们只作为一摞的一部分存在，不会从容器左边探出去。
            x: root.width - root.cardWidth - Math.min(slot, root.maxSlotOffset) * root.stepX
            y: Math.min(slot, root.maxSlotOffset) * root.stepY
            // 前排压在后面那些卡的上面：槽位越靠前 z 越大。
            z: root.count - slot
            // 收起态只露前 `peekCount` 张：第 4 张起随 progress 淡出。
            opacity: slot < root.peekCount ? 1 : root.progress
            // 只在**彻底静下来的收起态**才把被淡出的那两张从场景里摘掉：
            // 它们是透明的，却仍然让 GlassCard 的模糊层每 500 ms 采一次地图
            // （「隐藏内容不做无意义模糊」的契约）。动画一开始（expanded 或 progress>0）
            // 就重新挂上 —— 那时它们的 opacity 还接近 0，Loader 重建的纹理在淡入里看不出来，
            // 而**收回过程中**绝不会触发重建（否则卡片滑回去时模糊会闪）。
            // 注意判据里必须带 `expanded`：展开的第一帧 progress 还是 0，只看 progress
            // 会把卡 4/5 在这一帧里摘掉、下一帧再挂上（又是一次 Loader 重建）。
            visible: slot < root.peekCount || root.expanded || root.progress > 0.0001
            // 收起态里换前排时的滑移（几像素）：只在「静止的收起态」开动画。
            // 展开/收回过程中 x/y 是由 progress 逐帧推出来的，若这里也开 Behavior，
            // 会变成「动画套动画」——卡片会明显落在容器边缘后面。
            // 判据必须是 `!expanded && progress == 0` 两个条件：只看 progress 的话，
            // 展开的第一帧 progress 还是 0、Behavior 会把那一小步也做成动画；
            // 只看 !expanded 的话，收回动画全程都会走 Behavior。
            Behavior on x {
                enabled: !root.expanded && root.progress <= 0.0001
                NumberAnimation { duration: root.expandDuration; easing.type: Easing.OutCubic }
            }
            Behavior on y {
                enabled: !root.expanded && root.progress <= 0.0001
                NumberAnimation { duration: root.expandDuration; easing.type: Easing.OutCubic }
            }
            onImplicitWidthChanged: root.noteCardSize(implicitWidth, implicitHeight)
            onImplicitHeightChanged: root.noteCardSize(implicitWidth, implicitHeight)

            /// 露在后面的深度（0 = 前排）。
            readonly property real stackDepth: Math.max(0, slot)
            // 收起态的层次只靠**一条发丝边 + 一点点压暗**，绝不盖实色纸：
            // 底下必须还看得见磨砂玻璃的模糊（用户：「底下的两张没有模糊，是纯色的」）。
            // 压白/盖不透明纸都会把模糊糊成平色，所以这层是 `color: transparent` 的空框。
            // 压暗用中性黑、越深越重（第 2 张 4%、第 3 张 8%）：只让纸边比前排深一档，
            // 模糊本身的明暗对比仍然在。深色主题靠提亮，所以换成白色。
            // **不要**改用「加深描边 / 投影做层次」：那两版在浅色地图上都很脏
            // （用户原话「好丑」）。也不要用 `visible` 开关卡本身：玻璃底的 Loader 重建会闪。
            Rectangle {
                anchors.fill: parent
                z: 50
                radius: parent.radius
                color: "transparent"
                border.width: 1
                border.color: root.theme.dark ? "#314044" : "#D5E0E2"
                opacity: stackDepth > 0 ? (1 - root.progress) : 0
                visible: opacity > 0.001
            }
            Rectangle {
                anchors.fill: parent
                z: 51
                radius: parent.radius
                color: root.theme.dark ? "#FFFFFF" : "#0E2A30"
                opacity: stackDepth > 0 ? 0.04 * stackDepth * (1 - root.progress) : 0
                visible: opacity > 0.001
            }
        }
    }
}
