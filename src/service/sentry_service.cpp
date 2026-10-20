#include "service/sentry_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#if __has_include(<QQmlApplicationEngine>)
#include <QQmlApplicationEngine>
#include <QQmlError>
#define KOMIRA_HAS_QML 1
#endif
#include <QStandardPaths>
#include <QSysInfo>

#include <string>

#ifndef KOMIRA_VERSION
#define KOMIRA_VERSION "1.3.2"
#endif

namespace komira {

SentryService* SentryService::s_instance = nullptr;
QtMessageHandler SentryService::s_previousMessageHandler = nullptr;
bool SentryService::s_messageHandlerInstalled = false;

namespace {

const char* const kDefaultDsn =
    "https://48fb324081d84266d8f3f961022bb237@o4511504349396992.ingest.us.sentry.io/4512246266789888";

sentry_level_t sentryLevel(const QString& level) {
    if (level.compare(QStringLiteral("fatal"), Qt::CaseInsensitive) == 0) return SENTRY_LEVEL_FATAL;
    if (level.compare(QStringLiteral("error"), Qt::CaseInsensitive) == 0) return SENTRY_LEVEL_ERROR;
    if (level.compare(QStringLiteral("warning"), Qt::CaseInsensitive) == 0) return SENTRY_LEVEL_WARNING;
    if (level.compare(QStringLiteral("debug"), Qt::CaseInsensitive) == 0) return SENTRY_LEVEL_DEBUG;
    return SENTRY_LEVEL_INFO;
}

sentry_value_t sentryValue(const QVariant& value) {
    if (!value.isValid() || value.isNull()) return sentry_value_new_null();
    if (value.typeId() == QMetaType::Bool) return sentry_value_new_bool(value.toBool());
    if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong) {
        return sentry_value_new_int64(value.toLongLong());
    }
    if (value.typeId() == QMetaType::UInt || value.typeId() == QMetaType::ULongLong) {
        return sentry_value_new_uint64(value.toULongLong());
    }
    if (value.typeId() == QMetaType::Double) return sentry_value_new_double(value.toDouble());
    if (value.typeId() == QMetaType::QVariantMap) {
        sentry_value_t object = sentry_value_new_object();
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            const QByteArray key = it.key().toUtf8();
            sentry_value_set_by_key(object, key.constData(), sentryValue(it.value()));
        }
        return object;
    }
    if (value.typeId() == QMetaType::QStringList) {
        sentry_value_t list = sentry_value_new_list();
        for (const QString& item : value.toStringList()) {
            const QByteArray utf8 = item.toUtf8();
            sentry_value_append(list, sentry_value_new_string(utf8.constData()));
        }
        return list;
    }

    const QByteArray utf8 = value.toString().toUtf8();
    return sentry_value_new_string(utf8.constData());
}

} // namespace

SentryService* SentryService::instance() {
    if (!s_instance) s_instance = new SentryService();
    return s_instance;
}

void SentryService::initializeEarly() {
    // Crashpad is installed by sentry_init(). Only install the Qt bridge here;
    // this function deliberately does not touch signal/SEH handlers.
    if (!s_messageHandlerInstalled) {
        s_previousMessageHandler = qInstallMessageHandler(SentryService::qtMessageHandler);
        s_messageHandlerInstalled = true;
    }
}

SentryService::SentryService(QObject* parent)
    : QObject(parent), dsn_(QString::fromLatin1(kDefaultDsn)) {
    s_instance = this;
}

SentryService::~SentryService() {
    shutdown();
    if (s_instance == this) s_instance = nullptr;
}

void SentryService::init() {
    if (initialized_ || shuttingDown_) return;

    sentry_options_t* options = sentry_options_new();
    if (!options) {
        qWarning() << "sentry-native: unable to allocate options";
        return;
    }

    const QByteArray dsn = dsn_.toUtf8();
    sentry_options_set_dsn(options, dsn.constData());
    sentry_options_set_release(options, KOMIRA_VERSION);
#ifdef QT_NO_DEBUG
    sentry_options_set_environment(options, "production");
#else
    sentry_options_set_environment(options, "development");
#endif
    sentry_options_set_symbolize_stacktraces(options, 1);

    // Crashpad owns and rotates files in this directory, so it must not share
    // the application's normal data directory.
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) cacheDir = QCoreApplication::applicationDirPath() + QStringLiteral("/cache");
    const QString sentryDir = cacheDir + QStringLiteral("/sentry");
    QDir().mkpath(sentryDir);
