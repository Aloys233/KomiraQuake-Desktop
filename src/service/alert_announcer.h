#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

#include "model/earthquake_event.h"

namespace komira {

class AlertSoundService;
class SpeechService;
class SettingsStore;

/// 告警编排：音效 + 语音。《NATIVE_PORT_SPEC》 §10。
class AlertAnnouncer : public QObject {
    Q_OBJECT
public:
    AlertAnnouncer(SettingsStore* settings, AlertSoundService* sound, SpeechService* speech,
                   QObject* parent = nullptr);

    void onWarning(const EarthquakeEvent& event);
    /// [nowMs] 为校时后的当前时刻（epoch ms）。《NATIVE_PORT_SPEC》 §13。
    void onCountdown(const EarthquakeEvent& event, long long nowMs);
    void onArrived(const EarthquakeEvent& event);
    void onMuteChanged();
    void clear();
    void stopOutput();
    void finish(const EarthquakeEvent& event, bool ownsOutput);
    bool eligible(const EarthquakeEvent& event) const;

private:
    struct EventState {
        bool issued = false;
        bool warned = false;
        bool cautioned = false;
        bool intense = false;
        QSet<int> countdownSeconds;
    };

    EventState& stateFor(const EarthquakeEvent& event);
    static QString eventKey(const EarthquakeEvent& event);
    void speakPhase(const EarthquakeEvent& event, EventState& state);

    SettingsStore* settings_;
    AlertSoundService* sound_;
    SpeechService* speech_;
    QHash<QString, EventState> states_;
};

} // namespace komira
