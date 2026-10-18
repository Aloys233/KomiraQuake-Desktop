import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
// 本模块的 C++ 类型（VectorMapItem）。显式导入而非依赖同目录隐式解析：
// 主程序由 loadFromModule 走模块系统，gui_test 则按 qrc URL 直接加载，
// 显式导入在两种加载方式下都成立。
import KomiraQuake

// 原生栅格瓦片地图（slippy map）。《NATIVE_PORT_SPEC》 §8 / §12。
// 默认只画"最近一次地震"的 X 十字；有焦点事件时画焦点事件，并按走时表反解画 P/S 波前圆。
Item {
    id: root
    objectName: "mapView"
    property Theme theme: Theme {}
    readonly property alias renderLayer: mapRenderLayer
    readonly property var shiftedUser: root.hasUser ? root.shiftCoord(root.userLat, root.userLon) : ({ lat: 0, lon: 0 })
    property real centerLat: 35.0
    property real centerLon: 105.0
    property real zoom: 6.0
    property string tileUrlTemplate: "https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=7&x={x}&y={y}&z={z}"
    property var subdomains: ["1", "2", "3", "4"]
    /// 地图上唯一要画的那一次地震。
    property var event
    /// 是否有显式焦点（活动预警或用户点击）。仅此时才自动取景，
    /// 否则「最近一次事件」只画标记，不抢镜头。
    property bool hasFocus: false
    /// 相机是否在动（拖动 / 缩放动画 / 波前跟随）。玻璃卡片的背景模糊只在移动时逐帧
    /// 重采样：live 模糊源会让场景图永不空闲，窗口静止时也持续渲染地图子树。
    property bool cameraMoving: false
    Timer {
        id: cameraMotionTimer
        interval: 180
        onTriggered: root.cameraMoving = false
    }
    function noteCameraMoved() {
        root.cameraMoving = true;
        cameraMotionTimer.restart();
    }
    // 相机一变就收起悬停浮层：它锚在屏幕坐标上，跟着镜头漂会与测站错位。
    onCenterLatChanged: { noteCameraMoved(); stationHover.hide(); }
    onCenterLonChanged: { noteCameraMoved(); stationHover.hide(); }
    onZoomChanged: { noteCameraMoved(); stationHover.hide(); }
    /// 是否存在活动预警：空闲自动归位时优先回到预警震中而非我的位置。
    property bool warningActive: false
    property real userLat: 0.0
    property real userLon: 0.0
    property bool hasUser: false
    /// 镜头是否锁定在焦点事件上（工具条的"跟随震中"按下态）。默认不跟随：无定位时地图是全国概览。
    property bool following: false
    /// 用户是否手动控制过镜头（拖动/缩放/回到我的位置/复位）。用于避免定位到达时抢镜头。
    property bool userMovedCamera: false
    /// 指针正按在地图上（按下到松开）。手势期间到达的新事件聚焦、波前逐帧跟随、
    /// 空闲/结束归位一律推迟——这些程序化镜头会与拖拽逐帧抢写 center/zoom，
    /// 手感上就是"鼠标被抢走"。
    property bool gestureActive: false
    /// 手势期间收到的**新焦点**（新实时事件、列表中显式选中）：记住，松开后再飞过去。
    /// 波前逐帧跟随不记——松手后跟随循环下一帧自然追上；空闲/结束归位直接作废
    /// （用户正在自己看地图，拖动已重启空闲计时）。
    property bool pendingFocus: false
    function beginGesture() {
        gestureActive = true;
    }
    function endGesture() {
        if (!gestureActive) return;
        gestureActive = false;
        if (pendingFocus) {
            pendingFocus = false;
            frameEventImpl(undefined);
        }
        // 手势中窗口尺寸可能变过（minZoom 抬升），补做一次下界检查。
        if (root.zoom < root.minZoom) moveCamera(root.centerLat, root.centerLon, root.minZoom, 0);
    }
    // 窗口隐藏/离开时可能收不到松开事件：不能把手势态卡在 true，否则之后所有镜头都会被冻结。
    onVisibleChanged: if (!visible) endGesture()
    /// 左侧 HUD 的**宽度**（px）：取景时按它让开左侧竖栏。
    property real hudWidth: 0
    /// 左侧 HUD 的**高度**（px，含余量）：取景时按它让开顶部。
    property real hudHeight: 80
    /// 左下角徽章（数据源 + 时钟）合计占用的高度（px）。
    property real bottomBadgeHeight: 92
    readonly property string basemap: app.settings.basemapId
    readonly property bool gcjDatum: basemap === "amap_vector" ||
                                     basemap === "petal" ||
                                     (basemap === "custom" && app.settings.customBasemapDatum === 1)
    /// 瓦片源支持的最低层级：Petal 自建服务没有 z0（预热与缓存从 z1 起），
    /// 父层兜底不能越过这个下界，否则会反复请求 404。
    readonly property int tileMinZoom: basemap === "petal" ? 1 : 0
    property bool previousGcjDatum: gcjDatum

    // 地图交互选点模式（桌面端关键功能）
    property bool pickingLocation: false
    signal locationPicked(real lat, real lon)

    // 事件列表 / 设置由主窗口裁决状态，工具条只负责触发。
    property bool listExpanded: false
    signal toggleListRequested()
    signal openSettingsRequested()

    function coordAt(screenXPos, screenYPos) {
        const z = tileZoom;
        const s = tileScale;
        const px = screenXPos + originX;
        const py = screenYPos + originY;
        const worldLon = wrapLon(unprojLon(px / s, z));
        const worldLat = clampLat(unprojLat(py / s, z));
        if (gcjDatum && app && typeof app.gcj02ToWgs84 === "function") {
            const pt = app.gcj02ToWgs84(worldLat, worldLon);
            return { lat: pt.y, lon: pt.x };
        }
        return { lat: worldLat, lon: worldLon };
    }

    function wrapLon(lon) { return ((lon + 180) % 360 + 360) % 360 - 180; }
    function clampLat(lat) { return Math.max(-85.05112878, Math.min(85.05112878, lat)); }
    /// 镜头动画：程序化切换（滚轮/按钮缩放、事件取景、回到我的位置、复位）平滑补间；
    /// 拖动平移保持 1:1 跟手，直接写属性并停掉动画。
    readonly property int cameraDuration: theme.reduceMotion ? 0 : 600
    readonly property int zoomDuration: theme.reduceMotion ? 0 : 180

    /// 三段式镜头缓动（与 Android 侧同名函数一致），平移与缩放共用时间轴、各自分配路程：
    ///   1. 0–35%   快速平移（走完 60% 路程），缩放只走 10%；
    ///   2. 35–75%  缩放主段（再走 75%），平移明显降速，只走 34%；
    ///   3. 75–100% 收尾，平移最后 6%、缩放最后 15% 一起缓慢落位。
    /// 用分段三次 Hermite：节点间速度连续（无顿挫），且各段单调（不冲过头）。
    function cameraHermite(t, ts, ps, ms) {
        if (t <= 0) return 0;
        if (t >= 1) return 1;
        let i = 0;
        while (i < ts.length - 2 && t > ts[i + 1]) i++;
        const h = ts[i + 1] - ts[i];
        const u = (t - ts[i]) / h;
        const u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * ps[i]
             + (u3 - 2 * u2 + u) * h * ms[i]
             + (-2 * u3 + 3 * u2) * ps[i + 1]
             + (u3 - u2) * h * ms[i + 1];
    }
    /// 平移：先快后慢，缩放主段内速度压到最低。
    function cameraPanEase(t) {
        return cameraHermite(t, [0, 0.35, 0.75, 1], [0, 0.60, 0.94, 1], [0, 2.0, 0.45, 0]);
    }
    /// 缩放：前段只轻微启动，主体集中在平移降速的第二段，尾段收口。
    function cameraZoomEase(t) {
        return cameraHermite(t, [0, 0.35, 0.75, 1], [0, 0.10, 0.85, 1], [0, 0.7, 1.05, 0]);
    }
    /// 纯缩放步进（滚轮/按钮）：立即起步的 OutCubic，180ms 内不拖沓。
    function cameraZoomStepEase(t) {
        const u = 1 - t;
        return 1 - u * u * u;
    }

    // 补间只驱动一个线性进度，平移与缩放各用自己的曲线映射，
    // 保证缩放进行时平移已进入慢速段。
    property real cameraProgress: 0
    /// "flight"：聚焦/复位的三段式；"zoom"：纯缩放步进（保持光标锚点不动）。
    property string cameraTweenMode: "flight"
    /// 纯缩放时的锚点：归一化世界坐标 + 相对屏幕中心的像素偏移。
    property real cameraAnchorX: 0
    property real cameraAnchorY: 0
    property real cameraAnchorOffsetX: 0
    property real cameraAnchorOffsetY: 0
    property real cameraStartLat: 0
    property real cameraStartLon: 0
    property real cameraStartZoom: 0
    property real cameraTargetLat: 0
    property real cameraTargetLon: 0
    property real cameraTargetZoom: 0

    NumberAnimation {
        id: cameraMove
        target: root
        property: "cameraProgress"
        from: 0
        to: 1
        easing.type: Easing.Linear
        onStopped: {
            // 正常结束：精确落位并归一化跨 ±180° 的目标经度。
            // 中途被打断（拖动/新目标）则保持当前位置，不回弹。
            if (root.cameraProgress >= 1) {
                root.centerLat = root.cameraTargetLat;
                root.centerLon = root.wrapLon(root.cameraTargetLon);
                root.zoom = root.cameraTargetZoom;
            }
            root.centerLon = root.wrapLon(root.centerLon);
            // 补间结束才允许结算底衬（见 refreshTopLayerState）。
            root.refreshTopLayerState();
        }
    }
    onCameraProgressChanged: {
        if (cameraTweenMode === "zoom") {
            // 纯缩放：由锚点反解中心，光标下的地理点全程不动。
            // （平移与缩放若各用一条曲线，中途锚点会先漂走再回来，感觉"缩放中心不对"。）
            zoom = cameraStartZoom + (cameraTargetZoom - cameraStartZoom) * cameraZoomStepEase(cameraProgress);
            const world = worldSize(zoom);
            centerLon = unprojLon(cameraAnchorX * world - cameraAnchorOffsetX, zoom);
            centerLat = clampLat(unprojLat(cameraAnchorY * world - cameraAnchorOffsetY, zoom));
            return;
        }
        const pan = cameraPanEase(cameraProgress);
        const z = cameraZoomEase(cameraProgress);
        centerLat = cameraStartLat + (cameraTargetLat - cameraStartLat) * pan;
        centerLon = cameraStartLon + (cameraTargetLon - cameraStartLon) * pan;
        zoom = cameraStartZoom + (cameraTargetZoom - cameraStartZoom) * z;
    }

    // 跟随波前时，波前半径每 100ms 才更新一次；若把目标值直接赋值，缩放会一格一格地跳。
    // 用 Behavior 把目标值插值到显示帧率，跟随才够细腻。拖动（following=false 或指针按住）、
    // 程序化补间（cameraMove 进行中）或"减少动态效果"时禁用，保持 1:1 跟手。
    readonly property bool followSmoothing: following && !gestureActive && !cameraMove.running && !theme.reduceMotion && root.visible
    readonly property int followSmoothMs: 120
    Behavior on zoom {
        enabled: root.followSmoothing
        NumberAnimation { duration: root.followSmoothMs; easing.type: Easing.Linear }
    }
    Behavior on centerLat {
        enabled: root.followSmoothing
        NumberAnimation { duration: root.followSmoothMs; easing.type: Easing.Linear }
    }
    Behavior on centerLon {
        enabled: root.followSmoothing
        NumberAnimation { duration: root.followSmoothMs; easing.type: Easing.Linear }
    }

    // 波前半径的显示值（km）。AppController 按显示帧率推送半径（见 updateWaveRadii），
    // 半径本身仍做 120ms 插值，掩盖走时表离散点。
    // 平滑半径（km）而非像素宽度：宽度还含 pxPerKmValue，镜头缩放期间它每帧都在变，
    // 平滑宽度会与 zoom 的 Behavior 叠加成两层滞后。
    // 波前结束（半径归 -1）时 Behavior 关闭，半径立即归零：此时圆已 visible:false，
    // 没必要再补一段收缩动画，也不必担心残值被下一个事件继承。
    // 不按 hasFocus 门控：切换焦点时半径已是当前值，重新聚焦不会出现"从0 涨出来"的动画。
    readonly property bool waveSmoothing: !theme.reduceMotion && root.visible
    readonly property bool wavePActive: (app.waveRadii.pKm || 0) > 0
    readonly property bool waveSActive: (app.waveRadii.sKm || 0) > 0
    property real wavePKmShown: Math.max(0, app.waveRadii.pKm || 0)
    property real waveSKmShown: Math.max(0, app.waveRadii.sKm || 0)
    Behavior on wavePKmShown {
        enabled: root.waveSmoothing && root.wavePActive
        NumberAnimation { duration: root.followSmoothMs; easing.type: Easing.Linear }
    }
    Behavior on waveSKmShown {
        enabled: root.waveSmoothing && root.waveSActive
        NumberAnimation { duration: root.followSmoothMs; easing.type: Easing.Linear }
    }

    /// 视口把世界完整铺满所需的最小缩放。低于它世界（纵向不环绕）会在容器里露出一条背景带
    /// ——看起来就是「缩成了长方形」。与 Android 的 `minZoomForViewport` 同式。
    readonly property real minZoom: Math.max(1, Math.log2(Math.max(width, height) / tileSize))
    function clampZoom(z) { return Math.max(minZoom, Math.min(18, z)); }

    /// 平滑移动镜头；duration 为 0 或开启"减少动态效果"时立即生效。
    /// 经度按最短路径插值，避免跨 ±180° 时绕行一整圈。
    function moveCamera(lat, lon, z, duration, mode) {
        const d = duration === undefined ? cameraDuration : duration;
        cameraMove.stop();
        cameraTweenMode = mode === undefined ? "flight" : mode;
        const targetLat = clampLat(lat);
        const targetLon = centerLon + wrapLon(lon - centerLon);
        const targetZoom = clampZoom(z);
        if (theme.reduceMotion || d <= 0) {
            centerLat = targetLat;
            centerLon = wrapLon(targetLon);
            zoom = targetZoom;
            return;
        }
        cameraStartLat = centerLat;
        cameraStartLon = centerLon;
        cameraStartZoom = zoom;
        cameraTargetLat = targetLat;
        cameraTargetLon = targetLon;
        cameraTargetZoom = targetZoom;
        cameraMove.duration = d;
        cameraMove.start();
    }

    function focusUser() {
        if (!hasUser) return;
        idleResetTimer.stop();
        following = false;
        userMovedCamera = true;
        moveCamera(shiftedUser.lat, shiftedUser.lon, 6, cameraDuration);
    }
    /// 还原默认视野：停止跟随，回到用户位置（无定位时回到全国概览）。
    /// 指针按在地图上时作废：用户正拖着/正要点地图，不该把视野拉走；
    /// 空闲计时已由拖动重启，松手后自然会再触发。
    function resetView() {
        if (gestureActive) return;
        idleResetTimer.stop();
        following = false;
        userMovedCamera = true;
        if (hasUser) moveCamera(shiftedUser.lat, shiftedUser.lon, 6, cameraDuration);
        else frameChina(true);
    }
    /// 震中跟随开关：未跟随时取景震中；跟随时再次点击退出，回到默认视野（无定位即全国概览）。
    function toggleFollowEvent() {
        if (following) resetView();
        else focusEvent();
    }
    /// 中国版图范围（大陆 + 海南 + 台湾，含四至点），用于无定位时的全国概览取景。
    readonly property var chinaBounds: ({ lonMin: 73.5, lonMax: 135.1, latMin: 18.0, latMax: 53.6 })
    /// 按中国范围自动取景：缩放到版图铺满可视区（扣除左侧 HUD 与右侧工具条遮挡），
    /// 再把版图中心对齐到可视区中心。取整到**整数层级**（对齐 Leaflet fitBounds 的 floor 行为），
    /// 保证静止时瓦片 1:1 渲染、无缩放拉伸。取代原先写死的 `zoom 6`。
    function frameChina(animate) {
        // 指针按住地图期间不重取景（布局/尺寸变化会走到这里）。
        if (gestureActive) return;
        if (width <= 0 || height <= 0) return;
        const b = chinaBounds;
        const x0 = projX(b.lonMin, 0), x1 = projX(b.lonMax, 0);
        const y0 = projY(b.latMax, 0), y1 = projY(b.latMin, 0);
        // 与 frameEvent 用同一组遮挡常量，否则两套取景会给出不一致的视野。
        const left = Math.min(hudWidth + 24, width * 0.5);
        const right = 76, top = hudHeight, bottom = bottomBadgeHeight;
        const availableW = Math.max(80, width - left - right);
        const availableH = Math.max(80, height - top - bottom);
        const z = Math.max(minZoom, Math.min(12, Math.floor(Math.log2(Math.min(availableW / (x1 - x0), availableH / (y1 - y0))))));
        const scale = Math.pow(2, z);
        const lat = unprojLat((y0 + y1) / 2 - (top - bottom) / (2 * scale), 0);
        const lon = unprojLon((x0 + x1) / 2 - (left - right) / (2 * scale), 0);
        moveCamera(lat, lon, z, animate === false ? 0 : cameraDuration);
    }
    /// 无定位、无焦点时的初始视野：按中国范围自动取景（不再写死 zoom 6）。
    property bool defaultOverviewDone: false
    function applyDefaultOverview() {
        if (defaultOverviewDone) return;
        if (hasUser || hasFocus) { defaultOverviewDone = true; return; }
        if (width <= 0 || height <= 0) return;
        defaultOverviewDone = true;
        following = false;
        frameChina(false);
    }
    /// **显式**聚焦震中：工具条「跟随震中」按钮与「选中事件」都走这里。
    /// 与自动跟随共用同一套可见波前半径，显式动作也能把镜头带到仍在淡出的波前。
    /// 按住/拖动地图期间不抢镜头：记下来，松开后再飞过去（新事件不该打断拖拽）。
    function focusEvent() {
        if (gestureActive) {
            pendingFocus = true;
            return;
        }
        frameEventImpl(undefined);
    }
    /// 自动聚焦震中区域（不并入用户所在地）：半径跟随 P/S 波前，连续（无级）缩放。
    /// animate === false 用于波前逐帧重取景，避免与补间互相追赶。
    function frameEvent(animate) {
        frameEventImpl(animate);
    }
    function frameEventImpl(animate) {
        // 手势期间一律不写镜头：逐帧跟随会与拖拽抢同一组属性，手感就是"鼠标被抢"。
        if (gestureActive) return;
        following = true;
        idleResetTimer.stop();
        if (!hasEvent || width <= 0 || height <= 0) return;
        const x = projX(shiftedHypo.lon, 0);
        const y = projY(shiftedHypo.lat, 0);
        // 取景必须读**平滑后**的半径（wavePKmShown），与实际画出来的圆同源。
        // 读 app.waveRadii 的原始值会让镜头永远比看到的圆宽一圈，zoom 曲线也自带噪声。
        // 波前很小（t≈0）时保底 100 km；P/S 全部隐藏（历史事件/已淡出）时固定 300 km。
        const pKm = root.wavePKmShown;
        const sKm = root.waveSKmShown;
        const hasWaves = pKm > 0 || sKm > 0;
        // CSIS I 是共同终点；描边只在终点前最后一点范围内淡出，相机跟随到同一边界。
        const radiusKm = hasWaves ? Math.max(100, pKm, sKm) : 300;
        const r = radiusKm * 256 / (360 * 111.32 * Math.max(0.01, Math.cos(shiftedHypo.lat * Math.PI / 180)));
        const minX = x - r, maxX = x + r, minY = y - r, maxY = y + r;
        const left = Math.min(hudWidth + 24, width * 0.5);
        const right = 76, top = hudHeight, bottom = bottomBadgeHeight;
        const availableW = Math.max(80, width - left - right);
        const availableH = Math.max(80, height - top - bottom);
        // 无级缩放：不取整，随波前连续变化（clampZoom 已限制到 [1,18]）。
        const z = clampZoom(Math.log2(Math.min(availableW / (maxX - minX), availableH / (maxY - minY))));
        const scale = Math.pow(2, z);
        const lat = unprojLat((minY + maxY) / 2 - (top - bottom) / (2 * scale), 0);
        const lon = unprojLon((minX + maxX) / 2 - (left - right) / (2 * scale), 0);
        moveCamera(lat, lon, z, animate === false ? 0 : cameraDuration);
    }

    /// 波前（P/S）是否出现过。用于识别"波前消失"的瞬间并自动回到默认视野。
    property bool wavesShown: false
    /// 波前/事件结束后保留当前镜头一段时间，避免刚结束就把用户视野拉走。
    readonly property int eventEndResetMs: 2 * 60 * 1000
    property string pendingEventEndId: ""
    /// 波前出现/结束：供主窗口收起 HUD 等使用。
    signal wavesStarted()
    signal wavesFinished()

    function eventIdentity() {
        return root.event && root.event.id !== undefined ? String(root.event.id) : "";
    }
    function cancelEventEndReset() {
        eventEndResetTimer.stop();
        root.pendingEventEndId = "";
    }

    Timer {
        id: eventEndResetTimer
        interval: root.eventEndResetMs
        repeat: false
        onTriggered: {
            // 用户正按着地图：稍后再归位，别在指针下把画面抽走。
            if (root.gestureActive) { eventEndResetTimer.restart(); return; }
            const currentId = root.eventIdentity();
            // 空事件表示原事件已从焦点中移除；同一 ID 则表示仍停留在已结束事件。
            // 只有切换到了另一场事件才取消本次延迟回位。
            if (root.pendingEventEndId !== "" && currentId !== "" &&
                currentId !== root.pendingEventEndId) {
                root.pendingEventEndId = "";
                return;
            }
            root.pendingEventEndId = "";
            root.resetView();
        }
    }

    // 波前按显示帧率变化时无级跟随：duration 0 直接赋值，避免每 tick 重启 420ms 补间。
    // 初次聚焦的入场补间结束后（cameraMove 停止）才接管，避免打断入场动画。
    Connections {
        target: app
        function onWaveRadiiChanged() {
            const hasWaves = (app.waveRadii.pKm || 0) > 0 || (app.waveRadii.sKm || 0) > 0;
            if (hasWaves) {
                root.cancelEventEndReset();
                if (!root.wavesShown) {
                    root.wavesShown = true;
                    root.wavesStarted();
                }
                if (!root.following || !root.hasFocus) return;
                if (cameraMove.running) return;
                // 用户正按住地图：推迟到松开后再重新取景，避免与拖拽抢镜头。
                // 不记为待执行动作——松手后跟随循环会在下一帧自然追到新半径。
                if (root.gestureActive) return;
                root.frameEvent(false);
                return;
            }
            // 波前全部消失（走完/淡出/事件结束）。仍聚焦（app.hasMapFocus）说明是自然结束而非
            // 用户取消焦点：若之前在跟随震中，立即取消跟随，延迟 2 分钟回到默认镜头。
            // 用户手动操作过镜头（following=false）时不抢镜头。
            if (!root.wavesShown) return;
            root.wavesShown = false;
            if (!app.hasMapFocus) return;
            if (root.following) {
                root.pendingEventEndId = root.eventIdentity();
                root.following = false;
                eventEndResetTimer.restart();
            }
            root.wavesFinished();
        }
    }

    /// 用户操作地图后，若一段时间无操作且未在跟随事件，则自动回到默认视野（我的位置/全国概览）。
    property int idleResetMs: 20000
    Timer {
        id: idleResetTimer
        interval: root.idleResetMs
        onTriggered: {
            if (root.pickingLocation || root.following) return;
            // 用户仍按着地图：本轮作废，重新计时；松开后无操作自然再触发。
            if (root.gestureActive) { idleResetTimer.restart(); return; }
            // 有活动预警时归位到预警震中并重新跟随；否则回到我的位置（无定位则全国概览）。
            if (root.warningActive && root.hasEvent) root.frameEvent();
            else root.resetView();
        }
    }
    function noteCameraInteraction() {
        cancelEventEndReset();
        idleResetTimer.restart();
    }

    onEventChanged: {
        const currentId = root.eventIdentity();
        if (eventEndResetTimer.running && currentId !== "" &&
            currentId !== root.pendingEventEndId) root.cancelEventEndReset();
    }
    /// 当前（或目标）整数缩放层级：相机动画进行中取动画目标，否则取当前值。
    /// 以目标为基准，快速连续滚动时每一格都实打实 ±1，不会被进行中的补间吞掉。
    readonly property int zoomLevel: cameraMove.running ? Math.round(cameraTargetZoom) : Math.round(zoom)

    /// 滚轮/按钮缩放：每次动作按整数层级步进，并吸附到整数 z（对齐参考项目 Leaflet 的整数缩放）。
    /// 整数层级下瓦片以 1:1 像素渲染（tileScale == 1），不会因非整数倍拉伸而发虚或反复切层。
    function zoomAt(x, y, delta) {
        following = false;
        userMovedCamera = true;
        root.noteCameraInteraction();
        const level = zoomLevel;
        const z = clampZoom(level + delta);
        if (z === level) return;   // 已到缩放上下限，避免原地抖动
        const oldSize = worldSize(zoom);
        const anchorX = (projX(centerLon, zoom) + x - width / 2) / oldSize;
        const anchorY = (projY(centerLat, zoom) + y - height / 2) / oldSize;
        const lat = unprojLat(anchorY * worldSize(z) - y + height / 2, z);
        const lon = unprojLon(anchorX * worldSize(z) - x + width / 2, z);
        // 记录锚点：补间过程中由 zoom 反解中心，鼠标下的地点全程钉住。
        cameraAnchorX = anchorX;
        cameraAnchorY = anchorY;
        cameraAnchorOffsetX = x - width / 2;
        cameraAnchorOffsetY = y - height / 2;
        moveCamera(lat, lon, z, zoomDuration, "zoom");
    }
    onWidthChanged: {
        applyDefaultOverview();
        if (root.gestureActive) return;
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
    onHeightChanged: {
        applyDefaultOverview();
        if (root.gestureActive) return;
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
    onHudWidthChanged: reframeForLayout()
    onHudHeightChanged: reframeForLayout()
    /// HUD 尺寸或视口变化后重取景：与 onWidth/onHeight 同一套逻辑，抽出来避免三处重复。
    function reframeForLayout() {
        applyDefaultOverview();
        if (root.gestureActive) return;
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
    // 窗口尺寸变化后最小缩放随之变化：当前缩放若已低于新下界就抬上来，避免露出背景带。
    // 手势期间不写镜头（松开后由 endGesture 补做）。
    onMinZoomChanged: if (!gestureActive && root.zoom < root.minZoom)
                          moveCamera(root.centerLat, root.centerLon, root.minZoom, 0)
    onGcjDatumChanged: {
        if (previousGcjDatum === gcjDatum) return;
        const pt = gcjDatum ? app.wgs84ToGcj02(centerLat, centerLon)
                            : app.gcj02ToWgs84(centerLat, centerLon);
        cameraMove.stop();
        centerLat = clampLat(pt.y);
        centerLon = wrapLon(pt.x);
        previousGcjDatum = gcjDatum;
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
    }

    readonly property int tileSize: 256
    readonly property int tileZoom: Math.max(0, Math.min(18, Math.round(zoom)))
    readonly property real tileScale: Math.pow(2, zoom - tileZoom)
    readonly property real scaledTile: tileSize * tileScale
    /// 视口外预取一圈，避免缩放/切层时同时创建过多 Image delegate。
    /// 拖动跨过边界时只补一条边，旧层仍负责兜底。
    readonly property int prefetchMargin: 1

    /// 当前应绘制的瓦片集合，存**绝对瓦片坐标 + 层级 tz**。用 ListModel + 增量增删：
    /// 平移跨过瓦片边界时只增删边缘一圈，已有瓦片的 delegate 不重建、source 不变，不会整屏闪黑重载。
    /// 缩放跨层级时**保留旧层作为底衬**（underlayZ），新层加载完再移除，避免整层清空造成闪屏。
    ListModel { id: tileModel }
    /// 当前已调度的栅格瓦片数。对外暴露以便测试断言「矢量底图下应为 0」——
    /// tileModel 只有 id、没有 property，C++ 侧取不到它。
    readonly property int tileCount: tileModel.count
    /// 顶层（当前层）的缩放级别；-1 表示尚未初始化。
    property int tileModelZoom: -1
    /// 过渡期保留的旧层级别；-1 表示没有底衬。
    property int underlayZ: -1
    /// 顶层瓦片状态表：key "tz:tx:ty" -> Image.status，用于判断新层是否已加载完成。
    property var tileStatus: ({})
    /// tileStatus 是普通 JS 对象，原地改字段不会触发绑定；每次变更递增此值，
    /// 让依赖 tileStatus 的绑定（父层兜底可见性）重新求值。
    property int tileStatusRevision: 0
    /// 当前顶层尚未结算（Null/Loading）的瓦片数。状态变化增量维护，避免每张瓦片
    /// Loading/Ready 都遍历整个 ListModel。
    property int topLayerPendingTiles: 0
    property int topLayerTileCount: 0
    /// 底衬兜底上限：顶层若始终未就绪（持续失败/超时），也不无限保留底衬。
    Timer {
        id: underlayMaxTimer
        interval: 8000
        onTriggered: root.retireUnderlay()
    }
    /// 顶层全部就绪后撤掉底衬。不再额外等待淡入：新层是直接整层接管的，
    /// 没有「淡入期间露底」的窗口；旧层多留一帧都只会盖住新图。
    /// 用 0ms Timer 而不是直接调用：就绪通知可能发生在 syncTiles 的增删循环里，
    /// 此时立刻删底衬会改动正在遍历的 ListModel。
    Timer {
        id: underlayRetireTimer
        interval: 0
        onTriggered: root.retireUnderlay()
    }
    onTileZoomChanged: {
        // 旧顶层降级为底衬；更早的底衬在 syncTiles 里被丢弃，最多保留两层。
        root.underlayZ = root.tileModelZoom < 0 ? -1 : root.tileModelZoom;
        root.tileModelZoom = root.tileZoom;
        root.topLayerPendingTiles = 0;
        root.topLayerTileCount = 0;
        if (root.underlayZ >= 0) underlayMaxTimer.restart();
        Qt.callLater(syncTiles);
    }
    // 用 Qt.callLater 合并：缩放一帧内 originX/scaledTile 等绑定会分多趟生效，
    // firstTile/lastTile 会随之横跳多次；直接调用会让瓦片集合反复增删（实测创建数 30 倍于合并后），
    // 造成缩放卡死。延迟到事件循环末尾、值稳定后再统一增量同步。
    onFirstTileXChanged: Qt.callLater(syncTiles)
    onFirstTileYChanged: Qt.callLater(syncTiles)
    onLastTileXChanged: Qt.callLater(syncTiles)
    onLastTileYChanged: Qt.callLater(syncTiles)
    // 切到/切出纯矢量底图时要立即重算瓦片集合（要么清空，要么按当前相机重建）。
    Component.onCompleted: {
        if (root.tileModelZoom < 0) root.tileModelZoom = root.tileZoom;
        Qt.callLater(syncTiles);
        Qt.callLater(applyDefaultOverview);
    }

    function retireUnderlay() {
        underlayMaxTimer.stop();
        underlayRetireTimer.stop();
        const u = root.underlayZ;
        if (u < 0) return;
        for (let i = tileModel.count - 1; i >= 0; --i) {
            if (tileModel.get(i).tz === u) removeTileAt(i);
        }
        root.underlayZ = -1;
    }

    function tileStatusKey(tz, tx, ty) { return tz + ":" + tx + ":" + ty; }
    function tileIsPending(status) {
        return status === undefined || status === Image.Null || status === Image.Loading;
    }
    /// 本格瓦片是否已经有像素。父层兜底只在这之前显示；Ready 后立即让位。
    function tileReady(tz, tx, ty) {
        // 依赖 tileStatusRevision：tileStatus 是原地改写的 JS 对象，本身不发通知。
        root.tileStatusRevision;
        return tileStatus[tileStatusKey(tz, tx, ty)] === Image.Ready;
    }

    function noteTileStatus(tz, tx, ty, status) {
        const key = tileStatusKey(tz, tx, ty);
        const oldStatus = tileStatus[key];
        tileStatus[key] = status;
        if ((status === Image.Ready) !== (oldStatus === Image.Ready)) ++root.tileStatusRevision;
        if (tz === tileModelZoom && tileIsPending(oldStatus) !== tileIsPending(status)) {
            root.topLayerPendingTiles += tileIsPending(status) ? 1 : -1;
        }
        refreshTopLayerState();
    }
    function forgetTileStatus(tz, tx, ty) {
        delete tileStatus[tileStatusKey(tz, tx, ty)];
    }
    function removeTileAt(index) {
        const e = tileModel.get(index);
        const key = tileStatusKey(e.tz, e.tx, e.ty);
        if (e.tz === tileModelZoom) {
            if (tileIsPending(tileStatus[key])) {
                root.topLayerPendingTiles = Math.max(0, root.topLayerPendingTiles - 1);
            }
            root.topLayerTileCount = Math.max(0, root.topLayerTileCount - 1);
        }
        delete tileStatus[key];
        tileModel.remove(index);
    }

    function recountTopLayerPending() {
        let pending = 0;
        let total = 0;
        for (let i = 0; i < tileModel.count; ++i) {
            const e = tileModel.get(i);
            if (e.tz === tileModelZoom) {
                total++;
                if (tileIsPending(tileStatus[tileStatusKey(e.tz, e.tx, e.ty)])) {
                    pending++;
                }
            }
        }
        root.topLayerPendingTiles = pending;
        root.topLayerTileCount = total;
    }
    /// 顶层瓦片全部结算（Ready/Error，无 Loading/Null）才撤掉底衬，
    /// 避免冷瓦片（如 Petal 高层）加载慢时被固定计时器提前撤走而露白。
    function refreshTopLayerState() {
        if (underlayZ < 0) return;
        // 镜头补间期间不撤底衬：新视口边缘的瓦片是移动途中才加入集合的，
        // 提前撤掉旧层会让后半程露白。补间结束（onStopped）后再结算。
        if (cameraMove.running) {
            underlayRetireTimer.stop();
            return;
        }
        if (topLayerTileCount > 0 && topLayerPendingTiles === 0) {
            if (!underlayRetireTimer.running) underlayRetireTimer.start();
        } else {
            underlayRetireTimer.stop();
        }
    }

    /// 让 ListModel 与当前可视瓦片范围保持一致。只增删差集，保留已有瓦片与过渡底衬。
    function syncTiles() {
        if (width <= 0 || height <= 0) return;
        // 纯矢量底图不加载栅格瓦片：直接清空并返回，避免为看不见的底图发请求。
        if (root.isVectorOnly()) {
            if (tileModel.count > 0) {
                for (let i = tileModel.count - 1; i >= 0; --i) removeTileAt(i);
                retireUnderlay();
            }
            return;
        }
        const z = tileZoom;
        if (tileModelZoom < 0) tileModelZoom = z;
        const n = Math.pow(2, z);
        const keep = ({});
        for (let tx = firstTileX; tx <= lastTileX; ++tx) {
            for (let ty = firstTileY; ty <= lastTileY; ++ty) {
                if (ty < 0 || ty >= n) continue;
                keep[z + ":" + tx + ":" + ty] = true;
            }
        }
        for (let i = tileModel.count - 1; i >= 0; --i) {
            const e = tileModel.get(i);
            if (e.tz === z) {
                if (!keep[e.tz + ":" + e.tx + ":" + e.ty]) removeTileAt(i);
            } else if (e.tz !== underlayZ) {
                removeTileAt(i);   // 只保留当前层与唯一底衬层
            }
        }
        // 处理回切到仍保留的底衬层，并把本次差集同步变成增量状态基线。
        recountTopLayerPending();
        const have = ({});
        for (let i = 0; i < tileModel.count; ++i) {
            const e = tileModel.get(i);
            if (e.tz === z) have[e.tz + ":" + e.tx + ":" + e.ty] = true;
        }
        for (const key in keep) {
            if (have[key]) continue;
            const parts = key.split(":");
            tileStatus[key] = Image.Null;
            root.topLayerPendingTiles++;
            root.topLayerTileCount++;
            tileModel.append({ tx: parseInt(parts[1]), ty: parseInt(parts[2]), tz: z });
        }
        refreshTopLayerState();
    }

    function shiftCoord(lat, lon) {
        // 日本及其它境外测站本来就不应做 GCJ-02 纠偏。提前在 QML 侧筛掉，
        // 既保持坐标不变，也避免为每个境外站点跨 C++ 调用一次。
        if (!gcjDatum || lat < 0.8293 || lat > 55.8271
                || lon < 72.004 || lon > 137.8347
                || !app || typeof app.wgs84ToGcj02 !== "function") {
            return { lat: lat, lon: lon };
        }
        const pt = app.wgs84ToGcj02(lat, lon);
        return { lat: pt.y, lon: pt.x };
    }

    readonly property bool hasEvent: !!event
    readonly property var shiftedHypo: hasEvent ? shiftCoord(event.latitude, event.longitude) : ({ lat: 0, lon: 0 })
    readonly property real hypocenterX: hasEvent ? screenX(shiftedHypo.lon) : 0
    readonly property real hypocenterY: hasEvent ? screenY(shiftedHypo.lat) : 0
    /// px per km。墨卡托保角，水平/垂直同尺度：worldPx/360° ÷ (111.32·cosφ) km/°。
    readonly property real pxPerKmValue: hasEvent ? pxPerKm(shiftedHypo.lat) : 1.0

    clip: true

    function worldSize(z) { return tileSize * Math.pow(2, z); }
    function projX(lon, z) { return (lon + 180) / 360 * worldSize(z); }
    function projY(lat, z) {
        const rad = clampLat(lat) * Math.PI / 180.0;
        const y = (1.0 - Math.log(Math.tan(rad) + 1.0 / Math.cos(rad)) / Math.PI) / 2.0;
        return y * worldSize(z);
    }
    function unprojLon(x, z) { return x / worldSize(z) * 360.0 - 180.0; }
    function unprojLat(y, z) {
        const n = Math.PI - 2.0 * Math.PI * y / worldSize(z);
        return 180.0 / Math.PI * Math.atan(0.5 * (Math.exp(n) - Math.exp(-n)));
    }
    function pxPerKm(lat) {
        const cosLat = Math.max(0.01, Math.cos(lat * Math.PI / 180.0));
        return (worldSize(tileZoom) * tileScale / 360.0) / (111.32 * cosLat);
    }

    readonly property real originX: projX(centerLon, tileZoom) * tileScale - width / 2
    readonly property real originY: projY(centerLat, tileZoom) * tileScale - height / 2

    function screenX(lon) { return width / 2 + wrapLon(lon - centerLon) / 360 * worldSize(zoom); }
    function screenY(lat) { return projY(lat, tileZoom) * tileScale - originY; }

    // 瓦片范围是整数，只在跨越瓦片边界或整数缩放级别时变化；
    // 拖动/捏合过程中 Repeater 因此不会每帧重建 delegate（否则瓦片会反复闪烁重载）。
    readonly property int firstTileX: Math.floor(originX / scaledTile) - prefetchMargin
    readonly property int firstTileY: Math.floor(originY / scaledTile) - prefetchMargin
    readonly property int lastTileX: Math.floor((originX + width) / scaledTile) + prefetchMargin
    readonly property int lastTileY: Math.floor((originY + height) / scaledTile) + prefetchMargin
    function wrapTile(t, n) { return ((t % n) + n) % n; }

    function tileUrl(x, y, z) {
        // 纯矢量底图没有栅格层：返回空串，避免拼出无效 URL 触发加载失败。
        if (root.isVectorOnly()) return "";
        let templateUrl = tileUrlTemplate;
        if (basemap === "osm") templateUrl = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
        else if (basemap === "petal") templateUrl = "https://tilemap.aloys23.link/petal/{z}/{x}/{y}";
        else if (basemap === "custom") templateUrl = app.settings.customBasemapUrl;
        let url = templateUrl.replace("{x}", x).replace("{y}", y).replace("{z}", z);
        if (url.indexOf("{s}") >= 0) {
            url = url.replace("{s}", subdomains[(x + y) % subdomains.length]);
        }
        return url;
    }

    // ---- 矢量层参数 ------------------------------------------------------
    // 这些属性属于 root，**必须放在 mapRenderLayer 之外**：mapRenderLayer 只是
    // 承载渲染内容（供模糊捕获）的容器，属性挂在它上面就成了子对象属性，
    // 上层读 root.vectorOnly 会得到 undefined。
    //
    // 纯矢量底图（basemap === "vector"）时矢量层作为唯一底图，z 更低；
    // 叠加模式（vectorOverlay）时压在瓦片之上、波前之下。
    readonly property bool vectorOnly: (basemap === "vector")
    onVectorOnlyChanged: Qt.callLater(syncTiles)
    readonly property bool vectorActive: (isVectorOnly() || app.settings.vectorOverlay)
    /// 叠加到 GCJ-02 栅格底图时才对矢量顶点纠偏；纯矢量模式下不纠偏
    //（矢量数据与震中同为 WGS-84，天然一致）。
    readonly property bool vectorGcjShift: !isVectorOnly() && !!gcjDatum
    /// 省级分界线（province 层）只在 z >= 5 时画：全国概览（z4~5 以下）时 35 条省界
    /// 会糊成一团噪声，看不清底图。与 Android 的 PROVINCE_BORDER_MIN_ZOOM 对齐。
    readonly property real provinceBorderMinZoom: 5
    readonly property bool showProvinceBorders: root.vectorActive && app.settings.showProvinceBorders
        && root.zoom >= root.provinceBorderMinZoom
    /// 底图是否为纯矢量（供 syncTiles / tileUrl 早退）。显式函数而非 bool 属性，
    /// 避免属性在 Component.onCompleted 之前尚未求值时得到 undefined。
    function isVectorOnly() { return basemap === "vector"; }

    // The sole backdrop capture target. Controls below are siblings, never captured.
    // 背景、栅格瓦片、矢量层都必须是它的子项：它们既是渲染内容，也是模糊的
    // backdropSource（见下方各 BackdropBlur 的 backdropSource: mapRenderLayer）。
    Item {
        id: mapRenderLayer
        objectName: "mapRenderLayer"
        anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: root.theme.surfaceContainerLow
        // 压到瓦片之下：底衬层要用负 z，背景必须比底衬更低。
        z: -2
    }

    // ---- 矢量层 ----------------------------------------------------------
    // 三层叠加：世界陆地 → 省界 → 区县（EEW 时按烈度着色）。
    // 相机属性直接绑定 root 的中心/缩放：VectorMapItem 内部把几何投影到整数 zoom 缓存，
    // 拖动时只做 translate+scale，不重建路径。

    VectorMapItem {
        id: vectorWorld
        objectName: "vectorWorldLayer"
        anchors.fill: parent
        layerName: "world"
        visible: !!root.vectorActive
        z: root.isVectorOnly() ? -1 : 0
        centerLat: root.centerLat
        centerLon: root.centerLon
        zoom: root.zoom
        gcjShift: false   // 全球层强制不纠偏：outOfChina 是矩形 bbox，蒙古/印度会被误平移
        fill: root.isVectorOnly() ? root.theme.surfaceContainer : "transparent"
        strokeColor: root.isVectorOnly() ? root.theme.outlineVariant : "transparent"
        strokeWidth: 1.0
    }

    VectorMapItem {
        id: vectorProvince
        objectName: "vectorProvinceLayer"
        anchors.fill: parent
        layerName: "province"
        // 省级分界线只在拉近后才画（见 root.showProvinceBorders）：全国概览时线条糊成噪声。
        visible: root.showProvinceBorders
        z: root.isVectorOnly() ? 0 : 1
        centerLat: root.centerLat
        centerLon: root.centerLon
        zoom: root.zoom
        gcjShift: !!root.vectorGcjShift
        fill: "transparent"
        strokeColor: root.theme.outline
        strokeWidth: 1.0
    }

    VectorMapItem {
        id: vectorDistrict
        objectName: "vectorDistrictLayer"
        anchors.fill: parent
        layerName: "district"
        visible: !!root.vectorActive
        z: root.isVectorOnly() ? 0 : 1
        centerLat: root.centerLat
        centerLon: root.centerLon
        zoom: root.zoom
        gcjShift: !!root.vectorGcjShift
        // 区县层平时完全透明：只在有震度的区县才描边 + 填色（见 vector_map_item.cpp）。
        fill: "transparent"
        // #bbbbbb —— kanameishi 的 eewBaseMapActiveStroke。
        strokeColor: "#bbbbbb"
        strokeWidth: 1.0
        districtIntensity: app.districtIntensity
    }

    Repeater {
        model: tileModel
        delegate: Item {
            id: tileCell
            required property int tx
            required property int ty
            required property int tz
            /// 该瓦片在当前（连续）缩放下的屏幕边长：底衬层 tz 更小 → 放大顶替，新层 tz 更大 → 缩小。
            readonly property real size: root.tileSize * Math.pow(2, root.zoom - tz)
            readonly property int side: Math.pow(2, tz)
            /// Repeater 按 model 行序绘制。缩小跨层时旧层（更大的 z）在尾部，
            /// 若不显式压 z，底衬会盖在当前（更小 z）层之上，出现「大 z 瓦片拼到小 z 位置」。
            z: (tz === root.underlayZ && tz !== root.tileModelZoom) ? -1 : 0
            x: tx * size - root.originX
            y: ty * size - root.originY
            width: size + 1
            height: size + 1
            Component.onDestruction: root.forgetTileStatus(tileCell.tz, tileCell.tx, tileCell.ty)
            /// 父层兜底：本格瓦片未就绪时先铺放大后的粗层，避免飞行/平移途中露白。
            /// 取 tz-2（4×4 个格子共享一张父瓦片），通常已在此前缩放经过的层级加载过。
            /// 用「整张父瓦片放大 4× + 裁剪到本格」而不是 sourceClipRect：后者按**源像素**
            /// 裁切，遇到 512px 瓦片源（Petal）会裁错区域，表现为别的瓦片影子叠在本格上。
            Item {
                id: fallbackCell
                anchors.fill: parent
                clip: true
                /// 父层级别；低于图源下界（Petal 无 z0）时为 -1，直接不兜底。
                readonly property int parentLevel: tileCell.tz - 2 >= root.tileMinZoom ? tileCell.tz - 2 : -1
                /// 只在「本格瓦片还没就绪」时显示父层，且**有底衬时不显示**：
                /// 跨层过渡期旧层（tz±1）已经整屏垫在最下面，父层（tz-2）更粗，
                /// 画上去只会盖住更好的旧层 —— 用户看到的就是「整屏闪一下更糊的图」。
                /// 父层只用于没有底衬可垫的首屏/平移缺口。
                readonly property bool active: parentLevel >= 0
                        && tileCell.tz === root.tileModelZoom
                        && root.underlayZ < 0
                        && !root.tileReady(tileCell.tz, tileCell.tx, tileCell.ty)
                visible: active && fallbackImage.status === Image.Ready
                Image {
                    id: fallbackImage
                    width: parent.width * 4
                    height: parent.height * 4
                    // 父瓦片左上角对齐到 4×4 格块，再按本格在父块中的行列偏移窗口。
                    x: -root.wrapTile(tileCell.tx, 4) * parent.width
                    y: -root.wrapTile(tileCell.ty, 4) * parent.height
                    source: fallbackCell.active
                            ? root.tileUrl(root.wrapTile(Math.floor(tileCell.tx / 4), Math.pow(2, fallbackCell.parentLevel)),
                                           Math.floor(tileCell.ty / 4), fallbackCell.parentLevel)
                            : ""
                    asynchronous: true
                    cache: true
                    smooth: true
                }
            }
            Image {
                anchors.fill: parent
                source: root.tileUrl(root.wrapTile(tileCell.tx, tileCell.side), tileCell.ty, tileCell.tz)
                sourceSize.width: root.tileSize
                sourceSize.height: root.tileSize
                asynchronous: true
                cache: true
                smooth: true
                /// 就绪即 1:1 绘制，**不做 opacity 淡入**。
                /// 跨层时整层瓦片同时就绪，若让它从 0 淡到 1，新旧两个 zoom 会在
                /// 180ms 内半透明交叠（用户看到「其他 zoom 的瓦片淡出」）。
                /// 缺口由父层兜底和底衬（underlayZ）保证，不需要靠透明度过渡。
                opacity: status === Image.Ready ? 1 : 0
                onStatusChanged: root.noteTileStatus(tileCell.tz, tileCell.tx, tileCell.ty, status)
            }
        }
    }

    // P/S 波前圆：半径由 AppController 反解走时表得到（km），-1 表示不画；
    // 透明度按烈度影响半径渐隐（对齐 kanameishi），归零即隐藏。
    Rectangle {
        id: pWaveRing
        objectName: "pWaveRing"
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.pKm : -1
        z: 2
        visible: radiusKm > 0 && app.settings.showWavefront
        opacity: app.waveRadii.pOpacity === undefined ? 0 : app.waveRadii.pOpacity
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: Math.max(2, 2 * root.wavePKmShown * root.pxPerKmValue)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: root.theme.pWave
    }

    // S 波径向渐变填充（对齐 kanameishi 的 sWaveFill）：中心透明、边缘着色，
    // 叠加在 S 波描边之下；仅在"影响半径"内可见，透明度随半径由 0.25 渐隐到 0。
    Shape {
        id: sWaveFill
        objectName: "sWaveFill"
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.sKm : -1
        readonly property real fillOpacity: app.waveRadii.sFillOpacity === undefined ? 0 : app.waveRadii.sFillOpacity
        z: 2
        visible: radiusKm > 0 && fillOpacity > 0 && app.settings.showWavefront
        opacity: fillOpacity
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: Math.max(2, 2 * root.waveSKmShown * root.pxPerKmValue)
        height: width
        ShapePath {
            strokeColor: "transparent"
            startX: sWaveFill.width / 2
            startY: 0
            PathAngleArc {
                centerX: sWaveFill.width / 2
                centerY: sWaveFill.height / 2
                radiusX: sWaveFill.width / 2
                radiusY: sWaveFill.height / 2
                startAngle: -90
                sweepAngle: 360
            }
            fillGradient: RadialGradient {
                centerX: sWaveFill.width / 2
                centerY: sWaveFill.height / 2
                centerRadius: sWaveFill.width / 2
                focalX: sWaveFill.width / 2
                focalY: sWaveFill.height / 2
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: root.theme.sWave }
            }
        }
    }

    Rectangle {
        id: sWaveRing
        objectName: "sWaveRing"
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.sKm : -1
        z: 2
        visible: radiusKm > 0 && app.settings.showWavefront
        opacity: app.waveRadii.sOpacity === undefined ? 0 : app.waveRadii.sOpacity
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: Math.max(2, 2 * root.waveSKmShown * root.pxPerKmValue)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: root.theme.sWave
    }

    // 震中 X 十字。参考 kanameishi eqlistCross.svg：10px 米黄描边打底 + 6px 红描边。
    Canvas {
        id: hypocenter
        objectName: "hypocenterMarker"
        z: 3
        visible: root.hasEvent
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: 40
        height: 40
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.lineCap = "round";
            const cross = (color, lineWidth) => {
                ctx.strokeStyle = color;
                ctx.lineWidth = lineWidth;
                ctx.beginPath();
                ctx.moveTo(5, 5);
                ctx.lineTo(35, 35);
                ctx.moveTo(35, 5);
                ctx.lineTo(5, 35);
                ctx.stroke();
            };
            cross(root.theme.hypocenterHalo, 10);
            cross(root.theme.hypocenterCross, 6);
        }
    }

    // 用户位置（带纠偏）
    Item {
        z: 3
        visible: root.hasUser
        x: root.screenX(root.shiftedUser.lon) - 19
        y: root.screenY(root.shiftedUser.lat) - 19
        width: 38; height: 38
        Rectangle {
            anchors.centerIn: parent
            width: 38; height: 38; radius: 19
            color: Qt.rgba(root.theme.severity("NORMAL").r, root.theme.severity("NORMAL").g,
                           root.theme.severity("NORMAL").b, 0.18)
        }
        Rectangle {
            anchors.centerIn: parent
            width: 16; height: 16; radius: 8
            color: root.theme.severity("NORMAL")
        }
    }

    } // mapRenderLayer

    // ---- 测站强度层 ------------------------------------------------------
    // 上千个测站由 C++ 一次批量绘制（逐 Item 会拖垮场景图）。
    // NIED / S-Net（JMA 計測震度）沿用 kanameishi：常态圆点，颜色随连续震度色阶。
    // Pancakes / Wolfx（中国地震烈度 CSIS）对齐 flutterRhythmQuake：活跃测站画带烈度数字的
    // 圆角方标（高烈度白实边），缺测画蓝点。KMA（韩国 MMI）低烈度画 KMA 灰点，
    // MMI ≥ 2 画带数字方标。
    StationMapItem {
        id: stationLayer
        objectName: "stationIntensityLayer"
        anchors.fill: parent
        z: 2
        centerLat: root.centerLat
        centerLon: root.centerLon
        zoom: root.zoom
        snapshot: app.stationSnapshot
        visible: app.settings.showStationLayer
        gcjShift: root.gcjDatum
        niedColors: root.theme.niedColorBand
        csisColors: root.theme.csisColorBand
        kmaColors: root.theme.kmaColorBand
        kmaDotColors: root.theme.kmaDotColorBand
    }

    // ---- 测站悬停浮层 ----------------------------------------------------
    // 只有一个实例，命中靠 stationLayer.stationAt()（O(n) 纯算术，见 C++ 注释）。
    // 指针扫过时不逐像素查询：停下 90 ms 才查一次；地图在动、正在拖拽、选点模式或
    // 测站层隐藏时一律不显示。
    StationTooltip {
        id: stationTooltip
        theme: root.theme
        z: 20
        viewport: Qt.size(root.width, root.height)
        // 显示条件统一由 stationHover.query() 判定（它已经检查图层开关/手势/相机/选点），
        // 这里不再复写一遍，避免两条判据分叉。
        visible: stationHover.active
        info: stationHover.info
        anchor: stationHover.anchor
        // 模糊只采样地图渲染层（瓦片/矢量/测站/波前），不采 HUD、工具条或浮层自身。
        backdropSource: mapRenderLayer
    }
    QtObject {
        id: stationHover
        /// 最近一次命中的测站（未命中为 `{ found:false }`，浮层由 visible 隐藏）。
        property var info: ({ found: false })
        property point anchor: Qt.point(0, 0)
        /// 本次查询对应的指针位置：去抖回调里用它判断指针是否又动过。
        property point pending: Qt.point(-1, -1)
        /// 最近一次已知的指针位置（不区分是否停稳），用于数据刷新时重查同一处。
        property point pointer: Qt.point(-1, -1)
        property bool active: false

        function hide() {
            hoverTimer.stop();
            if (!active) return;
            active = false;
            info = ({ found: false });
        }

        /// 指针移动：重新计时，鼠标停稳后才真正查询。
        function schedule(x, y) {
            pending = Qt.point(x, y);
            pointer = Qt.point(x, y);
            if (active) {
                // 已经显示时不要等满去抖间隔，但仍然限频：先立即查一次，继续移动则再排一次。
                query(x, y, false);
            }
            hoverTimer.restart();
        }

        /// 查询并更新浮层。`settled` 为 true 表示指针已停稳（决定是否允许显示）。
        function query(x, y, settled) {
            if (!app.settings.showStationLayer || root.gestureActive || root.cameraMoving
                || root.pickingLocation) {
                hide();
                return;
            }
            const hit = stationLayer.stationAt(x, y);
            if (!hit.found) {
                // 停稳时未命中：彻底收起；仍在移动时先留着，避免扫过空隙时闪断。
                if (settled) hide();
                return;
            }
            const pos = stationLayer.stationScreenPos(hit.index);
            info = hit;
            anchor = Qt.point(pos.x, pos.y);
            active = true;
        }

        /// 数据帧到达时重查指针下的站：震度在 100 ms 级更新，悬停期间看到旧值会误导。
        /// 只在浮层已经显示时做一次 O(n) 命中（见 StationMapItem::stationAt），不重建图层。
        function refresh() {
            if (active && pointer.x >= 0) query(pointer.x, pointer.y, true);
        }
    }
    // 听图层自己的 stationsApplied()，而不是 StationSnapshot::changed()：QML Connections
    // 可能在图层内部连接之前就绑上，直接听快照会读到上一帧数据（浮层显示旧震度）。
    Connections {
        target: stationLayer
        function onStationsApplied() { stationHover.refresh(); }
    }
    // 关闭测站图层时浮层必须立刻收起：此时地图上已经没有可指的点了。
    Connections {
        target: app.settings
        function onChanged() {
            if (!app.settings.showStationLayer) stationHover.hide();
        }
    }
    Timer {
        id: hoverTimer
        objectName: "stationHoverTimer"
        // 指针停稳 90 ms 才查询：扫过地图时不会沿路闪出一串浮层。
        interval: 90
        onTriggered: stationHover.query(stationHover.pending.x, stationHover.pending.y, true)
    }

    // 右上角：各测站源的实时最大值（仅显示，不参与预警）；每个源一张小卡，横向排列。
    RowLayout {
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 16
        spacing: 8
        z: 10
        visible: !root.pickingLocation && app.settings.showStationLayer
        // model 用**长度**而不是数组本身：stationSources 每 100 ms 全量重建，
        // 若直接当 model，Repeater 会把三张玻璃卡（含模糊层）反复销毁重建。
        Repeater {
            model: app.stationSources.length
            delegate: StationIntensityCard {
                required property int index
                Layout.alignment: Qt.AlignTop
                theme: root.theme
                backdropSource: mapRenderLayer
                backdropLive: root.cameraMoving
                maxInfo: app.stationSources[index] || ({})
                sourceName: maxInfo.name
                caption: maxInfo.caption
                standard: maxInfo.standard
            }
        }
    }

    MouseArea {
        id: mapMouseArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        // 悬停跟踪：指针停稳后由 hoverTimer 调 stationHover.query()，不逐像素查询。
        hoverEnabled: true
        cursorShape: root.pickingLocation ? Qt.CrossCursor
                     : root.gestureActive ? Qt.ClosedHandCursor : Qt.ArrowCursor
        property real lastX: 0
        property real lastY: 0
        property real pressX: 0
        property real pressY: 0
        /// 滚轮增量累积器：凑满一格（angleDelta 120 = 一个刻度）才步进一级，
        /// 兼容高精度滚轮/触摸板的碎增量；一格刻度固定 = z 整数级 ±1。
        property real wheelAccum: 0
        onWheel: (wheel) => {
            wheelAccum += wheel.angleDelta.y;
            const notches = Math.trunc(wheelAccum / 120);
            if (notches !== 0) {
                wheelAccum -= notches * 120;
                root.zoomAt(wheel.x, wheel.y, notches);
            }
            wheel.accepted = true;
        }
        onPressed: (mouse) => {
            // 拖动接管镜头：终止进行中的补间，保证 1:1 跟手。
            cameraMove.stop();
            lastX = mouse.x;
            lastY = mouse.y;
            pressX = mouse.x;
            pressY = mouse.y;
            stationHover.hide();
            // 从按下的第一刻起就冻结程序化镜头：即便还没产生位移，新事件/波前跟随
            // 也不该在指针下把画面抽走。
            root.beginGesture();
        }
        // 松开或被其它控件抢走抓取时都回到自由态（canceled 同样会置 pressed=false）。
        onPressedChanged: if (!pressed) root.endGesture()
        onPositionChanged: (mouse) => {
            // 悬停：不按住时只登记指针位置，停稳后由 hoverTimer 查询一次。
            if (!pressed) {
                stationHover.schedule(mouse.x, mouse.y);
                return;
            }
            // 拖动：1:1 跟手平移镜头。
            root.following = false;
            root.userMovedCamera = true;
            root.noteCameraInteraction();
            const dx = mouse.x - lastX;
            const dy = mouse.y - lastY;
            lastX = mouse.x;
            lastY = mouse.y;
            const z = root.tileZoom;
            const s = root.tileScale;
            const worldPx = root.worldSize(z) * s;   // == tileSize * 2^zoom 的连续世界像素尺寸
            const nx = root.projX(root.centerLon, z) * s - dx;
            const ny = root.projY(root.centerLat, z) * s - dy;
            root.centerLon = root.wrapLon(root.unprojLon(nx / s, z));
            // 纵向不环绕：中心限制在"视口不出世界"的范围内，缩到最小也不会拖出背景带。
            const halfFrac = (root.height / 2) / worldPx;
            const nyFrac = (ny / s) / root.worldSize(z);
            const clampedNy = halfFrac < 0.5 ? Math.min(1 - halfFrac, Math.max(halfFrac, nyFrac)) : 0.5;
            root.centerLat = root.clampLat(root.unprojLat(clampedNy * root.worldSize(z), z));
        }
        onClicked: (mouse) => {
            // 如果移动距离极小判定为点击选点
            if (root.pickingLocation && Math.hypot(mouse.x - pressX, mouse.y - pressY) < 6) {
                const pt = root.coordAt(mouse.x, mouse.y);
                root.locationPicked(pt.lat, pt.lon);
                root.pickingLocation = false;
            }
        }
        // 松开、失去抓取或被取消时都回到自由态：endGesture 幂等，重复调用无副作用。
        onReleased: root.endGesture()
        onCanceled: root.endGesture()
        // area 级 exited：指针离开地图（含移入浮层/工具条）时收起浮层。
        onExited: stationHover.hide()
    }

    GlassCard {
        visible: root.pickingLocation
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 88
        width: Math.min(480, parent.width - 32)
        height: 64
        theme: root.theme
        backdropSource: mapRenderLayer
        backdropLive: root.cameraMoving
        z: 30
        RowLayout {
            anchors.fill: parent; anchors.margins: 12
            AppIcon { name: "map-pin"; size: 20; color: root.theme.accent }
            Text { Layout.fillWidth: true; text: "点击地图，设置基准位置"; wrapMode: Text.Wrap; font.pixelSize: 13; color: root.theme.textPrimary }
            GlassButton { theme: root.theme; text: "取消"; onClicked: root.pickingLocation = false }
        }
    }

    // One frosted toolbar, rather than a capture for every icon.
    // z 必须高于测站层（z: 2），否则测站会画到工具条上面。
    GlassCard {
        objectName: "mapToolbar"
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 48
        z: 10
        height: mapActions.implicitHeight + 8
        theme: root.theme
        backdropSource: mapRenderLayer
        backdropLive: root.cameraMoving
        radius: 14
        Column {
            id: mapActions
            x: 4; y: 4
            spacing: 4
            GlassButton { theme: root.theme; iconName: "plus"; accessibleName: "放大地图"; flat: true; onClicked: root.zoomAt(root.width / 2, root.height / 2, 1) }
            GlassButton { theme: root.theme; iconName: "minus"; accessibleName: "缩小地图"; flat: true; onClicked: root.zoomAt(root.width / 2, root.height / 2, -1) }
            Rectangle { width: 28; height: 1; x: 6; color: root.theme.glassBorder }
            GlassButton { theme: root.theme; visible: root.hasUser; iconName: "navigation"; accessibleName: "回到我的位置"; flat: true; onClicked: root.focusUser() }
            GlassButton {
                theme: root.theme; visible: root.hasEvent; iconName: "locate-fixed"
                accessibleName: root.following ? "正在跟随震中；点击退出跟随" : "恢复跟随震中"
                flat: !root.following; primary: root.following
                onClicked: root.toggleFollowEvent()
            }
            GlassButton {
                theme: root.theme; iconName: "layers"; accessibleName: "切换底图"; flat: true
                onClicked: {
                    const list = ["amap_vector", "petal", "osm", "vector"];
                    app.settings.basemapId = list[(list.indexOf(root.basemap) + 1) % list.length];
                }
            }
            // 应用级导航（kanameishi 式单条工具条）：事件列表抽屉 / 设置页。
            Rectangle { width: 28; height: 1; x: 6; color: root.theme.glassBorder }
            GlassButton { theme: root.theme; iconName: "list"; accessibleName: "事件列表"; flat: !root.listExpanded; primary: root.listExpanded; onClicked: root.toggleListRequested() }
            GlassButton { theme: root.theme; iconName: "settings"; accessibleName: "设置"; flat: true; onClicked: root.openSettingsRequested() }
        }
    }

    // 右下角动态比例尺
    readonly property real scaleCosLat: Math.max(0.01, Math.cos(root.centerLat * Math.PI / 180.0))
    readonly property real currentPxPerKm: (root.worldSize(root.tileZoom) * root.tileScale / 360.0) / (111.32 * scaleCosLat)
    readonly property real targetKm: 70.0 / Math.max(0.0001, currentPxPerKm)
    readonly property int scaleKm: {
        if (targetKm <= 2) return 1;
        if (targetKm <= 5) return 5;
        if (targetKm <= 15) return 10;
        if (targetKm <= 35) return 20;
        if (targetKm <= 75) return 50;
        if (targetKm <= 150) return 100;
        if (targetKm <= 350) return 200;
        if (targetKm <= 750) return 500;
        if (targetKm <= 1500) return 1000;
        return 2000;
    }
    readonly property real scaleBarWidth: Math.max(28, Math.min(130, scaleKm * currentPxPerKm))

    GlassCard {
        theme: root.theme
        backdropSource: mapRenderLayer
        backdropLive: root.cameraMoving
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: Math.max(scaleBarWidth + 16, 56)
        height: 24
        radius: 6
        z: 10

        Column {
            anchors.centerIn: parent
            spacing: 2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.scaleKm + " km"
                color: root.theme.textPrimary
                font.pixelSize: 10
            }
            Rectangle {
                width: root.scaleBarWidth
                height: 2
                color: root.theme.textPrimary
            }
        }
    }
}
