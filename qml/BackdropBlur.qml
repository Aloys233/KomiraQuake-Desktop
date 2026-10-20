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
    /// 模糊核半径。0 表示退化为「只做半透明底衬」。
    ///
    /// 恒为 64（全模糊），**不要**再按 `live` 降档。e0d8262（2026-10-19）曾把相机
    /// 移动时的 blurMax 降到 32、静止才回 64；用户实测反馈「拖动时模糊会关掉一会」——
    /// 且这不只是 32/64 的观感差异：blurRadius 同时决定 padding，padding 又参与
    /// sourceRect/textureSize，每次起停拖动都要重建采样纹理，重新分配的那几帧读不到
    /// 背景，看起来就是模糊被短暂关掉。后续 GPU 矢量层把拖动帧时从 81 ms 压到 22 ms，
    /// 这里已经没有省的必要。移动与静止的唯一区别只剩 `live`：是否逐帧重采样。
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
    // 逐项写入**预分配**数组再拼接：这个函数在相机移动时每帧对两条父链各求值一次，
    // 旧写法 `stamp += [..].join(",")` 每层都要新建一个数组、再拼一个新字符串。
    // 改用固定 7 元数组 + push 到同一个缓冲，仍保证「读到了这些属性」（依赖追踪靠读取，
    // 不靠结果），但每帧不再产生 N 个中间数组。
    readonly property var transformFields: new Array(7)
    function transformStamp(item) {
        let stamp = "";
        while (item) {
            const f = root.transformFields;
            f[0] = item.x; f[1] = item.y; f[2] = item.width; f[3] = item.height;
            f[4] = item.scale; f[5] = item.rotation; f[6] = item.transformOrigin;
            stamp += f.join(",");
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
