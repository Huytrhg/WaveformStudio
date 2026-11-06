#ifndef EFFECTOVERLAYS_H
#define EFFECTOVERLAYS_H

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRectF>
#include <QSize>
#include <QtMath>
#include <cmath>

struct EffectOverlaySettings {
    bool rainGlassEnabled = false;
    int rainCount = 70, rainSize = 45, rainSpeed = 55, rainOpacity = 55;
    int rainStreakLength = 35, rainAudioSensitivity = 55;
    bool rainAudioReactive = false;
    QColor rainColor = QColor("#D9EEFF");

    bool particlesEnabled = false;
    int particleCount = 45, particleSize = 35, particleSpeed = 35;
    int particleOpacity = 65, particleGlow = 60, particleTwinkle = 25;
    int particleSensitivity = 55;
    bool particlesAudioReactive = false;
    QColor particleColor = QColor("#A9D9FF");

    bool glowEnabled = false;
    int glowOpacity = 35, glowSpread = 18, glowSensitivity = 60;
    int glowSmoothing = 75;
    QColor glowColors[4] = {QColor("#00D5FF"), QColor("#376BFF"),
                            QColor("#D64DFF"), QColor("#00D5FF")};
};

// Reuses pre-rendered particle and droplet sprites. Animation positions are
// calculated from absolute time, so Preview and Export do not share timer state.
class EffectOverlayRenderer {
public:
    void reset() {
        m_rainSprite = QImage();
        m_particleSprite = QImage();
    }

    static qreal hash(int n) {
        const qreal value = qSin(n * 127.1 + 78.233) * 43758.5453;
        return value - qFloor(value);
    }

    void paintRainGlass(QPainter &p, const QRectF &area,
                        const EffectOverlaySettings &s, qreal time,
                        qreal audioEnergy = 0.0) {
        if (!s.rainGlassEnabled || area.isEmpty() || s.rainCount <= 0 ||
            s.rainOpacity <= 0) return;
        ensureRainSprite(s.rainColor);
        const qreal minSide = qMin(area.width(), area.height());
        const qreal audioBoost = s.rainAudioReactive
            ? 1.0 + qBound(0.0, audioEnergy * (0.25 + s.rainAudioSensitivity / 100.0), 0.8)
            : 1.0;
        p.save();
        p.setClipRect(area);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int i = 0; i < qBound(1, s.rainCount, 180); ++i) {
            const qreal seedX = hash(i * 11 + 1);
            const qreal seedY = hash(i * 11 + 2);
            const qreal depth = 0.55 + hash(i * 11 + 3) * 0.9;
            const qreal velocity = (0.035 + hash(i * 11 + 4) * 0.075) *
                                   (s.rainSpeed / 50.0) * depth;
            qreal y = std::fmod(seedY + time * velocity, 1.18) - 0.09;
            qreal x = seedX + qSin(time * 0.7 + i * 1.71) * 0.004 * depth;
            const qreal radius = minSide * (0.0018 + s.rainSize * 0.000075) * depth;
            const qreal alpha = qBound(0.0, (s.rainOpacity / 100.0) *
                (0.45 + hash(i * 11 + 5) * 0.45) * audioBoost, 0.92);
            const QPointF center(area.left() + x * area.width(),
                                 area.top() + y * area.height());
            const qreal streak = radius * (1.0 + s.rainStreakLength / 18.0) *
                                 (0.55 + velocity * 3.0);
            QPen trail(s.rainColor);
            QColor trailColor = s.rainColor;
            trailColor.setAlphaF(alpha * 0.34);
            trail.setColor(trailColor);
            trail.setWidthF(qMax<qreal>(0.6, radius * 0.16));
            p.setPen(trail);
            p.drawLine(QPointF(center.x(), center.y() - streak),
                       QPointF(center.x() - radius * 0.12, center.y() - radius * 0.15));
            p.setPen(Qt::NoPen);
            p.setOpacity(alpha);
            p.drawImage(QRectF(center.x() - radius, center.y() - radius,
                               radius * 2.0, radius * 2.0), m_rainSprite);
        }
        p.restore();
    }

    void paintParticles(QPainter &p, const QRectF &area,
                        const EffectOverlaySettings &s, qreal time,
                        qreal audioEnergy = 0.0) {
        if (!s.particlesEnabled || area.isEmpty() || s.particleCount <= 0 ||
            s.particleOpacity <= 0) return;
        ensureParticleSprite(s.particleColor, s.particleGlow);
        const qreal minSide = qMin(area.width(), area.height());
        const qreal energy = s.particlesAudioReactive
            ? qBound(0.0, audioEnergy * (0.25 + s.particleSensitivity / 50.0), 1.0)
            : 0.0;
        p.save();
        p.setClipRect(area);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int i = 0; i < qBound(1, s.particleCount, 180); ++i) {
            const qreal seedX = hash(i * 13 + 1);
            const qreal seedY = hash(i * 13 + 2);
            const qreal depth = 0.45 + hash(i * 13 + 3) * 0.9;
            const qreal speed = (0.012 + hash(i * 13 + 4) * 0.028) *
                                (s.particleSpeed / 50.0) * depth;
            const qreal y = 1.08 - std::fmod(seedY + time * speed, 1.18);
            const qreal x = seedX + qSin(time * 0.35 + i * 2.1) * 0.018 * depth;
            const qreal pulse = 1.0 - (s.particleTwinkle / 100.0) * 0.45 +
                (s.particleTwinkle / 100.0) * 0.45 *
                (0.5 + 0.5 * qSin(time * (1.0 + hash(i * 13 + 5) * 2.5) + i));
            const qreal radius = minSide * (0.0025 + s.particleSize * 0.00010) *
                                 depth * (1.0 + energy * 0.35);
            const qreal alpha = qBound(0.0, (s.particleOpacity / 100.0) *
                pulse * (0.55 + energy * 0.45), 0.95);
            const QPointF center(area.left() + x * area.width(),
                                 area.top() + y * area.height());
            p.setOpacity(alpha);
            p.drawImage(QRectF(center.x() - radius, center.y() - radius,
                               radius * 2.0, radius * 2.0), m_particleSprite);
        }
        p.restore();
    }

    static void paintGlow(QPainter &p, const QRectF &area,
                          const EffectOverlaySettings &s, qreal audioEnergy = 0.0) {
        if (!s.glowEnabled || area.isEmpty() || s.glowOpacity <= 0) return;
        const qreal level = s.glowSensitivity <= 0 ? 1.0 :
            qBound(0.0, audioEnergy * (0.25 + s.glowSensitivity / 50.0), 1.0);
        const qreal strength = (s.glowOpacity / 100.0) * level;
        if (strength <= 0.001) return;
        const qreal edgeWidth = qMin(area.width(), area.height()) *
                                (qBound(1, s.glowSpread, 50) / 100.0);
        const QColor clear(0, 0, 0, 0);
        const auto tint = [strength](QColor c, qreal factor) {
            c.setAlphaF(qBound(0.0, strength * factor, 0.85));
            return c;
        };
        const QRectF top(area.left(), area.top(), area.width(), edgeWidth);
        const QRectF bottom(area.left(), area.bottom() - edgeWidth, area.width(), edgeWidth);
        const QRectF left(area.left(), area.top(), edgeWidth, area.height());
        const QRectF right(area.right() - edgeWidth, area.top(), edgeWidth, area.height());
        QLinearGradient tg(top.topLeft(), top.bottomLeft());
        tg.setColorAt(0.0, tint(s.glowColors[0], 1.0));
        tg.setColorAt(0.38, tint(s.glowColors[0], 0.45));
        tg.setColorAt(1.0, clear);
        QLinearGradient bg(bottom.bottomLeft(), bottom.topLeft());
        bg.setColorAt(0.0, tint(s.glowColors[2], 1.0));
        bg.setColorAt(0.38, tint(s.glowColors[2], 0.45));
        bg.setColorAt(1.0, clear);
        QLinearGradient lg(left.topLeft(), left.topRight());
        lg.setColorAt(0.0, tint(s.glowColors[1], 1.0));
        lg.setColorAt(0.38, tint(s.glowColors[1], 0.45));
        lg.setColorAt(1.0, clear);
        QLinearGradient rg(right.topRight(), right.topLeft());
        rg.setColorAt(0.0, tint(s.glowColors[3], 1.0));
        rg.setColorAt(0.38, tint(s.glowColors[3], 0.45));
        rg.setColorAt(1.0, clear);
        p.save();
        p.setClipRect(area);
        p.fillRect(top, tg);
        p.fillRect(bottom, bg);
        p.fillRect(left, lg);
        p.fillRect(right, rg);
        p.restore();
    }

