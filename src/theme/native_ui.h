#pragma once

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSvgRenderer>

namespace komira {
// Rasterize the packaged Lucide artwork at the requested device resolution.
// SourceIn tinting uses QPainter, not a shader: icons also work on Qt's software backend.
class IconProvider final : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override {
        const auto parts = id.split('/');
        const QString name = parts.value(0);
        if (!QRegularExpression(QStringLiteral("^[a-z0-9-]+$")).match(name).hasMatch()) return {};
        QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(name));
        if (!svg.isValid()) return {};
        const QSize target = requested.isValid() ? requested : QSize(24, 24);
        QImage image(target, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        svg.render(&painter);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        const QColor tint(QStringLiteral("#") + parts.value(1, QStringLiteral("ff191c1d")));
        painter.fillRect(image.rect(), tint.isValid() ? tint : QColor(Qt::black));
        if (size) *size = target;
        return image;
    }
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
