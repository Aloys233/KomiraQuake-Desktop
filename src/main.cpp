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
#include "app/version.h"
#include "service/tray_controller.h"
#include "theme/native_ui.h"

namespace {

/// 瓦片网络请求的调优：
/// - 高德瓦片 CDN 会在 HTTP/2 连接中途回 GOAWAY，导致 QML Image 瓦片成片失败
///   （日志：stream error "Received GOAWAY"）。HTTP/1.1 没有 GOAWAY 帧，且 Qt 每主机
///   连接池天然限流，因此 QML 侧统一降到 1.1；同时补 User-Agent（部分 CDN 据此放行）。
/// - 强制 Accept-Encoding: identity。Qt 对需要解压的响应（Content-Encoding: gzip/br）
///   会跳过磁盘缓存：QNetworkDiskCache::prepare 已生成有效元数据，但 completeCacheSave
///   从不 insert，于是瓦片永远不入缓存 —— 每次缩放换 z 即新 URL，全部重新下载并闪白。
///   瓦片本身是已压缩位图，关闭传输压缩几乎无损，却让磁盘缓存恢复正常。
class TileNetworkAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override {
        QNetworkRequest tuned = request;
        tuned.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        tuned.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
        if (!tuned.hasRawHeader(QByteArrayLiteral("User-Agent"))) {
            tuned.setHeader(QNetworkRequest::UserAgentHeader,
                            QStringLiteral("komiraquake/2.0 (+https://api.wolfx.jp/)"));
        }
        return QNetworkAccessManager::createRequest(op, tuned, outgoingData);
    }
};

/// QML 栅格瓦片由 QQuickPixmap 经 **工厂新建的** QNetworkAccessManager 加载
/// （QQmlTypeLoader::createNetworkAccessManager → factory->create()），而不是
/// engine.networkAccessManager()。因此磁盘缓存必须在工厂里挂到每个新建的 NAM 上，
/// 否则瓦片根本不入缓存：每次缩放换 z 即新 URL，全部重新下载并闪白。
/// `setCache` 会接管 cache 的所有权，故每个 NAM 各配一个 QNetworkDiskCache。
class TileNetworkAccessManagerFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager* create(QObject* parent) override {
        auto* nam = new TileNetworkAccessManager(parent);
        auto* cache = new QNetworkDiskCache(nam);
        cache->setCacheDirectory(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tiles"));
        cache->setMaximumCacheSize(64LL * 1024 * 1024);
        nam->setCache(cache);
        return nam;
    }
};
} // namespace

int main(int argc, char* argv[]) {
    // 使用 QApplication（Qt Widgets），以便 QSystemTrayIcon / QMenu 可用。
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QCoreApplication::setOrganizationName(QStringLiteral("KomiraQuake"));
    QCoreApplication::setApplicationName(QStringLiteral("KomiraQuake"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KOMIRA_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("KomiraQuake - 地震预警"));

    komira::AppController controller;

    QQmlApplicationEngine engine;
    komira::configureNativeUi(engine);
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.setNetworkAccessManagerFactory(new TileNetworkAccessManagerFactory);

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
