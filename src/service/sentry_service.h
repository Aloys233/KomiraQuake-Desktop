#pragma once

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QQmlApplicationEngine;

namespace komira {

/// Sentry 面包屑结构体
struct SentryBreadcrumb {
    QString timestamp; // ISO 8601 UTC
    QString category;  // 如 "qt.log", "qml", "network", "quake", "app"
    QString message;
    QString level;     // "debug", "info", "warning", "error", "fatal"
    QVariantMap data;
};

/// 跨平台轻量级原生 Sentry 客户端
/// 提供：崩溃捕获与启动自动上报、Qt 日志拦截、QML 错误监控、异常捕获与面包屑追踪。
/// 客户端恒定开启，不允许被禁用。
class SentryService : public QObject {
    Q_OBJECT

public:
    static SentryService* instance();

    /// 在应用启动的最早时刻调用（注册信号处理器、terminate 处理与日志挂钩）
    static void initializeEarly();

    /// 在 Qt 事件循环就绪后调用（绑定网络）
    void init(QNetworkAccessManager* nam = nullptr);

    /// 监听 QML 引擎的警告与错误（接受 QQmlApplicationEngine 或 QObject）
    void attachQmlEngine(QObject* engine);

    /// 添加面包屑记录
    void addBreadcrumb(const QString& category, const QString& message,
                       const QString& level = QStringLiteral("info"),
                       const QVariantMap& data = {});

    /// 主动上报一条消息事件
    void captureMessage(const QString& message, const QString& level = QStringLiteral("info"),
                        const QVariantMap& extra = {});

    /// 主动上报异常事件
    void captureException(const QString& type, const QString& value,
                          const QString& stacktrace = QString(),
                          const QString& level = QStringLiteral("error"),
                          const QVariantMap& extra = {});

    /// 发送测试报告
    bool sendTestReport();

    /// 检查并上报上次未完成的崩溃信息
    void checkAndSendPendingCrashes();

    /// 是否启用了 Sentry 上报
    bool isEnabled() const;

    /// 获取 DSN
    QString dsn() const { return dsn_; }

    /// 记录即时崩溃到磁盘（供 terminate 或崩溃处理器调用）
    void recordCrashImmediately(const QString& type, const QString& message, const QString& stacktrace = QString());

signals:
    void testReportSent(bool success, const QString& message);

private:
    explicit SentryService(QObject* parent = nullptr);
    ~SentryService() override;

    void parseDsn(const QString& dsn);
    void sendEnvelope(const QByteArray& envelopeData, const QString& eventId = QString(),
                      bool isTest = false);
    QByteArray buildEnvelope(const QJsonObject& event, const QString& eventId);
    QJsonObject buildEventBase(const QString& level, const QString& eventId);

    static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message);
    static void handleCrashSignal(int sig);
#ifdef Q_OS_WIN
    static long __stdcall winUnhandledExceptionFilter(void* exceptionPointers);
#endif

    QString dsn_;
    QString publicKey_;
    QString host_;
    QString projectId_;
    QString ingestUrl_;

    QNetworkAccessManager* nam_{nullptr};
    bool ownsNam_{false};

    mutable QMutex breadcrumbsMutex_;
    QList<SentryBreadcrumb> breadcrumbs_;
    static constexpr int maxBreadcrumbs_ = 100;

    mutable QMutex throttleMutex_;
    QHash<QString, qint64> lastSentTimestamps_;

    static SentryService* s_instance;
    static QtMessageHandler s_previousMessageHandler;
    static char s_crashFilePath[1024];
};

} // namespace komira
