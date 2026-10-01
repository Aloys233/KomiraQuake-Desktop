#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QTextToSpeech;

namespace komira {

/// 语音播报（zh-CN）。若 Qt TextToSpeech 不可用则静默降级。
class SpeechService : public QObject {
    Q_OBJECT
public:
    explicit SpeechService(QObject* parent = nullptr);

    bool available() const { return available_; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);

    void setRate(double rate);
    void setVolume(double volume);

    void speak(const QString& text, const QString& dedupeKey = QString(), int dedupeWindowMs = 4000);
    void speakSample();
    void stop();

private:
    QTextToSpeech* tts_ = nullptr;
    QHash<QString, qint64> lastSpoken_;
    bool enabled_ = false;
    bool available_ = false;
    double rate_ = 0.5;
    double volume_ = 1.0;
};

} // namespace komira
