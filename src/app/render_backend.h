#pragma once

#include <QString>

#include "core/render_backend_policy.h"

class QQmlApplicationEngine;

namespace komira {

/// 覆盖开关的环境变量名。值为 `software` / `opengl`（大小写不敏感）。
constexpr const char* kRenderBackendEnv = "KOMIRA_RENDER_BACKEND";

/// 探测当前平台插件能否真的创建 GL 上下文。以下情况都会失败：
/// Wayland 会话里没有 EGL、驱动与打包时的 glibc ABI 不匹配（NixOS 上
/// `libEGL_mesa` 因 `GLIBC_2.43 not found` 加载失败）、远程 / 无 GPU 会话。
///
/// 必须在 QGuiApplication 构造之后、场景图初始化之前调用。探测用的上下文与
/// 离屏表面在同一函数里销毁，不影响后续渲染。
bool probeOpenGl() noexcept;

/// 决策后端并下发到 QQuickWindow。必须在 QML 引擎加载之前调用。
/// 退到软件渲染时打一行日志解释原因。
RenderBackend configureRenderBackend() noexcept;

/// 兜底：探测用的是离屏表面，窗口要的是可见表面 —— 二者仍可能不一致。
/// Qt 在**没有**槽连着 QQuickWindow::sceneGraphError 时直接 qFatal，
/// 所以引擎创建后必须先挂上这个：报给 Sentry，再安静退出（exit 1），
/// 而不是让 SIGABRT 把整个进程带走。
void attachSceneGraphErrorHandler(QQmlApplicationEngine* engine);

/// 已挂上兜底的窗口数。测试用它确认「真的挂上了」—— 只连不看的话，
/// 少挂一个窗口就等于回到 qFatal。
int sceneGraphErrorGuardCount();

} // namespace komira
