#include "service/sentry_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#if __has_include(<QQmlApplicationEngine>)
#include <QQmlApplicationEngine>
#include <QQmlError>
#define KOMIRA_HAS_QML 1
#endif
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <exception>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__linux__)
#include <execinfo.h>
#endif
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifndef KOMIRA_VERSION
#define KOMIRA_VERSION "1.1.2"
#endif

namespace komira {

SentryService* SentryService::s_instance = nullptr;
QtMessageHandler SentryService::s_previousMessageHandler = nullptr;
char SentryService::s_crashFilePath[1024] = {0};

namespace {
const char* const kDefaultDsn =
    "https://48fb324081d84266d8f3f961022bb237@o4511504349396992.ingest.us.sentry.io/4512246266789888";

QString generateUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
}

QString currentUtcIsoString() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
} // namespace

SentryService* SentryService::instance() {
    if (!s_instance) {
        s_instance = new SentryService();
    }
    return s_instance;
}

void SentryService::initializeEarly() {
    // 提前计算并建立崩溃记录目录与路径
    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!appDataDir.isEmpty()) {
        const QString crashDir = appDataDir + QStringLiteral("/crashes");
        QDir().mkpath(crashDir);
        const QString crashFile = crashDir + QStringLiteral("/last_crash.log");
        const QByteArray crashFileUtf8 = crashFile.toUtf8();
        if (crashFileUtf8.size() < static_cast<int>(sizeof(s_crashFilePath))) {
            std::strncpy(s_crashFilePath, crashFileUtf8.constData(), sizeof(s_crashFilePath) - 1);
            s_crashFilePath[sizeof(s_crashFilePath) - 1] = '\0';
        }
    }

    // 安装崩溃信号捕获
#ifdef Q_OS_UNIX
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SentryService::handleCrashSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
#endif

#ifdef Q_OS_WIN
    // 声明刻意用 void* 以免在头文件里暴露 windows.h；这里按 Win32 回调类型做一次
    // 函数指针转换（实现只是写崩溃转储，不读 _EXCEPTION_POINTERS，转换是安全的）。
    SetUnhandledExceptionFilter(
        reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(&SentryService::winUnhandledExceptionFilter));
#endif

    // 安装 C++ 未捕获异常 terminate 钩子
    std::set_terminate([]() {
        QString err = QStringLiteral("std::terminate called without active exception");
        std::exception_ptr p = std::current_exception();
        if (p) {
            try {
                std::rethrow_exception(p);
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("Unknown non-std C++ exception thrown");
            }
        }
        if (s_instance) {
            s_instance->recordCrashImmediately(QStringLiteral("UncaughtException"), err);
        }
        std::abort();
    });

    // 挂钩 Qt 日志输出
    if (!s_previousMessageHandler) {
        s_previousMessageHandler = qInstallMessageHandler(SentryService::qtMessageHandler);
    }
}

SentryService::SentryService(QObject* parent)
    : QObject(parent) {
    s_instance = this;
    parseDsn(QString::fromLatin1(kDefaultDsn));
}

SentryService::~SentryService() {
    if (ownsNam_ && nam_) {
        delete nam_;
        nam_ = nullptr;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void SentryService::parseDsn(const QString& dsn) {
    dsn_ = dsn.trimmed();
    QUrl url(dsn_);
    if (!url.isValid()) {
        return;
    }

    publicKey_ = url.userName();
    host_ = url.host();
    projectId_ = url.path();
    while (projectId_.startsWith(QLatin1Char('/'))) {
        projectId_.remove(0, 1);
    }

    if (!host_.isEmpty() && !projectId_.isEmpty()) {
        const QString scheme = url.scheme().isEmpty() ? QStringLiteral("https") : url.scheme();
        ingestUrl_ = QStringLiteral("%1://%2/api/%3/envelope/").arg(scheme, host_, projectId_);
    }
}

void SentryService::init(QNetworkAccessManager* nam) {
    if (nam) {
        nam_ = nam;
        ownsNam_ = false;
    } else if (!nam_) {
        nam_ = new QNetworkAccessManager(this);
        ownsNam_ = true;
    }

    // 记录一条启动面包屑
    addBreadcrumb(
        QStringLiteral("app.lifecycle"),
        QStringLiteral("KomiraQuake Desktop started, version %1").arg(QStringLiteral(KOMIRA_VERSION)),
        QStringLiteral("info"),
        {
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())}
        }
    );

    // 异步检查并上报上次未处理的崩溃
    QMetaObject::invokeMethod(this, &SentryService::checkAndSendPendingCrashes, Qt::QueuedConnection);
}

