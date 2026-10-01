import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
    property real userLat: 0.0
    property real userLon: 0.0
    property bool hasUser: false
    /// 镜头是否锁定在焦点事件上（工具条的"跟随震中"按下态）。默认不跟随：无定位时地图是全国概览。
    property bool following: false
    /// 用户是否手动控制过镜头（拖动/缩放/回到我的位置/复位）。用于避免定位到达时抢镜头。
    property bool userMovedCamera: false
    property real hudInset: 0
    readonly property string basemap: app.settings.basemapId
    readonly property bool gcjDatum: basemap === "amap_vector" || basemap === "amap_satellite" ||
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
        following = false;
        userMovedCamera = true;
        moveCamera(shiftedUser.lat, shiftedUser.lon, 6.5, cameraDuration);
    }
    /// 还原默认视野：停止跟随，回到用户位置（无定位时回到全国概览）。
    function resetView() {
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
    /// 按中国范围自动取景：缩放到版图刚好铺满可视区（扣除左侧 HUD 与右侧工具条遮挡），
    /// 再把版图中心对齐到可视区中心。取代原先写死的 `zoom 6`。
    function frameChina(animate) {
        if (width <= 0 || height <= 0) return;
        const b = chinaBounds;
        const x0 = projX(b.lonMin, 0), x1 = projX(b.lonMax, 0);
        const y0 = projY(b.latMax, 0), y1 = projY(b.latMin, 0);
        const left = Math.min(hudInset + 24, width * 0.5);
        const right = 76, top = 80, bottom = 64;
        const availableW = Math.max(80, width - left - right);
        const availableH = Math.max(80, height - top - bottom);
        const z = Math.max(1, Math.min(12, Math.log2(Math.min(availableW / (x1 - x0), availableH / (y1 - y0)))));
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
    /// animate === false 用于尺寸/遮挡变化时的即时重新取景，避免与连续布局事件互相追赶。
    function frameEvent(animate) {
        following = true;
        if (!hasEvent || width <= 0 || height <= 0) return;
        const x = projX(shiftedHypo.lon, 0);
        const y = projY(shiftedHypo.lat, 0);
        // Bound wave context so old, distant wavefronts cannot dominate.
        // 取景至少覆盖震中周围 300 km；有更大波前时最多放宽到 1000 km。
        const radiusKm = Math.min(1000, Math.max(300, app.waveRadii.pKm || 0, app.waveRadii.sKm || 0));
        const r = radiusKm * 256 / (360 * 111.32 * Math.max(0.01, Math.cos(shiftedHypo.lat * Math.PI / 180)));
        let minX = x - r, maxX = x + r, minY = y - r, maxY = y + r;
        if (hasUser) {
            const ux = x + wrapLon(shiftedUser.lon - shiftedHypo.lon) / 360 * 256;
            const uy = projY(shiftedUser.lat, 0);
            minX = Math.min(minX, ux); maxX = Math.max(maxX, ux);
            minY = Math.min(minY, uy); maxY = Math.max(maxY, uy);
        }
        const left = Math.min(hudInset + 24, width * 0.5);
        const right = 76, top = 80, bottom = 64;
        const availableW = Math.max(80, width - left - right);
        const availableH = Math.max(80, height - top - bottom);
        const z = Math.max(1, Math.min(12, Math.log2(Math.min(availableW / (maxX - minX), availableH / (maxY - minY)))));
        const scale = Math.pow(2, z);
        const lat = unprojLat((minY + maxY) / 2 - (top - bottom) / (2 * scale), 0);
        const lon = unprojLon((minX + maxX) / 2 - (left - right) / (2 * scale), 0);
        moveCamera(lat, lon, z, animate === false ? 0 : cameraDuration);
    }
    function zoomAt(x, y, delta) {
        following = false;
        userMovedCamera = true;
        const oldSize = worldSize(zoom);
        const anchorX = (projX(centerLon, zoom) + x - width / 2) / oldSize;
        const anchorY = (projY(centerLat, zoom) + y - height / 2) / oldSize;
        const z = clampZoom(zoom + delta);
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

    /// 当前应绘制的瓦片集合，存**绝对瓦片坐标 + 层级 tz**。用 ListModel + 增量增删：
    /// 平移跨过瓦片边界时只增删边缘一圈，已有瓦片的 delegate 不重建、source 不变，不会整屏闪黑重载。
    /// 缩放跨层级时**保留旧层作为底衬**（underlayZ），新层加载完再移除，避免整层清空造成闪屏。
    ListModel { id: tileModel }
    /// 顶层（当前层）的缩放级别；-1 表示尚未初始化。
    property int tileModelZoom: -1
    /// 过渡期保留的旧层级别；-1 表示没有底衬。
    property int underlayZ: -1
    Timer {
        id: retireTimer
        interval: 1400
        onTriggered: root.retireUnderlay()
    }
    onTileZoomChanged: {
        // 旧顶层降级为底衬；更早的底衬在 syncTiles 里被丢弃，最多保留两层。
        root.underlayZ = root.tileModelZoom < 0 ? -1 : root.tileModelZoom;
        root.tileModelZoom = root.tileZoom;
        retireTimer.restart();
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
        const u = root.underlayZ;
        if (u < 0) return;
        for (let i = tileModel.count - 1; i >= 0; --i) {
            if (tileModel.get(i).tz === u) tileModel.remove(i);
        }
        root.underlayZ = -1;
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
        else if (basemap === "amap_satellite") templateUrl = "https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
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
            x: tx * size - root.originX
            y: ty * size - root.originY
            width: size + 1
            height: size + 1
            Image {
                anchors.fill: parent
                source: root.tileUrl(root.wrapTile(tileCell.tx, tileCell.side), tileCell.ty, tileCell.tz)
                sourceSize.width: root.tileSize
                sourceSize.height: root.tileSize
                asynchronous: true
                cache: true
                smooth: true
            }
        }
    }

    // P/S 波前圆：半径由 AppController 反解走时表得到（km），-1 表示不画。
    Rectangle {
        id: pWaveRing
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.pKm : -1
        visible: radiusKm > 0
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: Math.max(2, 2 * Math.max(0, radiusKm) * root.pxPerKmValue)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: root.theme.pWave
    }

    Rectangle {
        id: sWaveRing
        readonly property real radiusKm: root.hasEvent && root.hasFocus ? app.waveRadii.sKm : -1
        visible: radiusKm > 0
        x: root.hypocenterX - width / 2
        y: root.hypocenterY - height / 2
        width: Math.max(2, 2 * Math.max(0, radiusKm) * root.pxPerKmValue)
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

    // 震中距离参考同心圆环 (50km, 100km, 200km, 300km)
    Repeater {
        model: [50, 100, 200, 300]
        delegate: Rectangle {
            required property int modelData
            visible: root.hasEvent
            readonly property real r: modelData * root.pxPerKmValue
            x: root.hypocenterX - r
            y: root.hypocenterY - r
            width: r * 2
            height: r * 2
            radius: r
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.15)
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
        onWheel: (wheel) => {
            const delta = wheel.angleDelta.y / 120;
            root.zoomAt(wheel.x, wheel.y, delta * 0.5);
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
                    const list = ["amap_vector", "amap_satellite", "osm"];
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
