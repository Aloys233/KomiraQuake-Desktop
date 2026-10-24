#pragma once

#include <QString>
#include <QStringList>

#include <cmath>
#include <string>

namespace komira {

/// 报次归档的分组键：**报数机构 + 发震时刻的分钟桶 + 震中 0.1° 网格**。
///
/// 为什么分钟桶而不是毫秒：同一场地震的实时预警（EEW）与地震情报走两条链路，发震
/// 时刻口径不同 —— 预警精确到秒（`02:31:22`），JMA 情报的 `time_full` 是把秒**截断**
/// 到分钟（`02:31:00`），CENC 的自动测定与正式测定之间也会差几秒。用毫秒做键会让
/// 同一场地震拆成两组报次，详情页因此永远只看得到「第 1 报」。
///
/// 为什么还要震中网格：同一机构一分钟内理论上可能报两场小震（余震串），只按分钟归档
/// 会把它们并成一场。0.1° 约 11 km，小于 `kSameQuakeRadiusKm`(25 km)：跨链路的震中
/// 抖动（实测 CENC 同一地震 0.03°）落在同一格，相距 20 km 以上的两场地震基本分格。
///
/// 边界抖动（分钟进位、网格跨格）由 `quakeGroupKeys` 兜住：它同时给出本格与相邻格的
/// 相邻分钟共 27 个候选键，写入/读取时只接受**校验通过**（时刻差 ≤ 90 s、震中 ≤ 25 km，
/// 见 AppController）的那一组，绝不无条件按时间窗合并。
inline long long quakeMinuteBucket(long long timestampMs) {
    constexpr long long kMinute = 60'000;
    if (timestampMs <= 0) return 0;
    return timestampMs / kMinute;
}

/// 震中网格单元（0.1°）。坐标无效或为取消报文的 0/0 占位时返回空串。
inline QString quakeGridCell(double latitude, double longitude, int latOffset = 0,
                             int lonOffset = 0) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude)) return {};
    if (latitude == 0.0 && longitude == 0.0) return {};
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) return {};
    const long long latCell = std::llround(latitude * 10.0) + latOffset;
    const long long lonCell = std::llround(longitude * 10.0) + lonOffset;
    return QStringLiteral("%1:%2").arg(latCell).arg(lonCell);
}

inline QString quakeGroupKey(const std::string& agency, long long bucket, const QString& cell) {
    const QString trimmed = QString::fromStdString(agency).trimmed().toLower();
    if (trimmed.isEmpty() || bucket <= 0) return {};
    if (cell.isEmpty()) return trimmed + QLatin1Char('|') + QString::number(bucket);
    return trimmed + QLatin1Char('|') + QString::number(bucket) + QLatin1Char('|') + cell;
}

/// 候选归档键，**靠前的优先级更高**：同格同分钟 → 同格相邻分钟 → 邻格。
inline QStringList quakeGroupKeys(const std::string& agency, long long timestampMs, double latitude,
                                  double longitude) {
    QStringList keys;
    const long long bucket = quakeMinuteBucket(timestampMs);
    if (bucket <= 0 || QString::fromStdString(agency).trimmed().isEmpty()) return keys;
    const QString primaryCell = quakeGridCell(latitude, longitude);
    const long long buckets[] = {bucket, bucket - 1, bucket + 1};
    if (primaryCell.isEmpty()) {
        // 取消报文没有震中：只能按分钟命中，仍要求调用方做时刻校验。
        for (const long long b : buckets)
            if (b > 0) keys.push_back(quakeGroupKey(agency, b, QString()));
        return keys;
    }
    for (const long long b : buckets) {
        if (b <= 0) continue;
        keys.push_back(quakeGroupKey(agency, b, primaryCell));
    }
    for (int dLat = -1; dLat <= 1; ++dLat) {
        for (int dLon = -1; dLon <= 1; ++dLon) {
            if (dLat == 0 && dLon == 0) continue;
            const QString cell = quakeGridCell(latitude, longitude, dLat, dLon);
            for (const long long b : buckets)
                if (b > 0) keys.push_back(quakeGroupKey(agency, b, cell));
        }
    }
    return keys;
}

/// 主键：本格本分钟。写入时若没有任何候选键命中，就落在这里。
inline QString quakePrimaryGroupKey(const std::string& agency, long long timestampMs,
                                    double latitude, double longitude) {
    return quakeGroupKey(agency, quakeMinuteBucket(timestampMs), quakeGridCell(latitude, longitude));
}