void SentryService::attachQmlEngine(QObject* engine) {
    if (!engine) return;

#if defined(KOMIRA_HAS_QML)
    auto* qmlEngine = qobject_cast<QQmlApplicationEngine*>(engine);
    if (qmlEngine) {
        connect(qmlEngine, &QQmlApplicationEngine::warnings, this,
                [this](const QList<QQmlError>& warnings) {
                    for (const auto& w : warnings) {
                        QVariantMap data;
                        data[QStringLiteral("url")] = w.url().toString();
                        data[QStringLiteral("line")] = w.line();
                        data[QStringLiteral("column")] = w.column();
                        addBreadcrumb(QStringLiteral("qml"), w.toString(), QStringLiteral("warning"), data);
                    }
                });
    }
#endif
}

bool SentryService::isEnabled() const {
    return true; // 恒定开启，不允许被禁用
}

void SentryService::addBreadcrumb(const QString& category, const QString& message,
                                 const QString& level, const QVariantMap& data) {
    QMutexLocker locker(&breadcrumbsMutex_);
    SentryBreadcrumb crumb;
    crumb.timestamp = currentUtcIsoString();
    crumb.category = category;
    crumb.message = message;
    crumb.level = level;
    crumb.data = data;

    breadcrumbs_.append(crumb);
    if (breadcrumbs_.size() > maxBreadcrumbs_) {
        breadcrumbs_.removeFirst();
    }
}

void SentryService::captureMessage(const QString& message, const QString& level,
                                   const QVariantMap& extra) {
    if (!isEnabled() || ingestUrl_.isEmpty()) {
        return;
    }

    const QString eventId = generateUuid();
    QJsonObject event = buildEventBase(level, eventId);

    QJsonObject msgObj;
    msgObj[QStringLiteral("formatted")] = message;
    event[QStringLiteral("message")] = msgObj;

    if (!extra.isEmpty()) {
        event[QStringLiteral("extra")] = QJsonObject::fromVariantMap(extra);
    }

    const QByteArray envelopeData = buildEnvelope(event, eventId);
    sendEnvelope(envelopeData, eventId, false);
}

void SentryService::captureException(const QString& type, const QString& value,
                                     const QString& stacktrace, const QString& level,
                                     const QVariantMap& extra) {
    if (!isEnabled() || ingestUrl_.isEmpty()) {
        return;
    }

    const QString eventId = generateUuid();
    QJsonObject event = buildEventBase(level, eventId);

    QJsonObject exceptionObj;
    exceptionObj[QStringLiteral("type")] = type;
    exceptionObj[QStringLiteral("value")] = value;

    if (!stacktrace.isEmpty()) {
        QJsonObject stackObj;
        QJsonArray frames;
        const QStringList lines = stacktrace.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            QJsonObject frame;
            frame[QStringLiteral("function")] = line.trimmed();
            frames.append(frame);
        }
        stackObj[QStringLiteral("frames")] = frames;
        exceptionObj[QStringLiteral("stacktrace")] = stackObj;
    }

    QJsonArray exceptions;
    exceptions.append(exceptionObj);
    QJsonObject exceptionWrapper;
    exceptionWrapper[QStringLiteral("values")] = exceptions;
    event[QStringLiteral("exception")] = exceptionWrapper;

    if (!extra.isEmpty()) {
        event[QStringLiteral("extra")] = QJsonObject::fromVariantMap(extra);
    }

    const QByteArray envelopeData = buildEnvelope(event, eventId);
    sendEnvelope(envelopeData, eventId, false);
}

