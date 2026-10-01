#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QStandardPaths>

#include "app/app_controller.h"
#include "service/tray_controller.h"
#include "theme/native_ui.h"

namespace {

/// 高德瓦片 CDN 会在 HTTP/2 连接中途回 GOAWAY，导致 QML Image 瓦片成片失败
/// （日志：stream error "Received GOAWAY" / "Remote host signaled shutdown"）。
/// HTTP/1.1 没有 GOAWAY 帧，且 Qt 每主机连接池天然限流，因此 QML 侧统一降到 1.1；
/// 同时补上 User-Agent（部分瓦片 CDN 依据它放行）。
class TileNetworkAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override {
        QNetworkRequest tuned = request;
        tuned.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        if (!tuned.hasRawHeader(QByteArrayLiteral("User-Agent"))) {
            tuned.setHeader(QNetworkRequest::UserAgentHeader,
                            QStringLiteral("komiraquake/2.0 (+https://api.wolfx.jp/)"));
        }
        return QNetworkAccessManager::createRequest(op, tuned, outgoingData);
    }
};

class TileNetworkAccessManagerFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager* create(QObject* parent) override {
        return new TileNetworkAccessManager(parent);
    }
};

/// 给 QML Image 用的网络管理器：强制 HTTP/1.1 + User-Agent，并挂上磁盘缓存
/// （瓦片重复区域/重启后不再重新下载）。Image 走引擎的 QNetworkAccessManager。
/// 注意：工厂必须在首次 networkAccessManager() 之前设置，引擎才会用它创建 NAM。
void installNetworkAccessManager(QQmlApplicationEngine& engine) {
    engine.setNetworkAccessManagerFactory(new TileNetworkAccessManagerFactory);
    QNetworkAccessManager* nam = engine.networkAccessManager();
    if (!nam) return;
    auto* cache = new QNetworkDiskCache(&engine);
    cache->setCacheDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tiles"));
    cache->setMaximumCacheSize(64LL * 1024 * 1024);
    nam->setCache(cache);
}
} // namespace

int main(int argc, char* argv[]) {
    // 使用 QApplication（Qt Widgets），以便 QSystemTrayIcon / QMenu 可用。
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QCoreApplication::setOrganizationName(QStringLiteral("KomiraQuake"));
    QCoreApplication::setApplicationName(QStringLiteral("KomiraQuake"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("KomiraQuake - 地震预警"));

    komira::AppController controller;

    QQmlApplicationEngine engine;
    komira::configureNativeUi(engine);
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    installNetworkAccessManager(engine);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("KomiraQuake", "Main");
    if (engine.rootObjects().isEmpty()) return -1;

    komira::TrayController tray(&controller);
    QObject::connect(&tray, &komira::TrayController::showWindowRequested, &app, [&engine]() {
        const QList<QObject*> roots = engine.rootObjects();
        if (roots.isEmpty()) return;
        if (auto* window = qobject_cast<QQuickWindow*>(roots.first())) {
            window->show();
            window->raise();
            window->requestActivate();
        }
    });

    return app.exec();
}
