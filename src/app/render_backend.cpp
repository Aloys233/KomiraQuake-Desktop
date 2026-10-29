#include "app/render_backend.h"

#include "service/sentry_service.h"

#include <QDebug>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>

#include <cstring>

namespace komira {

namespace {
int s_guardedWindows = 0;

RenderBackendOverride overrideFromEnvironment() {
    const QByteArray raw = qgetenv(kRenderBackendEnv);
    return parseRenderBackendOverride(raw.constData(), int(raw.size()));
}

/// Qt 在哪些平台上默认就用 OpenGL 作 RHI 后端。Windows 默认 D3D11、macOS 默认
/// Metal —— 那两条路上「GL 不可用」不代表场景图不可用，此时既不该探测也不该
/// 改后端，交给 Qt 自己选。
constexpr bool platformDefaultsToOpenGl() noexcept {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return false;
#else
    return true;
#endif
}
} // namespace

bool probeOpenGl() noexcept {
    // QOpenGLContext::create() 问的就是当前平台集成。
    // 已知局限：offscreen / minimal 平台在无 GL 时也可能「成功」，但生产构建
    // 走真实的 wayland / xcb 插件，那里 create() 失败就是真失败。
    QOpenGLContext context;
    if (!context.create()) return false;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid()) return false;
    const bool current = context.makeCurrent(&surface);
    if (current) context.doneCurrent();
    return current;
}

RenderBackend configureRenderBackend() noexcept {
    RenderBackend backend = RenderBackend::Hardware;
    switch (overrideFromEnvironment()) {
    case RenderBackendOverride::Software:
        backend = RenderBackend::Software;
        qInfo("[render] backend: software（由 %s 强制）", kRenderBackendEnv);
        break;
    case RenderBackendOverride::OpenGl:
        backend = RenderBackend::Hardware;
        qInfo("[render] backend: opengl（由 %s 强制）", kRenderBackendEnv);
        break;
    case RenderBackendOverride::Auto:
        if (!platformDefaultsToOpenGl()) {
            qInfo("[render] backend: 交给 Qt 默认（本平台不用 OpenGL RHI）");
            return RenderBackend::Hardware;
        }
        if (probeOpenGl()) {
            qInfo("[render] backend: opengl");
        } else {
            // 不探测就直接交给场景图，Qt 会在 handleContextCreationFailure()
            // 里 qFatal：进程 SIGABRT，数据源、历史库、托盘、预警一起死。
            backend = RenderBackend::Software;
            qWarning("[render] OpenGL 不可用（无 EGL / 驱动与打包时的 glibc ABI 不匹配"
                     " / 无 GPU）—— 退到软件渲染，地图与测站层走软件绘制分支");
        }
        break;
    }

    // 必须在 QML 加载前设好：QQuickWindow 首次渲染时才读这个值。
    QQuickWindow::setGraphicsApi(backend == RenderBackend::Software
                                     ? QSGRendererInterface::Software
                                     : QSGRendererInterface::OpenGL);
    return backend;
}

int sceneGraphErrorGuardCount() {
    return s_guardedWindows;
}

void attachSceneGraphErrorHandler(QQmlApplicationEngine* engine) {
    if (!engine) return;

    const auto attach = [](QObject* object) {
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) return;
        // 每个窗口只挂一次：attach 可能在 objectCreated 与 rootObjects() 两个
        // 时机各跑一遍，重复连接会让同一个错误被上报两次。
        if (window->property("_komiraSceneGraphGuarded").toBool()) return;
        window->setProperty("_komiraSceneGraphGuarded", true);
        ++s_guardedWindows;
        QObject::connect(
            window, &QQuickWindow::sceneGraphError, window,
            [](QQuickWindow::SceneGraphError error, const QString& message) {
                // 到这里说明可见表面仍然建不出场景图 —— 探测说「能用」但实际
                // 不能用。绝不能让它落回 qFatal/SIGABRT：留证据，安静退出。
                SentryService::instance()->captureMessage(
                    QStringLiteral("scene graph error: %1").arg(message),
                    QStringLiteral("fatal"),
                    {{QStringLiteral("scene_graph_error"), int(error)}});
                qCritical("[render] scene graph 初始化失败（code=%d）：%s",
                          int(error), qPrintable(message));
                QCoreApplication::exit(1);
            });
    };

    QObject::connect(
        engine, &QQmlApplicationEngine::objectCreated, engine,
        [attach](QObject* object, const QUrl&) { attach(object); }, Qt::DirectConnection);

    // 引擎可能已经加载完（顺序变化时也不能漏挂）。
    for (QObject* root : engine->rootObjects()) attach(root);
}

} // namespace komira