bool SentryService::sendTestReport() {
    if (ingestUrl_.isEmpty()) {
        emit testReportSent(false, QStringLiteral("Sentry DSN 未配置喵！"));
        return false;
    }

    const QString eventId = generateUuid();
    QJsonObject event = buildEventBase(QStringLiteral("info"), eventId);

    QJsonObject msgObj;
    msgObj[QStringLiteral("formatted")] =
        QStringLiteral("KomiraQuake Sentry 诊断测试：客户端连接正常，测试事件发送成功！");
    event[QStringLiteral("message")] = msgObj;

    QJsonObject extra;
    extra[QStringLiteral("test_trigger")] = QStringLiteral("manual_user_button");
    extra[QStringLiteral("time_local")] = QDateTime::currentDateTime().toString();
    event[QStringLiteral("extra")] = extra;

    const QByteArray envelopeData = buildEnvelope(event, eventId);
    sendEnvelope(envelopeData, eventId, true);
    return true;
}

void SentryService::checkAndSendPendingCrashes() {
    if (!isEnabled() || ingestUrl_.isEmpty() || s_crashFilePath[0] == '\0') {
        return;
    }

    const QString filePath = QString::fromUtf8(s_crashFilePath);
    QFile file(filePath);
    if (!file.exists() || file.size() == 0) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QByteArray content = file.readAll();
    file.close();
    file.remove(); // 读出后删除，避免重复上报

    const QString text = QString::fromUtf8(content);
    QString sigName = QStringLiteral("Unknown Native Crash");
    QString stacktrace;

    const QStringList sections = text.split(QStringLiteral("---STACKTRACE---"));
    if (!sections.isEmpty()) {
        const QString header = sections.first();
        for (const QString& line : header.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("SIGNAL: "))) {
                sigName = line.mid(8).trimmed();
                break;
            }
        }
    }
    if (sections.size() > 1) {
        stacktrace = sections.at(1).trimmed();
    }

    captureException(QStringLiteral("NativeCrash"), sigName, stacktrace, QStringLiteral("fatal"),
                     {{QStringLiteral("recovered_from_previous_crash"), true}});
}

void SentryService::recordCrashImmediately(const QString& type, const QString& message,
                                           const QString& stacktrace) {
    if (s_crashFilePath[0] == '\0') return;

    QFile file(QString::fromUtf8(s_crashFilePath));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QString content = QStringLiteral("CRASH_REPORT_V1\nSIGNAL: %1: %2\n---STACKTRACE---\n%3\n")
                              .arg(type, message, stacktrace);
        file.write(content.toUtf8());
        file.close();
    }
}

