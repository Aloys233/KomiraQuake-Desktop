#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace komira {

namespace detail {

/// 归一化版本号：去前缀 `v`、去预发布/构建后缀（`1.2.0-rc1` → `1.2.0`），按 `.` 切段。
inline QStringList versionParts(const QString& version) {
    QString s = version.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) s.remove(0, 1);
    const qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) s = s.left(dash);
    return s.split(QLatin1Char('.'), Qt::SkipEmptyParts);
}

} // namespace detail

/// candidate 是否比 current 更新。仅按数值段比较，缺失段按 0；相等或更旧返回 false。
inline bool isNewerVersion(const QString& current, const QString& candidate) {
    const QStringList cur = detail::versionParts(current);
    const QStringList cand = detail::versionParts(candidate);
    const qsizetype count = qMax(cur.size(), cand.size());
    for (qsizetype i = 0; i < count; ++i) {
        const int a = i < cur.size() ? cur.at(i).toInt() : 0;
        const int b = i < cand.size() ? cand.at(i).toInt() : 0;
        if (a != b) return b > a;
    }
    return false;
}

} // namespace komira
