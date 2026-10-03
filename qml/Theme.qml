import QtQuick

// Shared material / type tokens; seismic colors remain the §9 contract.
QtObject {
    id: theme
    readonly property bool dark: app ? app.darkMode : false
    readonly property bool reduceMotion: app && app.settings ? app.settings.reduceMotion : false
    readonly property bool backgroundBlur: app && app.settings ? app.settings.backgroundBlur : true
    readonly property int motionDuration: reduceMotion ? 0 : 150
    readonly property string fontFamily: "Noto Sans CJK SC"
    // 数字与正文同族：字体默认就是等宽数字（tabular figures），无需换成 monospace，
    // 否则「M 5.6」这类数字会与旁边的说明文字不同字体、看起来不对齐。
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
    readonly property color backdropTint: dark ? "#99141D20" : "#ADF8FCFC"
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
    /// 中国地震烈度色阶（对齐 kanameishi CSIS 配色）。
    function intensityColor(raw) {
        const level = Math.round(raw);
        switch (level) {
        case 1: return "#9F9F9F";
        case 2: return "#CFCFCF";
        case 3: return "#5FCFFF";
        case 4: return "#3FAFFF";
        case 5: return "#5FDF8F";
        case 6: return "#F7E757";
        case 7: return "#FF8F00";
        case 8: return "#FF4F00";
        case 9: return "#DF0F0F";
        default: return level >= 10 ? "#7F007F" : "#9F9F9F";
        }
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