QJsonObject SentryService::buildEventBase(const QString& level, const QString& eventId) {
    QJsonObject event;
    event[QStringLiteral("event_id")] = eventId;
    event[QStringLiteral("timestamp")] = currentUtcIsoString();
    event[QStringLiteral("platform")] = QStringLiteral("native");
    event[QStringLiteral("level")] = level;
    event[QStringLiteral("logger")] = QStringLiteral("komiraquake.desktop");
    event[QStringLiteral("release")] = QStringLiteral("komiraquake@") + QStringLiteral(KOMIRA_VERSION);

#ifdef QT_NO_DEBUG
    event[QStringLiteral("environment")] = QStringLiteral("production");
#else
    event[QStringLiteral("environment")] = QStringLiteral("development");
#endif

    // 上下文 Contexts
    QJsonObject contexts;

    QJsonObject osObj;
    osObj[QStringLiteral("name")] = QSysInfo::prettyProductName();
    osObj[QStringLiteral("version")] = QSysInfo::productVersion();
    osObj[QStringLiteral("kernel_version")] = QSysInfo::kernelVersion();
    contexts[QStringLiteral("os")] = osObj;

    QJsonObject deviceObj;
    deviceObj[QStringLiteral("arch")] = QSysInfo::currentCpuArchitecture();
    deviceObj[QStringLiteral("model")] = QSysInfo::machineHostName();
    contexts[QStringLiteral("device")] = deviceObj;

    QJsonObject runtimeObj;
    runtimeObj[QStringLiteral("name")] = QStringLiteral("Qt");
    runtimeObj[QStringLiteral("version")] = QString::fromLatin1(qVersion());
    contexts[QStringLiteral("runtime")] = runtimeObj;

    QJsonObject appObj;
    appObj[QStringLiteral("app_name")] = QStringLiteral("KomiraQuake");
    appObj[QStringLiteral("app_version")] = QStringLiteral(KOMIRA_VERSION);
    contexts[QStringLiteral("app")] = appObj;

    event[QStringLiteral("contexts")] = contexts;

    // 标签 Tags
    QJsonObject tags;
    tags[QStringLiteral("os_type")] = QSysInfo::productType();
    tags[QStringLiteral("kernel")] = QSysInfo::kernelType();
    tags[QStringLiteral("desktop")] = QStringLiteral("true");
    event[QStringLiteral("tags")] = tags;

    // 面包屑 Breadcrumbs
    QJsonArray crumbsArray;
    {
        QMutexLocker locker(&breadcrumbsMutex_);
        for (const auto& c : breadcrumbs_) {
            QJsonObject co;
            co[QStringLiteral("timestamp")] = c.timestamp;
            co[QStringLiteral("category")] = c.category;
            co[QStringLiteral("message")] = c.message;
            co[QStringLiteral("level")] = c.level;
            if (!c.data.isEmpty()) {
                co[QStringLiteral("data")] = QJsonObject::fromVariantMap(c.data);
            }
            crumbsArray.append(co);
        }
    }
    event[QStringLiteral("breadcrumbs")] = crumbsArray;

    return event;
}

QByteArray SentryService::buildEnvelope(const QJsonObject& event, const QString& eventId) {
    // Envelope 格式：
    // 行 1: Envelope Header
    // 行 2: Item Header
    // 行 3: Payload JSON

    QJsonObject envHeader;
    envHeader[QStringLiteral("event_id")] = eventId;
    envHeader[QStringLiteral("sent_at")] = currentUtcIsoString();

    QJsonObject sdk;
    sdk[QStringLiteral("name")] = QStringLiteral("sentry.native.komiraquake");
    sdk[QStringLiteral("version")] = QStringLiteral(KOMIRA_VERSION);
    envHeader[QStringLiteral("sdk")] = sdk;

    QJsonObject itemHeader;
    itemHeader[QStringLiteral("type")] = QStringLiteral("event");
    itemHeader[QStringLiteral("content_type")] = QStringLiteral("application/json");

    QByteArray out;
    out.append(QJsonDocument(envHeader).toJson(QJsonDocument::Compact));
    out.append('\n');
    out.append(QJsonDocument(itemHeader).toJson(QJsonDocument::Compact));
    out.append('\n');
    out.append(QJsonDocument(event).toJson(QJsonDocument::Compact));
    out.append('\n');

    return out;
}

