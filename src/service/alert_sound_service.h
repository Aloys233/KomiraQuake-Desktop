#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QMediaPlayer;
class QAudioOutput;

namespace komira {

/// 告警音效播放。资源位于 <appDir>/assets/sounds/{srev,general}。
class AlertSoundService : public QObject {
    Q_OBJECT
public:
    explicit AlertSoundService(QObject* parent = nullptr);
    ~AlertSoundService() override;

    void setAssetRoot(const QString& root);

    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) { enabled_ = enabled; if (!enabled) stopAll(); }

    double volume() const { return volume_; }
    void setVolume(double volume);

    void play(const QString& key, int cooldownMs = 0);
    void playCountdownClip(int secondsLeft);
    void playIntense();
    void stopAll();

private:
    QString pathFor(const QString& key) const;
    void scanAvailable();

    QString assetRoot_;
    QStringList available_;
    QHash<QString, qint64> lastPlayed_;
    QMediaPlayer* player_ = nullptr;
    QAudioOutput* audioOutput_ = nullptr;
    bool enabled_ = true;
    double volume_ = 1.0;
};

} // namespace komira
