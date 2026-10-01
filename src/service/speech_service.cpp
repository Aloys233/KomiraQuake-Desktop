#include "service/speech_service.h"

#include <QDateTime>
#include <QTextToSpeech>

namespace komira {

SpeechService::SpeechService(QObject* parent) : QObject(parent) {
    tts_ = new QTextToSpeech(this);
    available_ = !tts_->availableEngines().isEmpty();
    if (available_) tts_->setLocale(QLocale(QLocale::Chinese, QLocale::China));
}

void SpeechService::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) stop();
}

void SpeechService::setRate(double rate) {
    rate_ = qBound(0.05, rate, 2.0);
    if (tts_) tts_->setRate(rate_);
}

void SpeechService::setVolume(double volume) {
    volume_ = qBound(0.0, volume, 1.0);
    if (tts_) tts_->setVolume(volume_);
}

void SpeechService::speak(const QString& text, const QString& dedupeKey, int dedupeWindowMs) {
    if (!enabled_ || !available_ || text.isEmpty()) return;
    if (!dedupeKey.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastSpoken_.value(dedupeKey, 0) < dedupeWindowMs) return;
        lastSpoken_[dedupeKey] = now;
    }
    if (tts_) {
        tts_->stop();
        tts_->say(text);
    }
}

void SpeechService::speakSample() {
    speak(QStringLiteral("地震预警测试：横波预计三十秒后到达，请就近避难。"),
          QStringLiteral("sample"));
}

void SpeechService::stop() {
    if (tts_) tts_->stop();
}

} // namespace komira
