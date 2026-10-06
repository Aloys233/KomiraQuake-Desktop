#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "core/intensity_calculator.h"
#include "model/data_source_info.h"
#include "model/earthquake_event.h"
#include "source/source_event_kind.h"

namespace komira {

/// 一帧报文进入活动生命周期后的处置结果。数据源据此可回报上游或调试工具：
/// 墓碑压制、重复、陈旧这些失败在链路上**不报错**，不回传就表现为静默丢弃。
enum class AdmissionStatus {
    Applied,      ///< 已进入（或更新）活动事件
    Tombstoned,   ///< 被终态墓碑压制：同一 eventId 此前已结束，旧报不得重开提醒
    Duplicate,    ///< 与已收报文重复（EventGate 判定）
    Ended,        ///< 取消报或已过期，活动事件随即结束
};

/// 数据源提供方的统一接口。Wolfx / Pancakes 以及后续新增的聚合商彼此**平级、互为备份**：
/// AppController 只按本接口接线，不再逐源硬编码。新增源只需实现该接口并在 AppController
/// 的注册表里登记一行；跨源合并、去重、聚合状态展示都由通用逻辑处理。
class EarthquakeSource : public QObject {
    Q_OBJECT
public:
    explicit EarthquakeSource(QObject* parent = nullptr) : QObject(parent) {}
    ~EarthquakeSource() override = default;

    /// 稳定标识（见 SourceIds::*）：用作设置键与逐源状态展示。
    virtual QString id() const = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
    /// 显式刷新目录；未启用时应忽略。
    virtual void refreshDirectory() = 0;
    virtual void setUserLocation(double lat, double lon) = 0;
    virtual void clearUserLocation() = 0;
    virtual void setStandard(IntensityStandard standard) = 0;
    /// 校时后的墙钟（epoch ms）：新鲜度判定与心跳展示走它。《NATIVE_PORT_SPEC》 §13。
    virtual void setNowProvider(std::function<long long()> provider) = 0;
    /// 单调耗时（ms）：目录轮询 RTT 量测走它，不受系统时间影响。
    virtual void setMonoProvider(std::function<long long()> provider) = 0;
    /// 凭据是否齐备（如 Jian 的登录令牌）。未就绪的源不会被启动，也就不会建立任何连接；
    /// 需要鉴权的源在设置里默认关闭，填好凭据后由用户主动开启。
    virtual bool isConfigured() const { return true; }
    virtual DataSourceInfo info() const = 0;
    /// 本源上一帧被 [eventReceived] 发出后，仓库层的最终处置结果。默认忽略：
    /// 只有需要把结果回传给上游（如自建的模拟源）的数据源才覆写。
    virtual void onAdmission(const EarthquakeEvent& event, AdmissionStatus status) {
        (void)event;
        (void)status;
    }

signals:
    void eventReceived(const komira::EarthquakeEvent& event, komira::SourceEventKind kind);
    void infoChanged();
};

} // namespace komira
