#ifndef EFFECTOVERLAYS_H
#define EFFECTOVERLAYS_H

#include <QColor>
#include <QImage>
#include <QLinearGradient>
#include <QPen>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRectF>
#include <QSize>
#include <QVector>
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
    bool particleRandomMotion = false;
    QColor particleColor = QColor("#A9D9FF");

    bool glowEnabled = false;
    int glowOpacity = 35, glowSpread = 18, glowSensitivity = 60;
    int glowSmoothing = 75;
    QColor glowColors[4] = {QColor("#00D5FF"), QColor("#376BFF"),
                            QColor("#D64DFF"), QColor("#00D5FF")};
    QVector<QPair<qreal, QColor>> glowGradientStops = {
        {0.0, QColor("#00D5FF")}, {0.33, QColor("#376BFF")},
        {0.66, QColor("#D64DFF")}, {1.0, QColor("#00D5FF")}
    };
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
            const qreal depth = 0.38 + hash(i * 11 + 3) * 1.28;
            const qreal sizeRoll = hash(i * 11 + 6);
            const qreal sizeFactor = sizeRoll < 0.68
                ? 0.48 + sizeRoll * 0.78 : 1.0 + (sizeRoll - 0.68) * 3.0;
            const qreal baseVelocity = (0.018 + hash(i * 11 + 4) * 0.082) *
                                       (s.rainSpeed / 50.0) * depth;
            const qreal rhythm = 0.55 + hash(i * 11 + 7) * 1.25;
            const qreal phase = hash(i * 11 + 8) * 6.28318530718;
            const qreal travel = time * baseVelocity *
                (0.82 + 0.20 * qSin(time * rhythm + phase) +
                 0.10 * qSin(time * rhythm * 0.37 + phase * 1.7));
            qreal y = std::fmod(seedY + travel, 1.18) - 0.09;
            qreal x = seedX + qSin(time * (0.25 + rhythm * 0.18) + phase) *
                      (0.003 + sizeFactor * 0.0018) * depth;
            const qreal radius = minSide *
                (0.00135 + s.rainSize * 0.000068) * depth * sizeFactor;
            const qreal alpha = qBound(0.0, (s.rainOpacity / 100.0) *
                (0.45 + hash(i * 11 + 5) * 0.45) * audioBoost, 0.92);
            const QPointF center(area.left() + x * area.width(),
                                 area.top() + y * area.height());
            const qreal localVelocity = baseVelocity *
                (0.82 + 0.20 * qSin(time * rhythm + phase) +
                 0.10 * qSin(time * rhythm * 0.37 + phase * 1.7));
            const qreal streak = radius * (0.65 + s.rainStreakLength / 24.0) *
                                 (0.45 + localVelocity * 5.0) *
                                 (0.65 + sizeFactor * 0.22);
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
            const qreal aspect = 0.78 + sizeFactor * 0.13;
            p.drawImage(QRectF(center.x() - radius * aspect,
                               center.y() - radius * (1.0 + sizeFactor * 0.12),
                               radius * 2.0 * aspect,
                               radius * 2.0 * (1.0 + sizeFactor * 0.12)),
                        m_rainSprite);
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
            qreal x = seedX, y = 1.08 - std::fmod(seedY + time * speed, 1.18);
            if (s.particleRandomMotion) {
                const qreal phaseX = hash(i * 13 + 6) * 6.28318530718;
                const qreal phaseY = hash(i * 13 + 7) * 6.28318530718;
                const qreal rateX = 0.16 + hash(i * 13 + 8) * 0.52;
                const qreal rateY = 0.12 + hash(i * 13 + 9) * 0.42;
                const qreal drift = (hash(i * 13 + 10) - 0.5) * 0.018;
                x += qSin(time * rateX + phaseX) * (0.035 + depth * 0.025)
                   + qSin(time * rateY * 0.43 + phaseY) * 0.018 + time * drift;
                y += qCos(time * rateY + phaseY) * (0.025 + depth * 0.018);
                x = std::fmod(x + 2.0, 1.0);
                y = std::fmod(y + 2.0, 1.0);
            } else {
                x += qSin(time * 0.35 + i * 2.1) * 0.018 * depth;
            }
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
        const qreal level = qBound(0.0,
            audioEnergy * (0.25 + s.glowSensitivity / 50.0), 1.0);
        const qreal strength = (s.glowOpacity / 100.0) * level;
        if (strength <= 0.001) return;
        QVector<QPair<qreal, QColor>> stops = s.glowGradientStops;
        if (stops.size() < 2)
            stops = {{0.0, s.glowColors[0]}, {0.33, s.glowColors[1]},
                     {0.66, s.glowColors[2]}, {1.0, s.glowColors[3]}};
        std::sort(stops.begin(), stops.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        const qreal phase = std::fmod(level * 0.42, 1.0);
        const auto gradientColor = [&stops, phase](qreal position) {
            const qreal t = std::fmod(position + phase + 1.0, 1.0);
            for (int i = 0; i + 1 < stops.size(); ++i) {
                if (t <= stops[i + 1].first) {
                    const qreal span = qMax<qreal>(0.0001, stops[i + 1].first - stops[i].first);
                    const qreal u = qBound(0.0, (t - stops[i].first) / span, 1.0);
                    return QColor::fromRgbF(
                        stops[i].second.redF() * (1-u) + stops[i+1].second.redF() * u,
                        stops[i].second.greenF() * (1-u) + stops[i+1].second.greenF() * u,
                        stops[i].second.blueF() * (1-u) + stops[i+1].second.blueF() * u);
                }
            }
            return stops.last().second;
        };
        const qreal minSide = qMin(area.width(), area.height());
        const qreal radiusBase = minSide * (0.025 + qBound(1, s.glowSpread, 50) * 0.006);
        const int bloomCount = 14;
        p.save();
        p.setClipRect(area);
        p.setCompositionMode(QPainter::CompositionMode_Screen);
        for (int i = 0; i < bloomCount; ++i) {
            const qreal u = (qreal(i) + 0.5) / bloomCount;
            const qreal edge = std::fmod(i * 0.61803398875, 4.0);
            QPointF center;
            if (edge < 1.0) center = QPointF(area.left() + u * area.width(), area.top());
            else if (edge < 2.0) center = QPointF(area.right(), area.top() + u * area.height());
            else if (edge < 3.0) center = QPointF(area.right() - u * area.width(), area.bottom());
            else center = QPointF(area.left(), area.bottom() - u * area.height());
            const qreal variance = 0.62 + hash(i * 19 + 4) * 0.92;
            const qreal radius = radiusBase * variance;
            const QColor color = gradientColor(u + edge * 0.13);
            QColor core = color; core.setAlphaF(qBound(0.0, strength * 0.46, 0.58));
            QColor middle = color; middle.setAlphaF(qBound(0.0, strength * 0.24, 0.34));
            QColor clear = color; clear.setAlpha(0);
            QRadialGradient bloom(center, radius);
            bloom.setColorAt(0.0, core);
            bloom.setColorAt(0.34, middle);
            bloom.setColorAt(1.0, clear);
            const qreal aspect = 0.58 + hash(i * 19 + 5) * 0.88;
            p.setPen(Qt::NoPen);
            p.setBrush(bloom);
            p.drawEllipse(QRectF(center.x() - radius * aspect, center.y() - radius,
                                 radius * 2.0 * aspect, radius * 2.0));
        }
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