/// 同一分钟内的候选键（本格 + 相邻 8 格）。
///
/// 用于**同一份上游报文**的跨链路识别：Wolfx 的 CENC 速报保留秒（16:47:57），Whews
/// 的同一条只有分钟（16:47:00），两路的 EventID 中段又是各自的报文时刻，所以既不能
/// 按 ID 也不能按毫秒对上。它们的共同点是落在同一分钟里 —— 但分钟内的两场小震仍可能
/// 互不相同，所以这里只给候选，调用方必须再按震中距离复核（见 AppController）。
inline QStringList quakeSameMinuteKeys(const std::string& agency, long long timestampMs,
                                       double latitude, double longitude) {
    QStringList keys;
    const long long bucket = quakeMinuteBucket(timestampMs);
    if (bucket <= 0 || QString::fromStdString(agency).trimmed().isEmpty()) return keys;
    const QString primaryCell = quakeGridCell(latitude, longitude);
    if (primaryCell.isEmpty()) {
        keys.push_back(quakeGroupKey(agency, bucket, QString()));
        return keys;
    }
    keys.push_back(quakeGroupKey(agency, bucket, primaryCell));
    for (int dLat = -1; dLat <= 1; ++dLat) {
        for (int dLon = -1; dLon <= 1; ++dLon) {
            if (dLat == 0 && dLon == 0) continue;
            keys.push_back(quakeGroupKey(agency, bucket, quakeGridCell(latitude, longitude, dLat, dLon)));
        }
    }
    return keys;
}

/// 归档键对应的「同一场地震」校验：时刻差与震中距离都在容差内。
/// 归档候选键本身已把范围限制在 ±1 分钟 / 相邻 0.1°；这一步挡掉真正相邻的两场小震。
inline bool quakeGroupMatches(long long timestampA, double latA, double lonA, long long timestampB,
                              double latB, double lonB) {
    if (timestampA <= 0 || timestampB <= 0) return false;
    constexpr long long kToleranceMs = 90'000;
    const long long delta = timestampA > timestampB ? timestampA - timestampB : timestampB - timestampA;
    if (delta > kToleranceMs) return false;
    // 缺坐标（取消报文）时只靠时刻判定 —— 同一机构 90 s 内不会报两场地震。
    if ((latA == 0.0 && lonA == 0.0) || (latB == 0.0 && lonB == 0.0)) return true;
    constexpr double kEarthRadiusKm = 6371.0;
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double dLat = (latB - latA) * kDeg;
    const double dLon = (lonB - lonA) * kDeg;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                     + std::cos(latA * kDeg) * std::cos(latB * kDeg) * std::sin(dLon / 2)
                           * std::sin(dLon / 2);
    const double km = 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(a)));
    return km <= 25.0;
}

/// 发震时刻是否恰好落在整分钟：说明这条链路把秒**截断**了（JMA 的 `time_full`、
/// Whews 的 `shockTime` 都是这种形态），它才可能与另一条带秒的链路差几十秒。
/// 两路都带秒却不同秒，代表同一分钟内的两场不同地震。
inline bool quakeTimestampTruncatedToMinute(long long timestampMs) {
    return timestampMs > 0 && timestampMs % 60'000 == 0;
}

/// 两条报次是否应并入同一归档组：
///   · 发震时刻完全相同（同一份报文的不同链路，秒数一致）；或
///   · 其中至少一路被截断到分钟 —— 另一路带秒时天然对不上，只能按分钟认；
/// 并且都要通过 `quakeGroupMatches` 的时刻/震中复核。
///
/// 这是「情报 ↔ 实时预警」能落进同一组报次的关键判据，也保证同一分钟内两场都带秒
/// 的小地震不会被压成一场。
inline bool quakeGroupMergeable(long long timestampA, double latA, double lonA, long long timestampB,
                                double latB, double lonB) {
    if (timestampA == timestampB) return quakeGroupMatches(timestampA, latA, lonA, timestampB, latB, lonB);
    if (!quakeTimestampTruncatedToMinute(timestampA)
        && !quakeTimestampTruncatedToMinute(timestampB))
        return false;
    return quakeGroupMatches(timestampA, latA, lonA, timestampB, latB, lonB);
}

} // namespace komira
