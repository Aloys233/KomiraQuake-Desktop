#pragma once

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSvgRenderer>

namespace komira {
// Rasterize the packaged Lucide artwork at the requested device resolution.
// SourceIn tinting uses QPainter, not a shader: icons also work on Qt's software backend.
//
// 结果**按 (name, 颜色, 尺寸) 缓存**：同一枚图标会被几十个 AppIcon 反复请求，而旧实现
// 每次都重新解析一次 SVG（XML 解析 + 路径构建）再栅格化。设置页滚动、主题切换、
// 列表滚动都会触发成片的重复请求 —— 这些开销全部落在 GUI 线程上。
class IconProvider final : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override {
        static const QRegularExpression kNamePattern(QStringLiteral("^[a-z0-9-]+$"));
        const QString name = id.section(QLatin1Char('/'), 0, 0);
        if (!kNamePattern.match(name).hasMatch()) return {};
        const QSize target = requested.isValid() ? requested : QSize(24, 24);

        // 键里包含颜色：同一形状不同色是不同位图（着色是 SourceIn 烘焙进像素的）。
        const QString key = id + QLatin1Char('@') + QString::number(target.width())
                            + QLatin1Char('x') + QString::number(target.height());
        {
            QMutexLocker locker(&mutex_);
            const auto hit = cache_.constFind(key);
            if (hit != cache_.constEnd()) {
                if (size) *size = target;
                return hit.value();
            }
        }

        QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(name));
        if (!svg.isValid()) return {};
        QImage image(target, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            svg.render(&painter);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            const QString tintHex = id.section(QLatin1Char('/'), 1, 1);
            const QColor tint(QStringLiteral("#")
                              + (tintHex.isEmpty() ? QStringLiteral("ff191c1d") : tintHex));
            painter.fillRect(image.rect(), tint.isValid() ? tint : QColor(Qt::black));
        }

        {
            // 上限保护：正常界面最多用到几百种 (图标, 颜色, 尺寸) 组合；
            // 主题切换会成倍增加，故设一个上限，超出即整体清空（代价只是下一帧重栅格化）。
            QMutexLocker locker(&mutex_);
            if (cache_.size() > kMaxEntries) cache_.clear();
            cache_.insert(key, image);
        }
        if (size) *size = target;
        return image;
    }

private:
    static constexpr int kMaxEntries = 512;
    QMutex mutex_;
    QHash<QString, QImage> cache_;
};

inline void configureNativeUi(QQmlEngine& engine) {
    // 打包的 Google Sans 静态拉丁子集（见 tools/import_google_sans.py）。中文字形不在其中，
    // 由 families 回退到系统 CJK；字重由各实例的 OS/2 决定。
    const QStringList fontDirEntries =
        QDir(QStringLiteral(":/fonts")).entryList({QStringLiteral("*.ttf")}, QDir::Files);
    for (const QString& file : fontDirEntries)
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + file);
    QFont font;
    font.setFamilies({QStringLiteral("Google Sans"), QStringLiteral("Noto Sans SC"),
                      QStringLiteral("Noto Sans CJK SC"), QStringLiteral("PingFang SC"),
                      QStringLiteral("sans-serif")});
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    // Google Sans 默认是比例数字；显式开启等宽数字，避免时钟/震级宽度抖动。
    font.setFeature(QFont::Tag("tnum"), 1);
    QGuiApplication::setFont(font);
    // The installed CJK variable font shows excessively emboldened distance-field
    // glyphs on the software scenegraph. Use native glyph rasterization on both
    // backends, rather than compensating with opacity or duplicated text layers.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    engine.addImageProvider(QStringLiteral("icons"), new IconProvider);
}
} // namespace komira
