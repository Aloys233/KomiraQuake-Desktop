import QtQuick

// Shared material / type tokens; seismic colors remain the §9 contract.
QtObject {
    id: theme
    readonly property bool dark: app ? app.darkMode : false
    readonly property bool reduceMotion: app && app.settings ? app.settings.reduceMotion : false
    readonly property bool backgroundBlur: app && app.settings ? app.settings.backgroundBlur : true
    readonly property int motionDuration: reduceMotion ? 0 : 150
    // 打包的 Google Sans（拉丁）；中文由应用字体的 families 回退到系统 CJK。
    readonly property string fontFamily: "Google Sans"
    // 数字与正文同族。Google Sans 默认是比例数字，应用字体已显式开启 tnum（等宽数字），
    // 因此无需换成 monospace，否则「M 5.6」这类数字会与旁边的说明文字不同字体、看起来不对齐。
    readonly property string numberFamily: Qt.application.font.family

    readonly property color normalLight: "#006874"
    readonly property color normalDark: "#4DDAD7"
    readonly property color watchLight: "#7A5900"
    readonly property color watchDark: "#FFBA28"
    readonly property color warningLight: "#BC2800"
    readonly property color warningDark: "#FF8C66"
    readonly property color criticalLight: "#BA1A1A"
    readonly property color criticalDark: "#FFB4AB"
    readonly property color pWave: "#0288D1"
    readonly property color sWave: "#E65100"
    readonly property color clockSynced: dark ? "#66BB6A" : "#2E7D32"
    readonly property color clockUnsynced: dark ? "#EF5350" : "#C62828"
    readonly property color hypocenterHalo: "#FFF1AA"
    readonly property color hypocenterCross: "#E21D1D"

    readonly property color surface: dark ? "#101719" : "#F4F7F7"
    readonly property color surfaceContainerLow: dark ? "#161E20" : "#FBFDFD"
    readonly property color surfaceContainer: dark ? "#1C2527" : "#EAEFF0"
    readonly property color surfaceContainerHigh: dark ? "#293335" : "#E1E8E9"
    readonly property color textPrimary: dark ? "#E5EBEC" : "#191F21"
    readonly property color outline: dark ? "#A5B3B6" : "#58696C"
    readonly property color outlineVariant: dark ? "#47575A" : "#BAC8CB"

    // Clean, opaque nested surfaces. Only backdrop-aware panels use the tint.
    readonly property color glassCard: surfaceContainerLow
    readonly property color glassCardHover: dark ? "#243033" : "#EAF1F2"
    readonly property color glassCardPressed: dark ? "#2B3B3E" : "#DDE9EB"
    readonly property color glassBorder: dark ? "#354548" : "#D4DFE1"
    readonly property color glassBorderHover: dark ? "#64777B" : "#A5B9BE"
    readonly property color glassHeader: surfaceContainerLow
    readonly property color glassInput: dark ? "#10191B" : "#F1F5F6"
    readonly property color glassShadow: dark ? "#50000000" : "#160C252A"
    readonly property color backdropTint: dark ? "#80141D20" : "#94F8FCFC"
    readonly property color backdropFallback: surfaceContainerLow
    readonly property color accent: dark ? "#4DDAD7" : "#006874"
    readonly property color accentHover: dark ? "#7BE5E2" : "#005763"
    readonly property color accentForeground: dark ? "#073637" : "#FFFFFF"
    readonly property color accentContainer: dark ? "#213F42" : "#DDEFF0"
    readonly property color accentBorder: dark ? "#3C797D" : "#98C6CA"

    function severity(tag) {
        switch (tag) {
        case "CRITICAL": return dark ? criticalDark : criticalLight;
        case "WARNING": return dark ? warningDark : warningLight;
        case "WATCH": return dark ? watchDark : watchLight;
        default: return dark ? normalDark : normalLight;
        }
    }
    function container(tag) {
        const c = severity(tag);
        return Qt.rgba(c.r, c.g, c.b, 0.14);
    }
    /// 中国地震烈度色阶（对齐 kanameishi CSIS 配色），下标即烈度等级 0..12。
    /// 地图上的测站方标也直接消费这张表（StationMapItem.csisColors）。
    readonly property var csisColorBand: [
        "#9F9F9F", "#9F9F9F", "#CFCFCF", "#5FCFFF", "#3FAFFF", "#5FDF8F",
        "#F7E757", "#FF8F00", "#FF4F00", "#DF0F0F", "#7F007F", "#7F007F", "#7F007F"
    ]
    function intensityColor(raw) {
        const level = Math.round(raw);
        return csisColorBand[Math.max(0, Math.min(csisColorBand.length - 1, level))];
    }
    /// NIED 测站色阶（对齐 kanameishi `colorBand.nied`）：连续震度 → 深蓝→青→绿→黄→橙→红。
    /// 低值/缺测（<= -3，Jian 用 -3 表示"无观测/低于基线"）都落在最低的深蓝，形成常态蓝色测站网。
    readonly property var niedColorBand: [
        "#0003cf",
        "#0014da", "#0037f0", "#006cdc", "#00b3a2", "#12dc72",
        "#31f049", "#64fb2a", "#9dfe17", "#ccff09", "#ebff03",
        "#fff500", "#ffe500", "#ffca00", "#ffa600", "#ff7e00",
        "#ff5900", "#fd3500", "#f81100", "#e50000", "#bd0000"
    ]
    /// 连续震度 → 测站颜色。缺测（`raw <= -3`）取最低档深蓝（默认观感即"蓝点"）。
    function niedStationColor(raw) {
        const idx = Math.max(0, Math.min(20, Math.round((raw + 3) * 2)));
        return niedColorBand[idx];
    }
    /// KMA MMI 色阶（对齐 kanameishi `kmaIntColorBand`，下标 = MMI + 2，即 0..13）。
    /// MMI 0–3 为灰/浅灰；4 起天蓝→蓝→绿→黄→橙→深橙→红→紫。
    readonly property var kmaColorBand: [
        "#9F9F9F", "#9F9F9F", "#9F9F9F", "#9F9F9F", "#CFCFCF", "#5FCFFF", "#3FAFFF",
        "#5FDF8F", "#F7E757", "#FF8F00", "#FF4F00", "#DF0F0F", "#7F007F", "#7F007F"
    ]
    /// KMA 普通测站圆点色阶（'kmaColorBand.nied' 顺序）。该通道静稳基线是 MMI 1，
    /// 0/1 两档统一用与 NIED 缺测点相同的深蓝（`#0003cf`），保证两张测站网的静稳蓝一致；
    /// MMI ≥ 2 改用 `kmaColorBand` 方标。
    readonly property var kmaDotColorBand: [
        "#0003cf", "#0003cf", "#05d384", "#50fb30", "#ccff09", "#fdfc00", "#ffca00",
        "#ff7900", "#ff4700", "#f91900", "#e10000", "#af0000", "#ae0000", "#ad0000"
    ]
    /// KMA MMI → 颜色：MMI 0/1 是常态基线，取圆点色阶的蓝；MMI ≥ 2 取 `kmaColorBand`
    /// （下标 = MMI+2，对齐 kanameishi `kmaIntColorBand`）。
    function kmaStationColor(raw) {
        const mmi = Math.max(0, Math.min(11, Math.round(raw)));
        if (mmi < 2) return kmaDotColorBand[mmi];
        return kmaColorBand[Math.min(kmaColorBand.length - 1, mmi + 2)];
    }
    /// 卡片大号数字用的颜色：色阶原色在浅色卡上偏亮、深色卡上偏暗，
    /// 这里按主题做对比度校正（浅色卡压暗、深色卡提亮），保证数字可读。
    function stationTextColor(raw) {
        const base = niedStationColor(raw);
        return dark ? Qt.lighter(base, 1.45) : Qt.darker(base, 1.5);
    }
    /// 卡片大号数字用色（按数据源制式）：`csis` 用中国烈度色阶、`kma` 用韩国 MMI 色阶，
    /// 其余用 NIED 连续色阶；同样按主题做对比度校正，保证数字可读。
    function stationTextColorFor(raw, standard) {
        let base;
        if (standard === "csis") base = intensityColor(raw);
        else if (standard === "kma") base = kmaStationColor(raw);
        else base = niedStationColor(raw);
        return dark ? Qt.lighter(base, 1.45) : Qt.darker(base, 1.5);
    }
    function magnitudeColor(m) {
        if (m < 3.0) return "#00796B";
        if (m < 4.5) return "#F57F17";
        if (m < 6.0) return "#E64A19";
        return "#C2185B";
    }
    function on(bg) { return bg.hslLightness < 0.5 ? "#FFFFFF" : "#1A1C1E"; }

    /// 数据源连接状态用色（对齐 kanameishi）：在线绿 / 连接中黄 / 断开或异常红。
    function connectionColor(tag) {
        switch (tag) {
        case "CONNECTED": return clockSynced;
        case "CONNECTING": return severity("WATCH");
        default: return clockUnsynced;
        }
    }
}
