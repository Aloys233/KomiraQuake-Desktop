#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include "core/sound_queue.h"

class QMediaPlayer;
class QAudioOutput;

namespace komira {

/// 告警音效播放。资源位于 <appDir>/assets/sounds/{srev,general}。
///
/// 分两条通道，避免长音频（intense 3.1s、hypocenter 5.4s）堵死每秒一次的倒计时：
///   - 语句通道：串行队列，一句播完再播下一句，绝不打断；
///   - 提示通道：倒计时与警报音独立播放器，可与语句并发，且新秒数立即顶掉旧秒数。
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

    /// 语句通道播放（issue/warn/final/update/...），可带冷却。
    void play(const QString& key, int cooldownMs = 0);
    /// 提示通道播放倒计时 `{n}s`。
    void playCountdownClip(int secondsLeft);
    /// 抵达提示序列：`0s` + 两下计时音。连着播完，不被后续秒数打断。
    void playArrivalCues();
    void playIntense();
    void stopAll();

private:
    QString pathFor(const QString& key) const;
    void scanAvailable();
    bool available(const QString& path) const;
    /// 语句通道入队并按需启动播放。
    void enqueueStatement(const QString& path, int cooldownMs);
    /// 语句队列串行播放：仅在空闲时启动下一段，避免新播报打断未播完的语句。
    void pumpQueue();
    void onMediaFinished();
    /// 启动提示通道待播序列的下一段。
    void pumpCueQueue();
    /// 提示通道播完一段后复位并续播序列。
    void onCueFinished();

    QString assetRoot_;
    QStringList available_;
    QHash<QString, qint64> lastPlayed_;
    SoundQueue queue_;
    /// 提示通道待播序列（抵达后的`0s` + 两下计时音）。
    QQueue<QString> cueQueue_;
    /// 语句通道播放器。
    QMediaPlayer* player_ = nullptr;
    /// 提示通道播放器：倒计时与警报音走这里，与语句并发。
    QMediaPlayer* cuePlayer_ = nullptr;
    QAudioOutput* audioOutput_ = nullptr;
    bool enabled_ = true;
    double volume_ = 1.0;
    /// 队列中是否已有片段在播。QMediaPlayer 状态切换存在异步窗口，需自行标记。
    bool playing_ = false;
    /// 提示通道是否正在播。20/30/40/50/60s 是 1.7~1.8s 的整句抵达播报，
    /// 每秒到来的秒数不得打断它们，否则「还有 N秒抵达」永远说不完整。
    bool cuePlaying_ = false;
};

} // namespace komira