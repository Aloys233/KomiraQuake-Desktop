import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

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
    /// 是否存在活动预警：空闲自动归位时优先回到预警震中而非我的位置。
    property bool warningActive: false
    property real userLat: 0.0
    property real userLon: 0.0
    property bool hasUser: false
    /// 镜头是否锁定在焦点事件上（工具条的"跟随震中"按下态）。默认不跟随：无定位时地图是全国概览。
    property bool following: false
    /// 用户是否手动控制过镜头（拖动/缩放/回到我的位置/复位）。用于避免定位到达时抢镜头。
    property bool userMovedCamera: false
    property real hudInset: 0
    readonly property string basemap: app.settings.basemapId
    readonly property bool gcjDatum: basemap === "amap_vector" ||
                                     basemap === "petal" ||
                                     (basemap === "custom" && app.settings.customBasemapDatum === 1)
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
    readonly property int cameraDuration: theme.reduceMotion ? 0 : 420
    readonly property int zoomDuration: theme.reduceMotion ? 0 : 180

    ParallelAnimation {
        id: cameraMove
        NumberAnimation { id: camLat; target: root; property: "centerLat"; easing.type: Easing.OutCubic }
        NumberAnimation { id: camLon; target: root; property: "centerLon"; easing.type: Easing.OutCubic }
        NumberAnimation { id: camZoom; target: root; property: "zoom"; easing.type: Easing.OutCubic }
        // 跨 ±180° 的插值会落在 [-180, 180] 之外，结束后归一化。
        onStopped: root.centerLon = root.wrapLon(root.centerLon)
    }

    // 跟随波前时，波前半径每 100ms 才更新一次；若把目标值直接赋值，缩放会一格一格地跳。
    // 用 Behavior 把目标值插值到显示帧率，跟随才够细腻。拖动（following=false）、
    // 程序化补间（cameraMove 进行中）或"减少动态效果"时禁用，保持 1:1 跟手。
    readonly property bool followSmoothing: following && !cameraMove.running && !theme.reduceMotion
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

    // 波前半径的显示值（km）。AppController 只按 10Hz 推送半径（见 updateWaveRadii），
    // 波前圆直接绑定的话就是一圈一圈地跳；这里把半径本身插值到显示帧率。
    // 平滑半径（km）而非像素宽度：宽度还含 pxPerKmValue，镜头缩放期间它每帧都在变，
    // 平滑宽度会与 zoom 的 Behavior 叠加成两层滞后。
    // 波前结束（半径归 -1）时 Behavior 关闭，半径立即归零：此时圆已 visible:false，
    // 没必要再补一段收缩动画，也不必担心残值被下一个事件继承。
    // 不按 hasFocus 门控：切换焦点时半径已是当前值，重新聚焦不会出现"从0 涨出来"的动画。
    readonly property bool waveSmoothing: !theme.reduceMotion
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

    function clampZoom(z) { return Math.max(1, Math.min(18, z)); }

    /// 平滑移动镜头；duration 为 0 或开启"减少动态效果"时立即生效。
    /// 经度按最短路径插值，避免跨 ±180° 时绕行一整圈。
    function moveCamera(lat, lon, z, duration) {
        const d = duration === undefined ? cameraDuration : duration;
        cameraMove.stop();
        const targetLat = clampLat(lat);
        const targetLon = centerLon + wrapLon(lon - centerLon);
        const targetZoom = clampZoom(z);
        if (theme.reduceMotion || d <= 0) {
            centerLat = targetLat;
            centerLon = wrapLon(targetLon);
            zoom = targetZoom;
            return;
        }
        camLat.from = centerLat; camLat.to = targetLat; camLat.duration = d;
        camLon.from = centerLon; camLon.to = targetLon; camLon.duration = d;
        camZoom.from = zoom; camZoom.to = targetZoom; camZoom.duration = d;
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
    function resetView() {
        idleResetTimer.stop();
        following = false;
        userMovedCamera = true;
        if (hasUser) moveCamera(shiftedUser.lat, shiftedUser.lon, 6, cameraDuration);
        else frameChina(true);
    }
    /// 震中跟随开关：未跟随时取景震中；跟随时再次点击退出，回到默认视野（无定位即全国概览）。
    function toggleFollowEvent() {
        if (following) resetView();
        else frameEvent();
    }
    /// 中国版图范围（大陆 + 海南 + 台湾，含四至点），用于无定位时的全国概览取景。
    readonly property var chinaBounds: ({ lonMin: 73.5, lonMax: 135.1, latMin: 18.0, latMax: 53.6 })
    /// 按中国范围自动取景：缩放到版图铺满可视区（扣除左侧 HUD 与右侧工具条遮挡），
    /// 再把版图中心对齐到可视区中心。取整到**整数层级**（对齐 Leaflet fitBounds 的 floor 行为），
    /// 保证静止时瓦片 1:1 渲染、无缩放拉伸。取代原先写死的 `zoom 6`。
    function frameChina(animate) {
        if (width <= 0 || height <= 0) return;
        const b = chinaBounds;
        const x0 = projX(b.lonMin, 0), x1 = projX(b.lonMax, 0);
        const y0 = projY(b.latMax, 0), y1 = projY(b.latMin, 0);
        const left = Math.min(hudInset + 24, width * 0.5);
        const right = 76, top = 80, bottom = 64;
        const availableW = Math.max(80, width - left - right);
        const availableH = Math.max(80, height - top - bottom);
        const z = Math.max(1, Math.min(12, Math.floor(Math.log2(Math.min(availableW / (x1 - x0), availableH / (y1 - y0))))));
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
    /// 聚焦震中区域（不并入用户所在地）：半径跟随 P/S 波前，连续（无级）缩放。
    /// animate === false 用于波前逐帧重取景，避免与 420ms 补间互相追赶。
    function frameEvent(animate) {
        following = true;
        idleResetTimer.stop();
        if (!hasEvent || width <= 0 || height <= 0) return;
        const x = projX(shiftedHypo.lon, 0);
        const y = projY(shiftedHypo.lat, 0);
        // 波前很小（t≈0）时保底 100 km；P/S 全部隐藏（历史事件/已淡出）时固定 300 km。
        // 不设半径上限：靠 zoom 下限与"烈度低于可感即淡出"自然收尾。
        const pKm = app.waveRadii.pKm || 0;
        const sKm = app.waveRadii.sKm || 0;
        const hasWaves = pKm > 0 || sKm > 0;
        const radiusKm = hasWaves ? Math.max(100, pKm, sKm) : 300;
        const r = radiusKm * 256 / (360 * 111.32 * Math.max(0.01, Math.cos(shiftedHypo.lat * Math.PI / 180)));
        const minX = x - r, maxX = x + r, minY = y - r, maxY = y + r;
        const left = Math.min(hudInset + 24, width * 0.5);
        const right = 76, top = 80, bottom = 64;
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
    /// 波前出现/结束：供主窗口收起 HUD 等使用。
    signal wavesStarted()
    signal wavesFinished()

    // 波前约 10Hz 变化时无级跟随：duration 0 直接赋值，避免每 tick 重启 420ms 补间。
    // 初次聚焦的入场补间结束后（cameraMove 停止）才接管，避免打断入场动画。
    Connections {
        target: app
        function onWaveRadiiChanged() {
            const hasWaves = (app.waveRadii.pKm || 0) > 0 || (app.waveRadii.sKm || 0) > 0;
            if (hasWaves) {
                if (!root.wavesShown) {
                    root.wavesShown = true;
                    root.wavesStarted();
                }
                if (!root.following || !root.hasFocus) return;
                if (cameraMove.running) return;
                root.frameEvent(false);
                return;
            }
            // 波前全部消失（走完/淡出/事件结束）。仍聚焦（app.hasMapFocus）说明是自然结束而非
            // 用户取消焦点：若之前在跟随震中，自动回到我的位置（无定位则全国概览），并收起 HUD。
            // 用户手动操作过镜头（following=false）时不抢镜头。
            if (!root.wavesShown) return;
            root.wavesShown = false;
            if (!app.hasMapFocus) return;
            if (root.following) root.resetView();
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
            // 有活动预警时归位到预警震中并重新跟随；否则回到我的位置（无定位则全国概览）。
            if (root.warningActive && root.hasEvent) root.frameEvent();
            else root.resetView();
        }
    }
    function noteCameraInteraction() { idleResetTimer.restart(); }
    /// 当前（或目标）整数缩放层级：相机动画进行中取动画目标，否则取当前值。
    /// 以目标为基准，快速连续滚动时每一格都实打实 ±1，不会被进行中的补间吞掉。
    readonly property int zoomLevel: cameraMove.running ? Math.round(camZoom.to) : Math.round(zoom)

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
        moveCamera(lat, lon, z, zoomDuration);
    }
    onWidthChanged: {
        applyDefaultOverview();
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
    onHeightChanged: {
        applyDefaultOverview();
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
    onHudInsetChanged: {
        applyDefaultOverview();
        if (following && hasFocus) Qt.callLater(() => frameEvent(false));
        else if (!hasFocus && !hasUser && !userMovedCamera) Qt.callLater(() => frameChina(false));
    }
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
    /// 视口外多取两圈，平移时新露出的瓦片已就绪，不会一格一格拼出来。
    readonly property int prefetchMargin: 2
    /// 瓦片就绪后淡入时长（ms）：掩盖跨层级切换时清晰度突变造成的顿挫。
    readonly property int tileFadeMs: 180

    /// 当前应绘制的瓦片集合，存**绝对瓦片坐标 + 层级 tz**。用 ListModel + 增量增删：
    /// 平移跨过瓦片边界时只增删边缘一圈，已有瓦片的 delegate 不重建、source 不变，不会整屏闪黑重载。
    /// 缩放跨层级时**保留旧层作为底衬**（underlayZ），新层加载完再移除，避免整层清空造成闪屏。
    ListModel { id: tileModel }
    /// 顶层（当前层）的缩放级别；-1 表示尚未初始化。
    property int tileModelZoom: -1
    /// 过渡期保留的旧层级别；-1 表示没有底衬。
    property int underlayZ: -1
    /// 顶层瓦片状态表：key "tz:tx:ty" -> Image.status，用于判断新层是否已加载完成。
    property var tileStatus: ({})
    /// 底衬兜底上限：顶层若始终未就绪（持续失败/超时），也不无限保留底衬。
    Timer {
        id: underlayMaxTimer
        interval: 8000
        onTriggered: root.retireUnderlay()
    }
    /// 顶层全部就绪后，等新层淡入（见 delegate 的 opacity）完成再撤底衬，避免淡入期间露底。
    Timer {
        id: underlayFadeTimer
        interval: root.tileFadeMs
        onTriggered: root.retireUnderlay()
    }
    onTileZoomChanged: {
        // 旧顶层降级为底衬；更早的底衬在 syncTiles 里被丢弃，最多保留两层。
        root.underlayZ = root.tileModelZoom < 0 ? -1 : root.tileModelZoom;
        root.tileModelZoom = root.tileZoom;
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
    Component.onCompleted: {
        if (root.tileModelZoom < 0) root.tileModelZoom = root.tileZoom;
        Qt.callLater(syncTiles);
        Qt.callLater(applyDefaultOverview);
    }

    function retireUnderlay() {
        underlayMaxTimer.stop();
        underlayFadeTimer.stop();
        const u = root.underlayZ;
        if (u < 0) return;
        for (let i = tileModel.count - 1; i >= 0; --i) {
            if (tileModel.get(i).tz === u) tileModel.remove(i);
        }
        root.underlayZ = -1;
    }

    function noteTileStatus(tz, tx, ty, status) {
        tileStatus[tz + ":" + tx + ":" + ty] = status;
        refreshTopLayerState();
    }
    function forgetTileStatus(tz, tx, ty) {
        delete tileStatus[tz + ":" + tx + ":" + ty];
    }
    /// 顶层瓦片全部结算（Ready/Error，无 Loading/Null）才撤掉底衬，
    /// 避免冷瓦片（如 Petal 高层）加载慢时被固定计时器提前撤走而露白。
    /// 就绪后再等 tileFadeMs，让新层淡入完成，避免淡入期间露底。
    function refreshTopLayerState() {
        if (underlayZ < 0) return;
        const z = tileModelZoom;
        let total = 0, loading = 0;
        for (let i = 0; i < tileModel.count; ++i) {
            const e = tileModel.get(i);
            if (e.tz !== z) continue;
            total++;
            const st = tileStatus[e.tz + ":" + e.tx + ":" + e.ty];
            if (st === undefined || st === Image.Null || st === Image.Loading) loading++;
        }
        if (total > 0 && loading === 0) {
            if (!underlayFadeTimer.running) underlayFadeTimer.start();
        } else {
            underlayFadeTimer.stop();
        }
    }

    /// 让 ListModel 与当前可视瓦片范围保持一致。只增删差集，保留已有瓦片与过渡底衬。
    function syncTiles() {
        if (width <= 0 || height <= 0) return;
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
                if (!keep[e.tz + ":" + e.tx + ":" + e.ty]) tileModel.remove(i);
            } else if (e.tz !== underlayZ) {
                tileModel.remove(i);   // 只保留当前层与唯一底衬层
            }
        }
        const have = ({});
        for (let i = 0; i < tileModel.count; ++i) {
            const e = tileModel.get(i);
            if (e.tz === z) have[e.tz + ":" + e.tx + ":" + e.ty] = true;
        }
        for (const key in keep) {
            if (have[key]) continue;
            const parts = key.split(":");
            tileModel.append({ tx: parseInt(parts[1]), ty: parseInt(parts[2]), tz: z });
        }
        refreshTopLayerState();
    }

    function shiftCoord(lat, lon) {
        if (!gcjDatum || !app || typeof app.wgs84ToGcj02 !== "function") return { lat: lat, lon: lon };
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

    // The sole backdrop capture target. Controls below are siblings, never captured.
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
            Image {
                anchors.fill: parent
                source: root.tileUrl(root.wrapTile(tileCell.tx, tileCell.side), tileCell.ty, tileCell.tz)
                sourceSize.width: root.tileSize
                sourceSize.height: root.tileSize
                asynchronous: true
                cache: true
                smooth: true
                /// 就绪后淡入：底衬（旧层）在下方透出，跨层切换因此是渐变而非整块突变。
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: root.tileFadeMs; easing.type: Easing.OutCubic }
                }
                onStatusChanged: root.noteTileStatus(tileCell.tz, tileCell.tx, tileCell.ty, status)
            }
        }
    }

    // P/S 波前圆：半径由 AppController 反解走时表得到（km），-1 表示不画；
    // 透明度按烈度影响半径渐隐（对齐 kanameishi），归零即隐藏。
    Rectangle {
        id: pWaveRing
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.pKm : -1
        visible: radiusKm > 0
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
        visible: radiusKm > 0 && fillOpacity > 0
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
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.sKm : -1
        visible: radiusKm > 0
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

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: root.pickingLocation ? Qt.CrossCursor : Qt.ArrowCursor
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
        }
        onPositionChanged: (mouse) => {
            if (!pressed) return;
            root.following = false;
            root.userMovedCamera = true;
            root.noteCameraInteraction();
            const dx = mouse.x - lastX;
            const dy = mouse.y - lastY;
            lastX = mouse.x;
            lastY = mouse.y;
            const z = root.tileZoom;
            const s = root.tileScale;
            const nx = root.projX(root.centerLon, z) * s - dx;
            const ny = root.projY(root.centerLat, z) * s - dy;
            root.centerLon = root.wrapLon(root.unprojLon(nx / s, z));
            root.centerLat = root.clampLat(root.unprojLat(ny / s, z));
        }
        onClicked: (mouse) => {
            // 如果移动距离极小判定为点击选点
            if (root.pickingLocation && Math.hypot(mouse.x - pressX, mouse.y - pressY) < 6) {
                const pt = root.coordAt(mouse.x, mouse.y);
                root.locationPicked(pt.lat, pt.lon);
                root.pickingLocation = false;
            }
        }
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
        z: 30
        RowLayout {
            anchors.fill: parent; anchors.margins: 12
            AppIcon { name: "map-pin"; size: 20; color: root.theme.accent }
            Text { Layout.fillWidth: true; text: "点击地图，设置基准位置"; wrapMode: Text.Wrap; font.pixelSize: 13; color: root.theme.textPrimary }
            GlassButton { theme: root.theme; text: "取消"; onClicked: root.pickingLocation = false }
        }
    }

    // One frosted toolbar, rather than a capture for every icon.
    GlassCard {
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: 48
        height: mapActions.implicitHeight + 8
        theme: root.theme
        backdropSource: mapRenderLayer
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
                    const list = ["amap_vector", "petal", "osm"];
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