#ifdef Q_OS_WIN
    const std::wstring sentryDirWide = sentryDir.toStdWString();
    sentry_options_set_database_pathw(options, sentryDirWide.c_str());
#else
    const QByteArray sentryDirUtf8 = sentryDir.toUtf8();
    sentry_options_set_database_path(options, sentryDirUtf8.constData());
#endif

    QString handlerPath = QCoreApplication::applicationDirPath() + QStringLiteral("/crashpad_handler");
#ifdef Q_OS_WIN
    handlerPath += QStringLiteral(".exe");
#endif
    if (!QFileInfo::exists(handlerPath)) {
        qWarning().noquote() << "sentry-native: Crashpad handler is missing:" << handlerPath;
    }
#ifdef Q_OS_WIN
    const std::wstring handlerPathWide = handlerPath.toStdWString();
    sentry_options_set_handler_pathw(options, handlerPathWide.c_str());
#else
    const QByteArray handlerPathUtf8 = handlerPath.toUtf8();
    sentry_options_set_handler_path(options, handlerPathUtf8.constData());
#endif
    // Keep the crashing process alive until Crashpad has handed the minidump to
    // its handler. This closes the small shutdown race on both Linux and Windows.
    sentry_options_set_crashpad_wait_for_upload(options, 1);

    const int result = sentry_init(options);
    if (result != 0) {
        qWarning() << "sentry-native: initialization failed with code" << result;
        return;
    }

    initialized_ = true;
    addBreadcrumb(
        QStringLiteral("app.lifecycle"),
        QStringLiteral("KomiraQuake Desktop started, version %1").arg(QStringLiteral(KOMIRA_VERSION)),
        QStringLiteral("info"),
        {
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())}
        });
}

void SentryService::shutdown() {
    if (!s_instance || !s_instance->initialized_ || s_instance->shuttingDown_) return;
    s_instance->shuttingDown_ = true;
    // Crashpad owns crash-time delivery; this flushes normal queued events.
    sentry_close();
    s_instance->initialized_ = false;
}

void SentryService::attachQmlEngine(QObject* engine) {
    if (!engine) return;

#if defined(KOMIRA_HAS_QML)
    auto* qmlEngine = qobject_cast<QQmlApplicationEngine*>(engine);
    if (qmlEngine) {
        connect(qmlEngine, &QQmlApplicationEngine::warnings, this,
                [this](const QList<QQmlError>& warnings) {
                    for (const auto& warning : warnings) {
                        QVariantMap data;
                        data[QStringLiteral("url")] = warning.url().toString();
                        data[QStringLiteral("line")] = warning.line();
                        data[QStringLiteral("column")] = warning.column();
                        addBreadcrumb(QStringLiteral("qml"), warning.toString(),
                                      QStringLiteral("warning"), data);
                    }
                });
    }
#endif
}

bool SentryService::isEnabled() const {
    return initialized_;
}

void SentryService::addVariantMap(sentry_value_t object, const QVariantMap& values) {
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const QByteArray key = it.key().toUtf8();
        sentry_value_set_by_key(object, key.constData(), sentryValue(it.value()));
    }
}

void SentryService::addBreadcrumb(const QString& category, const QString& message,
                                  const QString& level, const QVariantMap& data) {
    if (!initialized_ || shuttingDown_) return;
    const QByteArray categoryUtf8 = category.toUtf8();
    const QByteArray messageUtf8 = message.toUtf8();
    sentry_value_t breadcrumb = sentry_value_new_breadcrumb(categoryUtf8.constData(), messageUtf8.constData());
    const QByteArray levelUtf8 = level.toUtf8();
    sentry_value_set_by_key(breadcrumb, "category", sentry_value_new_string(categoryUtf8.constData()));
    sentry_value_set_by_key(breadcrumb, "level", sentry_value_new_string(levelUtf8.constData()));
    if (!data.isEmpty()) {
        sentry_value_t dataObject = sentry_value_new_object();
        addVariantMap(dataObject, data);
        sentry_value_set_by_key(breadcrumb, "data", dataObject);
    }
    sentry_add_breadcrumb(breadcrumb);
}

