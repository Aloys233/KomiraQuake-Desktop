import QtQuick
import QtQuick.Layouts

// 「启动与更新」分区。
Column {
    id: root
    objectName: "settingsPanel-about"
    property Theme theme: Theme {}
    signal notify(string message, bool success)
    width: parent.width
    spacing: 20

    SettingsSection {
        width: parent.width; theme: root.theme; title: "启动"; iconName: "activity"
        GlassSwitch {
            objectName: "autoStartSwitch"
            width: parent.width; theme: root.theme; text: "开机自启"
            checked: app.autoStart.enabled
            onToggled: app.autoStart.enabled = checked
        }
        GlassSwitch {
            objectName: "silentStartSwitch"
            width: parent.width; theme: root.theme; text: "静默启动"
            description: "启动时只显示托盘图标。"
            checked: app.settings.silentStart
            onToggled: app.settings.silentStart = checked
        }
        GlassSwitch {
            objectName: "closeToTraySwitch"
            width: parent.width; theme: root.theme; text: "关闭窗口时收进托盘"
            description: "关闭后继续在后台运行。"
            checked: app.settings.closeToTray
            onToggled: app.settings.closeToTray = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "开发者"; iconName: "sliders-horizontal"
        GlassSwitch {
            id: devModeToggle
            objectName: "devModeSwitch"
            width: parent.width; theme: root.theme; text: "开发者模式"
            description: "显示模拟数据源与尚未标定的推算区域。"
            checked: false
            // 与「震源推算」分区同一条纪律：用户点按会断开字面 `checked:` 绑定，
            // 之后设置再变开关就不动了。Binding + RestoreBindingOrValue 保证反向同步。
            Binding {
                target: devModeToggle
                property: "checked"
                value: app.settings.developerMode
                restoreMode: Binding.RestoreBindingOrValue
            }
            onToggled: app.settings.developerMode = checked
        }
    }

    SettingsSection {
        width: parent.width; theme: root.theme; title: "版本"; iconName: "refresh-cw"
        RowLayout {
            width: parent.width; spacing: 12
            Text {
                Layout.fillWidth: true
                text: "当前版本 v" + app.updater.currentVersion
                color: root.theme.textPrimary; font.pixelSize: 14
                font.family: root.theme.numberFamily
            }
            GlassButton {
                objectName: "checkUpdatesButton"
                theme: root.theme; text: "检查更新"; iconName: "refresh-cw"
                enabled: app.updater.state !== "checking"
                onClicked: app.updater.check(false)
            }
            GlassButton {
                objectName: "openReleaseButton"
                visible: app.updater.state === "updateAvailable"
                theme: root.theme; text: "打开下载页"; iconName: "external-link"; primary: true
                onClicked: app.updater.openReleasePage()
            }
        }
        Text {
            width: parent.width; visible: app.updater.message !== ""
            text: app.updater.message; wrapMode: Text.Wrap; font.pixelSize: 12
            color: app.updater.state === "updateAvailable" ? root.theme.accent
                 : app.updater.state === "error" ? root.theme.severity("WARNING")
                 : root.theme.outline
        }
        // 走的是哪条通道：只在真的经加速代理时出现。直连不写「直连」——那是默认状态，
        // 每行都多一句「经直连」等于噪声；而「经 gh-proxy.org」是用户需要知道的
        // （他可能已经忘了自己选过代理，也可能正是靠这条确认代理生效）。
        Text {
            objectName: "updateRouteLabel"
            width: parent.width
            visible: app.updater.usedProxy && app.updater.routeLabel !== ""
            text: "更新通道 · " + app.updater.routeLabel
            color: root.theme.outline; font.pixelSize: 11; wrapMode: Text.Wrap
        }
        // 安装包直链：加速代理通常**只放行资源下载、不放行网页**（实测 gh-proxy 对其
        // releases 页面返回 403「Web page content is not allowed」），所以「打开下载页」
        // 在代理通道下会失败，这里直接给文件链接。
        Column {
            width: parent.width; spacing: 8
            visible: app.updater.state === "updateAvailable" && app.updater.assets.length > 0
            Text {
                text: "安装包"
                color: root.theme.outline; font.pixelSize: 12
            }
            Repeater {
                model: app.updater.assets
                GlassButton {
                    required property var modelData
                    objectName: "updateAsset-" + modelData.name
                    theme: root.theme
                    width: parent.width
                    customRadius: 10
                    primary: modelData.recommended
                    text: modelData.name + (modelData.sizeText.length ? " · " + modelData.sizeText : "")
                    iconName: "download"
                    accessibleName: "下载 " + modelData.name
                    onClicked: app.updater.openUrl(modelData.downloadUrl)
                }
            }
        }
        // 仓库入口：侧栏底部那份在窄屏（无侧栏）时看不见，这里再给一份。
        // 桌面端看起来重复，但设置页的分区本来就是「所有能操作的东西都在面板里」。
        RowLayout {
            width: parent.width; spacing: 8
            AppIcon { name: "github"; size: 16; color: root.theme.outline }
            Text {
                Layout.fillWidth: true
                text: "项目主页"
                color: root.theme.textPrimary; font.pixelSize: 13
            }
            GlassButton {
                objectName: "openRepositoryButton"
                theme: root.theme; text: "GitHub"; iconName: "external-link"; flat: true
                Accessible.name: "在浏览器中打开项目仓库 " + app.updater.repositoryUrl
                onClicked: app.openExternalUrl(app.updater.repositoryUrl)
            }
        }
        Text {
            width: parent.width
            text: app.updater.repositoryUrl
            color: root.theme.outline; font.pixelSize: 11
            font.family: root.theme.monoFamily
            elide: Text.ElideMiddle
            Accessible.ignored: true
        }
        GlassSwitch {
            objectName: "autoCheckUpdatesSwitch"
            width: parent.width; theme: root.theme; text: "自动检查更新"
            checked: app.settings.autoCheckUpdates
            onToggled: app.settings.autoCheckUpdates = checked
        }
    }

    // ---- 更新通道 ------------------------------------------------------------
    // GitHub 在部分网络下不可达，所以检查更新允许走加速代理。默认「自动」：直连优先，
    // 失败再依次试内置代理 —— 用户什么都不用配；需要固定线路（或自建镜像）时才动这里。
    SettingsSection {
        width: parent.width; theme: root.theme; title: "更新通道"; iconName: "globe"
        Column {
            width: parent.width; spacing: 8
            Text {
                text: "GitHub 访问方式"
                color: root.theme.textPrimary; font.pixelSize: 14
            }
            GlassComboBox {
                id: channelCombo
                objectName: "updateChannelCombo"
                width: parent.width; theme: root.theme
                Accessible.name: "更新通道"
                readonly property var options: app.updater.channelOptions
                model: options.map(option => option.label)
                readonly property int selectedIndex: {
                    for (let i = 0; i < options.length; ++i) {
                        if (options[i].id === app.settings.updateChannel) return i;
                    }
                    return 0;
                }
                // Binding 而不是字面绑定：用户选择会写 currentIndex 从而断开字面绑定，
                // 之后「恢复默认」下拉框会停在旧值（与波前口径下拉同一个坑）。
                Binding {
                    target: channelCombo
                    property: "currentIndex"
                    value: channelCombo.selectedIndex
                    restoreMode: Binding.RestoreBindingOrValue
                }
                onActivated: index => {
                    app.settings.updateChannel = options[index].id;
                    // 选了「自定义」却还空着：当场提示一句，别等他点「检查更新」才发现没配。
                    if (options[index].id === "custom" && !app.settings.updateCustomProxy.length)
                        root.notify("已选择自定义代理，请填写代理地址", false);
                }
            }
            Text {
                width: parent.width
                text: "「自动」会先直连，失败后依次尝试内置加速代理；选具体代理则固定走那一条。"
                wrapMode: Text.Wrap; lineHeight: 1.4
                color: root.theme.outline; font.pixelSize: 12
            }
        }

        // 自定义代理：只在选中「自定义」时可编辑，但输入框始终可见 —— 隐藏它会让
        // 「代理填错了想改」变成「先切通道、再改」，多一步且容易找不到。
        Column {
            width: parent.width; spacing: 6
            enabled: app.settings.updateChannel === "custom"
            opacity: enabled ? 1 : 0.55
            Text {
                text: "自定义加速代理"
                color: root.theme.textPrimary; font.pixelSize: 14
            }
            GlassTextField {
                id: customProxyField
                objectName: "customProxyField"
                width: parent.width; theme: root.theme
                placeholderText: "https://你的代理地址（可省略 https://）"
                Accessible.name: "自定义加速代理"
                // 字面绑定就够（与 ClockSection 的 NTP 输入框同一写法）：TextField 的
                // `text` 由 C++ 侧写入时**不会**断开 QML 绑定，所以「清除 / 恢复默认」
                // 之后输入框仍会跟着设置回落 —— 已用真实键盘输入验证过（用例
                // `settingsUpdateChannelAndRepositoryLink` 的最后一段）。
                // 下拉框不同：`currentIndex` 会被用户选择直接写掉，那里必须用
                // Binding + RestoreBindingOrValue，别把两种机制混着抄。
                text: app.settings.updateCustomProxy
                // 回车即应用：这类单行输入用户的习惯就是打完回车。
                onAccepted: applyProxyButton.clicked()
            }
            Text {
                width: parent.width
                text: "代理需支持 `<代理地址>/<完整 GitHub 链接>` 的拼接形式（与 gh-proxy 相同）。"
                wrapMode: Text.Wrap; lineHeight: 1.4
                color: root.theme.outline; font.pixelSize: 12
            }
            RowLayout {
                width: parent.width; spacing: 8
                GlassButton {
                    id: applyProxyButton
                    objectName: "applyCustomProxyButton"
                    theme: root.theme; text: "应用并检查"; iconName: "check"
                    enabled: app.settings.updateChannel === "custom"
                             && customProxyField.text.trim() !== app.settings.updateCustomProxy
                    onClicked: {
                        app.settings.updateCustomProxy = customProxyField.text.trim();
                        app.updater.check(false);
                    }
                }
                GlassButton {
                    objectName: "resetCustomProxyButton"
                    theme: root.theme; text: "清除"; flat: true; iconName: "x"
                    visible: app.settings.updateCustomProxy.length > 0
                    // 只改设置：输入框由上面的 Binding 跟着回落（不再手写一行 `text = ""`，
                    // 那样等于给「绑定是否真的接上了」留了个假通过的缝）。
                    onClicked: app.settings.updateCustomProxy = ""
                }
                Item { Layout.fillWidth: true }
            }
        }
    }
}