private:
    QImage m_rainSprite;
    QImage m_particleSprite;
    QColor m_rainSpriteColor;
    QColor m_particleSpriteColor;
    int m_particleSpriteGlow = -1;

    void ensureRainSprite(const QColor &color) {
        if (!m_rainSprite.isNull() && m_rainSpriteColor == color) return;
        constexpr int side = 96;
        m_rainSprite = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        m_rainSprite.fill(Qt::transparent);
        QPainter p(&m_rainSprite);
        QRadialGradient g(QPointF(36, 32), 42);
        QColor center = color.lighter(145); center.setAlpha(190);
        QColor rim = color.darker(165); rim.setAlpha(135);
        QColor clear = color; clear.setAlpha(0);
        g.setColorAt(0.0, center);
        g.setColorAt(0.42, color);
        g.setColorAt(0.78, rim);
        g.setColorAt(1.0, clear);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QRectF(8, 7, 80, 82));
        p.setPen(QPen(QColor(255, 255, 255, 145), 3));
        p.drawArc(QRect(20, 17, 48, 55), 35 * 16, 105 * 16);
        m_rainSpriteColor = color;
    }

    void ensureParticleSprite(const QColor &color, int glow) {
        if (!m_particleSprite.isNull() && m_particleSpriteColor == color &&
            m_particleSpriteGlow == glow) return;
        constexpr int side = 96;
        m_particleSprite = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        m_particleSprite.fill(Qt::transparent);
        QPainter p(&m_particleSprite);
        const qreal halo = qBound(0, glow, 100) / 100.0;
        QRadialGradient g(QPointF(48, 48), 47);
        QColor outer = color; outer.setAlpha(qRound(40 + halo * 85));
        QColor middle = color; middle.setAlpha(qRound(95 + halo * 95));
        QColor core = color.lighter(155); core.setAlpha(235);
        QColor clear = color; clear.setAlpha(0);
        g.setColorAt(0.0, core);
        g.setColorAt(0.16, middle);
        g.setColorAt(0.45, outer);
        g.setColorAt(1.0, clear);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QRectF(1, 1, 94, 94));
        m_particleSpriteColor = color;
        m_particleSpriteGlow = glow;
    }
};

#endif // EFFECTOVERLAYS_H
