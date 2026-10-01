#include "service/alert_sound_service.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QUrl>

namespace komira {

AlertSoundService::AlertSoundService(QObject* parent) : QObject(parent) {
    audioOutput_ = new QAudioOutput(this);
    audioOutput_->setVolume(volume_);
    player_ = new QMediaPlayer(this);
    player_->setAudioOutput(audioOutput_);
}

AlertSoundService::~AlertSoundService() = default;

void AlertSoundService::setAssetRoot(const QString& root) {
    assetRoot_ = root;
    scanAvailable();
}

void AlertSoundService::scanAvailable() {
    available_.clear();
    for (const QString& dir : {QStringLiteral("srev"), QStringLiteral("general")}) {
        QDir d(assetRoot_ + "/sounds/" + dir);
        if (d.exists()) available_ += d.entryList(QDir::Files);
    }
}

void AlertSoundService::setVolume(double volume) {
    volume_ = qBound(0.0, volume, 1.0);
    if (audioOutput_) audioOutput_->setVolume(volume_);
}

QString AlertSoundService::pathFor(const QString& key) const {
    if (key == "countdown") return assetRoot_ + "/sounds/general/countdown.wav";
    if (key == "intense") return assetRoot_ + "/sounds/general/intense.wav";
    if (key == "ews") return assetRoot_ + "/sounds/general/ews.mp3";

    bool ok = false;
    if (key.endsWith('s')) {
        key.left(key.size() - 1).toInt(&ok);
        if (ok) return assetRoot_ + "/sounds/general/" + key + ".mp3";
    }
    return assetRoot_ + "/sounds/srev/" + key + ".mp3";
}

void AlertSoundService::play(const QString& key, int cooldownMs) {
    if (!enabled_ || !player_) return;
    const QString fileName = QFileInfo(pathFor(key)).fileName();
    if (!available_.contains(fileName)) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cooldownMs > 0) {
        const qint64 last = lastPlayed_.value(key, 0);
        if (now - last < cooldownMs) return;
    }
    lastPlayed_[key] = now;

    player_->setSource(QUrl::fromLocalFile(pathFor(key)));
    player_->play();
}

void AlertSoundService::playCountdownClip(int secondsLeft) {
    if (secondsLeft < 0 || secondsLeft > 60) return;
    const QString file = QString::number(secondsLeft) + "s.mp3";
    if (available_.contains(file)) {
        play(QString::number(secondsLeft) + "s");
    } else {
        play(QStringLiteral("countdown"));
    }
}

void AlertSoundService::playIntense() { play(QStringLiteral("intense")); }

void AlertSoundService::stopAll() {
    if (player_) player_->stop();
}

} // namespace komira
