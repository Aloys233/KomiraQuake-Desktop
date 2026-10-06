#include "service/alert_announcer.h"

#include "core/intensity_calculator.h"
#include "prefs/settings_store.h"
#include "service/alert_sound_service.h"

namespace komira {

AlertAnnouncer::AlertAnnouncer(SettingsStore* settings, AlertSoundService* sound,
                               QObject* parent)
    : QObject(parent), settings_(settings), sound_(sound) {}

QString AlertAnnouncer::eventKey(const EarthquakeEvent& event) {
    return QString::fromStdString(event.identity());
}

AlertAnnouncer::EventState& AlertAnnouncer::stateFor(const EarthquakeEvent& event) {
    const QString key = eventKey(event);
    if (!states_.contains(key) && states_.size() > 20) states_.clear();
    return states_[key];
}

bool AlertAnnouncer::eligible(const EarthquakeEvent& event) const {
    // 预警总开关（设置页「地震预警」）：关闭时只展示，不产生任何提醒。
    if (!settings_->enableWarnings()) return false;
    if (event.isCanceled) return false;
    // 唯一过滤条件：本地预估烈度是否达到阈值（0 表示不作筛选，无定位放行）。
    // 按displayedLevel 比较，即「设置页所选显示标准」下的显示级数：
    //   - 取显示档位而非raw 原始值，否则 raw 2.6 显示为Ⅲ度、阈值 3.0 却被判为未达到；
    //   - 跟随所选标准，否则选 JMA 时会拿 CSIS 阈值去比震度，量纲不同。
    const double filter = settings_->localIntensityFilter();
    if (filter > 0.0 && event.distanceKm >= 0.0) {
        const auto standard = settings_->intensityStandard() == 1 ? IntensityStandard::Jma
                                                                    : IntensityStandard::Csis;
        const double level = IntensityCalculator::displayedLevel(
            event.magnitude, event.rawIntensity, event.distanceKm, event.depth, standard);
        if (level < filter) return false;
    }
    return true;
}

void AlertAnnouncer::stopOutput() {
    sound_->stopAll();
}

void AlertAnnouncer::finish(const EarthquakeEvent& event, bool ownsOutput) {
    states_.remove(eventKey(event));
    if (ownsOutput) stopOutput();
}

void AlertAnnouncer::onWarning(const EarthquakeEvent& event) {
    if (event.isCanceled) {
        finish(event, true);
        return;
    }
    if (settings_->isMuted() || !eligible(event)) return;
    EventState& state = stateFor(event);
    const bool first = !state.issued;
    if (first) sound_->play(QStringLiteral("issue"));
    else if (event.isFinal) sound_->play(QStringLiteral("final"));
    else sound_->play(QStringLiteral("update"), 3000);

    if (event.warningLevel == WarningLevel::Critical && !state.warned) {
        state.warned = state.cautioned = true;
        sound_->play(QStringLiteral("warn"));
    } else if (event.warningLevel == WarningLevel::Warning && !state.cautioned) {
        state.cautioned = true;
        sound_->play(QStringLiteral("caution"));
    }
    state.issued = true;
}

void AlertAnnouncer::onCountdown(const EarthquakeEvent& event, long long nowMs) {
    if (settings_->isMuted() || !eligible(event)) return;

    const int seconds = event.remainingSeconds(nowMs);
    if (seconds <= 0 || seconds > 60) return;
    EventState& current = stateFor(event);
    if (current.countdownSeconds.contains(seconds)) return;
    current.countdownSeconds.insert(seconds);
    sound_->playCountdownClip(seconds);

    if (seconds <= 10) {
        EventState& state = stateFor(event);
        if (!state.intense) {
            state.intense = true;
            sound_->playIntense();
        }
    }
}

void AlertAnnouncer::onArrived(const EarthquakeEvent& event) {
    if (settings_->isMuted() || !eligible(event)) return;
    EventState& state = stateFor(event);
    if (state.arrived) return;
    state.arrived = true;
    // 与 Android 同规则：只有确实播过倒计时的事件才播抵达，避免从未提醒过的事件
    // 在倒计时缺席的情况下凭空播报抵达。
    if (state.countdownSeconds.isEmpty()) return;
    // 抵达提示：`0s` + 两下计时音走提示通道连播，与 hypocenter 并发。
    sound_->playArrivalCues();
    sound_->play(QStringLiteral("hypocenter"), 10000);
}

void AlertAnnouncer::onMuteChanged() {
    if (settings_->isMuted()) {
        sound_->stopAll();
    }
}

void AlertAnnouncer::clear() { states_.clear(); }

} // namespace komira
