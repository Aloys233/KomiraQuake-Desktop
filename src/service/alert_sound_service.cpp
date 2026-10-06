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
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia)
            onMediaFinished();
    });
    cuePlayer_ = new QMediaPlayer(this);
    cuePlayer_->setAudioOutput(audioOutput_);
    connect(cuePlayer_, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia)
                    onCueFinished();
            });
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

bool AlertSoundService::available(const QString& path) const {
    return available_.contains(QFileInfo(path).fileName());
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
    enqueueStatement(pathFor(key), cooldownMs);
}

void AlertSoundService::playCountdownClip(int secondsLeft) {
    if (!enabled_ || !cuePlayer_) return;
    if (secondsLeft < 0 || secondsLeft > 60) return;
    // 20/30/40/50/60s 是 1.7~1.8s 的整句播报，比倒计时周期长。
    // 正在播这类句子时必须让它说完，否则「还有 N秒抵达」每次都被切断。
    if (cuePlaying_) return;
    const QString file = QString::number(secondsLeft) + "s.mp3";
    const QString path = available_.contains(file)
                             ? pathFor(QString::number(secondsLeft) + "s")
                             : pathFor(QStringLiteral("countdown"));
    if (!available(path)) return;
    // 提示通道独立于语句通道：长音频（intense 3.1s）不会再堵死每秒一次的秒数。
    cuePlaying_ = true;
    cuePlayer_->setSource(QUrl::fromLocalFile(path));
    cuePlayer_->play();
}

void AlertSoundService::playArrivalCues() {
    if (!enabled_ || !cuePlayer_) return;
    // 抵达后补两下计时音，强化「已经到时」这一下。参考 kanameishi 的到时提示，
    // 但它只播一次 0s（Math.ceil 到 0 后不再变化），这里按需求多补两下。
    const QStringList cues = {QStringLiteral("0s"), QStringLiteral("countdown"),
                              QStringLiteral("countdown")};
    for (const QString& key : cues) {
        const QString path = pathFor(key);
        if (available(path)) cueQueue_.enqueue(path);
    }
    pumpCueQueue();
}

void AlertSoundService::pumpCueQueue() {
    if (!cuePlayer_ || cuePlaying_) return;
    if (cueQueue_.isEmpty()) return;
    const QString next = cueQueue_.dequeue();
    cuePlaying_ = true;
    cuePlayer_->setSource(QUrl::fromLocalFile(next));
    cuePlayer_->play();
}

void AlertSoundService::enqueueStatement(const QString& path, int cooldownMs) {
    if (!enabled_ || !player_ || path.isEmpty()) return;
    if (!available(path)) return;

    if (cooldownMs > 0) {
        const qint64 last = lastPlayed_.value(path, 0);
        if (QDateTime::currentMSecsSinceEpoch() - last < cooldownMs) return;
    }
    lastPlayed_[path] = QDateTime::currentMSecsSinceEpoch();

    queue_.enqueue(path.toStdString());
    pumpQueue();
}

void AlertSoundService::pumpQueue() {
    if (!player_ || playing_) return;
    const std::string next = queue_.take();
    if (next.empty()) return;
    playing_ = true;
    player_->setSource(QUrl::fromLocalFile(QString::fromStdString(next)));
    player_->play();
}

void AlertSoundService::onMediaFinished() {
    playing_ = false;
    pumpQueue();
}

void AlertSoundService::onCueFinished() {
    cuePlaying_ = false;
    pumpCueQueue();
}

void AlertSoundService::playIntense() { play(QStringLiteral("intense")); }

void AlertSoundService::stopAll() {
    queue_.clear();
    cueQueue_.clear();
    playing_ = false;
    if (player_) player_->stop();
    cuePlaying_ = false;
    if (cuePlayer_) cuePlayer_->stop();
}

} // namespace komira