void SentryService::captureMessage(const QString& message, const QString& level,
                                   const QVariantMap& extra) {
    if (!initialized_ || shuttingDown_) return;
    const QByteArray messageUtf8 = message.toUtf8();
    sentry_value_t event = sentry_value_new_message_event(
        sentryLevel(level), "komiraquake.desktop", messageUtf8.constData());
    if (!extra.isEmpty()) {
        sentry_value_t extraObject = sentry_value_new_object();
        addVariantMap(extraObject, extra);
        sentry_value_set_by_key(event, "extra", extraObject);
    }
    sentry_capture_event(event);
}

void SentryService::captureException(const QString& type, const QString& value,
                                     const QString& stacktrace, const QString& level,
                                     const QVariantMap& extra) {
    if (!initialized_ || shuttingDown_) return;

    const QByteArray typeUtf8 = type.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    const QByteArray levelUtf8 = level.toUtf8();
    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(event, "level", sentry_value_new_string(levelUtf8.constData()));
    sentry_event_add_exception(
        event, sentry_value_new_exception(typeUtf8.constData(), valueUtf8.constData()));

    QVariantMap eventExtra = extra;
    if (!stacktrace.isEmpty()) eventExtra.insert(QStringLiteral("stacktrace"), stacktrace);
    if (!eventExtra.isEmpty()) {
        sentry_value_t extraObject = sentry_value_new_object();
        addVariantMap(extraObject, eventExtra);
        sentry_value_set_by_key(event, "extra", extraObject);
    }
    sentry_capture_event(event);
}

bool SentryService::sendTestReport() {
    if (!initialized_) {
        emit testReportSent(false, QStringLiteral("Sentry Native SDK 未初始化，Crashpad handler 可能未随程序发布。"));
        return false;
    }

    captureMessage(QStringLiteral("KomiraQuake Sentry 诊断测试：客户端连接正常，测试事件已加入 SDK 队列。"),
                   QStringLiteral("info"),
                   {{QStringLiteral("test_trigger"), QStringLiteral("manual_user_button")} });
    emit testReportSent(true, QStringLiteral("测试事件已加入 Sentry Native SDK 队列。"));
    return true;
}

void SentryService::checkAndSendPendingCrashes() {
    // Crashpad persists crash reports in sentryDir and the SDK uploads them
    // itself. This compatibility method intentionally does nothing.
}

void SentryService::recordCrashImmediately(const QString& type, const QString& message,
                                           const QString& stacktrace) {
    captureException(type, message, stacktrace, QStringLiteral("fatal"));
}

void SentryService::qtMessageHandler(QtMsgType type, const QMessageLogContext& context,
                                     const QString& message) {
    if (s_instance && s_instance->initialized_ && !s_instance->shuttingDown_) {
        QString level;
        switch (type) {
        case QtDebugMsg: level = QStringLiteral("debug"); break;
        case QtInfoMsg: level = QStringLiteral("info"); break;
        case QtWarningMsg: level = QStringLiteral("warning"); break;
        case QtCriticalMsg: level = QStringLiteral("error"); break;
        case QtFatalMsg: level = QStringLiteral("fatal"); break;
        }

        QVariantMap data;
        if (context.file) data[QStringLiteral("file")] = QString::fromUtf8(context.file);
        if (context.line > 0) data[QStringLiteral("line")] = context.line;
        if (context.function) data[QStringLiteral("function")] = QString::fromUtf8(context.function);
        if (context.category && qstrcmp(context.category, "default") != 0) {
            data[QStringLiteral("category")] = QString::fromUtf8(context.category);
        }

        s_instance->addBreadcrumb(QStringLiteral("qt.log"), message, level, data);
        if (type == QtCriticalMsg || type == QtFatalMsg) {
            s_instance->captureMessage(message, level, data);
        }
    }

    if (s_previousMessageHandler) s_previousMessageHandler(type, context, message);
}

} // namespace komira
