import QtQuick
import QtQuick.Controls

// JSON 查看器：富文本语法高亮 + 双向滚动。只在详情页的原始报文弹窗里使用。
// 高亮在 QML 里做而不是引入 QSyntaxHighlighter：报文是只读的一次性渲染，
// 不需要文档/光标模型；`<span>` 着色足够，也不会因大文档阻塞 GUI。
Item {
    id: root
    property Theme theme: Theme {}
    property string text: ""

    readonly property color keyColor: theme.dark ? "#7ADBD8" : "#006874"
    readonly property color stringColor: theme.dark ? "#A5D6A7" : "#2E7D32"
    readonly property color numberColor: theme.dark ? "#FFB74D" : "#B45F06"
    readonly property color keywordColor: theme.dark ? "#CE93D8" : "#7B1FA2"
    readonly property color punctColor: theme.outline

    // 换一份报文时回到顶部：详情页里同一个查看器会一直复用。
    onTextChanged: {
        flick.contentX = 0
        flick.contentY = 0
    }

    function esc(value) {
        return String(value)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
    }

    // 富文本会折叠连续空格与换行，逐字符换成实体后才能保留缩进。
    function plain(value) {
        return esc(value)
            .replace(/ /g, "&nbsp;")
            .replace(/\t/g, "&nbsp;&nbsp;&nbsp;&nbsp;")
            .replace(/\n/g, "<br/>")
    }

    // 中文字符必须单独换字体：TextEdit 的基字体是 JetBrains Mono，
    // 其缺失字形会落到系统等宽 CJK，而不是应用内嵌的思源黑体。
    function isCjkChar(ch) {
        const code = ch.charCodeAt(0)
        return (code >= 0x2E80 && code <= 0x2FFF)      // CJK 部首/标点
            || (code >= 0x3000 && code <= 0x30FF)      // CJK 标点、假名
            || (code >= 0x3400 && code <= 0x4DBF)      // 扩展 A
            || (code >= 0x4E00 && code <= 0x9FFF)      // 基本汉字
            || (code >= 0xAC00 && code <= 0xD7AF)      // 谚文
            || (code >= 0xF900 && code <= 0xFAFF)      // 兼容汉字
            || (code >= 0xFF00 && code <= 0xFFEF)      // 全角
    }

    function spanRun(color, value, cjk) {
        const family = cjk ? ";font-family:'" + root.theme.hanFamily + "'" : ""
        return "<span style=\"color:" + color + family + "\">" + plain(value) + "</span>"
    }

    // 按 CJK / 非 CJK 切段着色：非 CJK 继承 JetBrains Mono，中文强制思源黑体。
    function paint(color, value) {
        if (!value) return ""
        let out = ""
        let start = 0
        let cjk = isCjkChar(value.charAt(0))
        for (let i = 1; i < value.length; ++i) {
            const next = isCjkChar(value.charAt(i))
            if (next === cjk) continue
            out += spanRun(color, value.substring(start, i), cjk)
            start = i
            cjk = next
        }
        return out + spanRun(color, value.substring(start), cjk)
    }

    readonly property string highlighted: {
        const raw = root.text
        if (!raw) return ""
        const token = /("(?:\\.|[^"\\])*")(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b|([{}\[\],:])/g
        let out = ""
        let last = 0
        let match
        while ((match = token.exec(raw)) !== null) {
            out += paint(root.theme.textPrimary, raw.substring(last, match.index))
            if (match[1] !== undefined) {
                if (match[2] !== undefined)
                    out += paint(keyColor, match[1]) + paint(punctColor, match[2])
                else
                    out += paint(stringColor, match[1])
            } else if (match[3] !== undefined) {
                out += paint(numberColor, match[3])
            } else if (match[4] !== undefined) {
                out += paint(keywordColor, match[4])
            } else if (match[5] !== undefined) {
                out += paint(punctColor, match[5])
            }
            last = token.lastIndex
        }
        out += paint(root.theme.textPrimary, raw.substring(last))
        return out
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: editor.width
        contentHeight: editor.height
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickIfNeeded
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

        TextEdit {
            id: editor
            width: Math.max(flick.width, implicitWidth)
            height: implicitHeight
            text: root.highlighted
            textFormat: TextEdit.RichText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            color: root.theme.textPrimary
            selectionColor: root.theme.accentContainer
            selectedTextColor: root.theme.textPrimary
            font.family: root.theme.monoFamily
            font.pixelSize: 11
            font.letterSpacing: 0.1
        }
    }
}