void SentryService::sendEnvelope(const QByteArray& envelopeData, const QString& eventId,
                                bool isTest) {
    if (!nam_ || ingestUrl_.isEmpty()) {
        if (isTest) {
            emit testReportSent(false, QStringLiteral("网络管理器未准备好喵！"));
        }
        return;
    }

    QNetworkRequest req((QUrl(ingestUrl_)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-sentry-envelope"));

    const QString authHeader = QStringLiteral(
        "Sentry sentry_version=7, sentry_client=komiraquake-desktop/%1, sentry_key=%2"
    ).arg(QStringLiteral(KOMIRA_VERSION), publicKey_);
    req.setRawHeader("X-Sentry-Auth", authHeader.toUtf8());

    QNetworkReply* reply = nam_->post(req, envelopeData);
    connect(reply, &QNetworkReply::finished, this, [this, reply, eventId, isTest]() {
        reply->deleteLater();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool success = (reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300);

        if (isTest) {
            if (success) {
                emit testReportSent(true, QStringLiteral("测试事件已成功发送到 Sentry 喵！(ID: %1)").arg(eventId));
            } else {
                emit testReportSent(false, QStringLiteral("Sentry 发送失败喵 (HTTP %1): %2")
                                              .arg(statusCode)
                                              .arg(reply->errorString()));
            }
        }
    });
}

void SentryService::qtMessageHandler(QtMsgType type, const QMessageLogContext& context,
                                     const QString& message) {
    if (s_instance) {
        QString levelStr;
        switch (type) {
        case QtDebugMsg:    levelStr = QStringLiteral("debug"); break;
        case QtInfoMsg:     levelStr = QStringLiteral("info"); break;
        case QtWarningMsg:  levelStr = QStringLiteral("warning"); break;
        case QtCriticalMsg: levelStr = QStringLiteral("error"); break;
        case QtFatalMsg:    levelStr = QStringLiteral("fatal"); break;
        }

        QVariantMap data;
        if (context.file) data[QStringLiteral("file")] = QString::fromUtf8(context.file);
        if (context.line > 0) data[QStringLiteral("line")] = context.line;
        if (context.function) data[QStringLiteral("function")] = QString::fromUtf8(context.function);
        if (context.category && qstrcmp(context.category, "default") != 0) {
            data[QStringLiteral("category")] = QString::fromUtf8(context.category);
        }

        s_instance->addBreadcrumb(QStringLiteral("qt.log"), message, levelStr, data);

        // 如果是 critical 或 fatal，进行限流去重上报
        if (type == QtCriticalMsg || type == QtFatalMsg) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            bool shouldSend = false;
            {
                QMutexLocker locker(&s_instance->throttleMutex_);
                const qint64 lastTime = s_instance->lastSentTimestamps_.value(message, 0);
                if (now - lastTime > 5000) { // 5秒内同名错误只报一次
                    s_instance->lastSentTimestamps_[message] = now;
                    shouldSend = true;
                }
            }
            if (shouldSend) {
                s_instance->captureMessage(message, levelStr, data);
            }
        }
    }

    if (s_previousMessageHandler) {
        s_previousMessageHandler(type, context, message);
    }
}

#ifdef Q_OS_UNIX
void SentryService::handleCrashSignal(int sig) {
    if (s_crashFilePath[0] != '\0') {
        const int fd = open(s_crashFilePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            const char* sigName = "UNKNOWN";
            switch (sig) {
            case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault)"; break;
            case SIGABRT: sigName = "SIGABRT (Abort)"; break;
            case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
            case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
            case SIGBUS:  sigName = "SIGBUS (Bus Error)"; break;
            }
            const char header[] = "CRASH_REPORT_V1\nSIGNAL: ";
            ssize_t ret = 0;
            ret += write(fd, header, sizeof(header) - 1);
            ret += write(fd, sigName, std::strlen(sigName));
            const char traceHeader[] = "\n---STACKTRACE---\n";
            ret += write(fd, traceHeader, sizeof(traceHeader) - 1);
            (void)ret;

#if defined(__GLIBC__) || defined(__linux__)
            void* callstack[64];
            int frames = backtrace(callstack, 64);
            backtrace_symbols_fd(callstack, frames, fd);
#endif
            close(fd);
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

#ifdef Q_OS_WIN
long __stdcall SentryService::winUnhandledExceptionFilter(void* exceptionPointers) {
    if (s_crashFilePath[0] != '\0') {
        HANDLE hFile = CreateFileA(s_crashFilePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const char header[] = "CRASH_REPORT_V1\nSIGNAL: Windows Unhandled Exception\n---STACKTRACE---\n";
            WriteFile(hFile, header, static_cast<DWORD>(std::strlen(header)), &written, NULL);

            void* stack[64];
            unsigned short frames = CaptureStackBackTrace(0, 64, stack, NULL);
            for (unsigned short i = 0; i < frames; ++i) {
                char buf[64];
                int len = snprintf(buf, sizeof(buf), "[%u] %p\n", i, stack[i]);
                WriteFile(hFile, buf, static_cast<DWORD>(len), &written, NULL);
            }
            CloseHandle(hFile);
        }
    }
    return 0; // EXCEPTION_CONTINUE_SEARCH
}
#endif

} // namespace komira
