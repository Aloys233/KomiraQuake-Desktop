#pragma once

namespace komira {

/// 场景图后端。Qt 6 的 RHI 拿不到可用 GL 上下文时走
/// `qFatal("Failed to initialize graphics backend for OpenGL.")` → SIGABRT，
/// 整个进程一起死：数据源、历史库、托盘、预警逻辑统统陪葬 —— 而这些没有一个
/// 需要 GPU。故只有两档后端，拿不到 GL 就退软件渲染。
enum class RenderBackend { Hardware, Software };

/// 启动期决策（纯函数、无 Qt 依赖）：拿不到 GL 就退软件渲染。
constexpr RenderBackend chooseRenderBackend(bool openGlUsable) noexcept {
    return openGlUsable ? RenderBackend::Hardware : RenderBackend::Software;
}

constexpr const char* renderBackendName(RenderBackend backend) noexcept {
    return backend == RenderBackend::Software ? "software" : "opengl";
}

/// 覆盖开关 `KOMIRA_RENDER_BACKEND` 的取值：0 = 未指定（自动探测），
/// 1 = 强制软件，2 = 强制 OpenGL。其它字符串一概按未指定处理，绝不让拼错的
/// 环境变量把用户锁死在一条坏路径上。
enum class RenderBackendOverride { Auto, Software, OpenGl };

constexpr RenderBackendOverride parseRenderBackendOverride(const char* value,
                                                          int length) noexcept {
    if (!value || length <= 0) return RenderBackendOverride::Auto;
    // "software"
    constexpr char kSoftware[] = "software";
    constexpr int kSoftwareLen = sizeof(kSoftware) - 1;
    if (length == kSoftwareLen) {
        bool same = true;
        for (int i = 0; i < length; ++i) {
            const char c = value[i];
            const char folded = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
            if (folded != kSoftware[i]) { same = false; break; }
        }
        if (same) return RenderBackendOverride::Software;
    }
    // "opengl"
    constexpr char kOpenGl[] = "opengl";
    constexpr int kOpenGlLen = sizeof(kOpenGl) - 1;
    if (length == kOpenGlLen) {
        bool same = true;
        for (int i = 0; i < length; ++i) {
            const char c = value[i];
            const char folded = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
            if (folded != kOpenGl[i]) { same = false; break; }
        }
        if (same) return RenderBackendOverride::OpenGl;
    }
    return RenderBackendOverride::Auto;
}

} // namespace komira
