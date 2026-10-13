#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <sentry.h>

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

/// 基于 sentry-native + Crashpad 的桌面 Sentry 客户端。
/// 提供：out-of-process 崩溃捕获、Qt 日志拦截、QML 错误监控、异常捕获与面包屑追踪。
/// 客户端恒定开启，不允许被禁用。
class SentryService : public QObject {
    Q_OBJECT

public:
    static SentryService* instance();

    /// 在 QApplication 创建后、业务对象创建前调用。
    static void initializeEarly();

    /// 初始化 sentry-native、Crashpad handler 和 SDK 持久化目录。
    void init();

    /// 在应用退出时刷新并关闭 SDK。
    static void shutdown();

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

    /// 兼容旧调用；Crashpad 自己持久化和补发崩溃，不再读取自定义日志。
    void checkAndSendPendingCrashes();

    /// 是否启用了 Sentry 上报
    bool isEnabled() const;

    /// 获取 DSN
    QString dsn() const { return dsn_; }

    /// 兼容旧调用：将语言级异常交给 sentry-native 上报。
    void recordCrashImmediately(const QString& type, const QString& message, const QString& stacktrace = QString());

signals:
    void testReportSent(bool success, const QString& message);

private:
    explicit SentryService(QObject* parent = nullptr);
    ~SentryService() override;

    static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message);
    static void addVariantMap(sentry_value_t object, const QVariantMap& values);

    QString dsn_;
    bool initialized_{false};
    bool shuttingDown_{false};

    static SentryService* s_instance;
    static QtMessageHandler s_previousMessageHandler;
    static bool s_messageHandlerInstalled;
};

} // namespace komira
