import QtQuick
import QtQuick.Effects

// Capture ONLY the supplied map render layer. Never capture an ancestor containing
// this panel, its foreground, or other effects. Padded samples avoid dark blur edges;
// the separate alpha mask supplies real rounded clipping (Item.clip is rectangular).
Item {
    id: root
    objectName: "backdropBlur"
    required property Item sourceItem
    property real cornerRadius: 16
    /// 背景是否逐帧重采样。live=true 会让 SceneGraph 永不空闲：实测窗口可见但静止时
    /// 仍以 ~86 fps 渲染、吃掉约 25% CPU（每个玻璃卡片都在重绘整棵地图子树）。
    /// 相机移动时才置 true；静止时由下面的定时器低频补帧。
    property bool live: true
    readonly property int blurRadius: 64
    // Keep the sample margin at least as wide as the full blur kernel.
    readonly property real padding: blurRadius
    clip: true

    // mapToItem() itself has no QML dependency notifications. Read each chain's
    // geometry inputs so the binding invalidates on layout, reparenting, or source
    // movement, without a frame timer or a recursive capture. The two chains need
    // not share a parent (Main's HUD and MapView's toolbar do not).
    //
    // Item.transform is deliberately NOT read: it is a QQmlListProperty with no
    // NOTIFY signal, so it is non-bindable. Reading it can never invalidate the
    // binding and only makes QQmlExpression log "depends on non-bindable
    // properties" on every update. No item in this app uses the transform list,
    // and the bindable inputs below cover layout/scale/rotation changes.
    function transformStamp(item) {
        let stamp = "";
        while (item) {
            stamp += [item.x, item.y, item.width, item.height, item.scale,
                      item.rotation, item.transformOrigin].join(",");
            item = item.parent;
        }
        return stamp;
    }

    ShaderEffectSource {
        id: sample
        objectName: "backdropSample"
        sourceItem: root.sourceItem
        sourceRect: {
            const w = root.width, h = root.height;
            const geometry = root.transformStamp(root) + root.transformStamp(root.sourceItem);
            return root.mapToItem(root.sourceItem,
                                  Qt.rect(-root.padding, -root.padding,
                                          w + 2 * root.padding, h + 2 * root.padding));
        }
        textureSize: Qt.size(Math.ceil((root.width + 2 * root.padding) / 2),
                             Math.ceil((root.height + 2 * root.padding) / 2))
        width: root.width + 2 * root.padding
        height: root.height + 2 * root.padding
        live: root.live && (Window.window ? Window.window.visible : true)
        recursive: false
        hideSource: false
        visible: false
        smooth: true
    }
    // 静止时每 500 ms 补一帧，兼顾「不持续渲染」与「数据/瓦片更新后背景不过期」。
    Timer {
        objectName: "backdropRefresh"
        interval: 500
        repeat: true
        // Timer 的作用域对象不是 Item，必须显式从 root 取附着的 Window，
        // 否则 Window.window 会报 “only supports types derived from Item”。
        running: !root.live && root.visible
                 && (root.Window.window ? root.Window.window.visible : true)
        onTriggered: sample.scheduleUpdate()
    }
    Item {
        id: mask
        width: sample.width
        height: sample.height
        visible: false
        layer.enabled: true
        layer.smooth: true
        Rectangle {
            x: root.padding; y: root.padding
            width: root.width; height: root.height
            radius: root.cornerRadius
            color: "white"
            antialiasing: true
        }
    }
    MultiEffect {
        objectName: "backdropEffect"
        x: -root.padding; y: -root.padding
        width: sample.width; height: sample.height
        source: sample
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: root.blurRadius
        blur: 1.0
        maskEnabled: true
        maskSource: mask
        maskThresholdMin: 0.0
        maskSpreadAtMin: 0.0
    }
}
