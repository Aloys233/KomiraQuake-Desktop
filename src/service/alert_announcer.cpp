#include "service/alert_announcer.h"

#include <QSet>

#include "prefs/settings_store.h"
#include "service/alert_sound_service.h"
#include "service/speech_service.h"

namespace komira {

AlertAnnouncer::AlertAnnouncer(SettingsStore* settings, AlertSoundService* sound,
                               SpeechService* speech, QObject* parent)
    : QObject(parent), settings_(settings), sound_(sound), speech_(speech) {}

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
    const double filter = settings_->localIntensityFilter();
    if (filter > 0.0 && event.distanceKm >= 0.0 && event.rawIntensity < filter) return false;
    return true;
}

void AlertAnnouncer::stopOutput() {
    sound_->stopAll();
    speech_->stop();
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
    speakPhase(event, state);
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

    static const QSet<int> speechSeconds = {10, 20, 30};
    if (settings_->speakCountdown() && speechSeconds.contains(seconds)) {
        speech_->speak(QStringLiteral("预计还有 %1 秒。").arg(seconds),
                       eventKey(event) + QStringLiteral(":countdown:%1").arg(seconds), 2000);
    }
}

void AlertAnnouncer::onArrived(const EarthquakeEvent& event) {
    if (settings_->isMuted() || !eligible(event)) return;
    sound_->play(QStringLiteral("hypocenter"), 10000);
}

void AlertAnnouncer::onMuteChanged() {
    if (settings_->isMuted()) {
        sound_->stopAll();
        speech_->stop();
    }
}

void AlertAnnouncer::clear() { states_.clear(); }

void AlertAnnouncer::speakPhase(const EarthquakeEvent& event, EventState& state) {
    if (!settings_->enableSpeech()) return;
    const QString location = QString::fromStdString(event.location);
    QString text;
    if (event.isFinal) {
        text = QStringLiteral("%1 地震，最终报，震级 %2。")
                   .arg(location)
                   .arg(event.magnitude, 0, 'f', 1);
    } else if (!state.issued) {
        text = QStringLiteral("%1 发生地震，预估烈度 %2，震级 %3。")
                   .arg(location, QString::fromStdString(event.estimatedIntensity))
                   .arg(event.magnitude, 0, 'f', 1);
    } else if (event.warningLevel == WarningLevel::Critical) {
        text = QStringLiteral("严重地震预警，%1，请立即避险。").arg(location);
    } else if (settings_->speakUpdates()) {
        text = QStringLiteral("地震预警更新，%1，震级 %2。").arg(location).arg(event.magnitude, 0, 'f', 1);
    }
    if (!text.isEmpty()) {
        speech_->speak(text, eventKey(event) + QStringLiteral(":") + QString::number(event.reportNum), 6000);
    }
}

} // namespace komira
