#ifndef SPECTRUMRENDERER_H
#define SPECTRUMRENDERER_H

// Shared, offscreen Spectrum drawing engine.
// B1: introduced independently; B2 will route Preview through it,
// and B3 will use the same drawing path for exported video frames.
#include <QColor>
#include <QImage>
#include <QElapsedTimer>
#include <QDebug>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QVector>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

struct SpectrumRenderSettings {
    int mode = 0; // 0 Bars, 1 Lines, 2 Circular, 3 Filled, 4 Dancing Particles
    int positionX = 50, positionY = 78;
    int opacity = 92, scale = 72, columns = 64;
    int height = 100, thickness = 75;
    int glow = 0, glowRadius = 50, glowIntensity = 100;
    bool peakHold = false;
    bool gradient = true;
    int interpolation = 1;
    qreal gradientPhase = 0;
    // B4.3.4: independent horizontal travel phase for the four-line waveform.
    int curveTravelSpeed = 35;
    int curveReleaseSpeed = 80; // 0 slow release, 100 near-instant release
    qreal curvePhase = 0;
    QColor solidColor = QColor(0x35B8FF);
    QVector<QPair<qreal, QColor>> stops = {
        {0.0, QColor(0x00D5FF)},
        {0.5, QColor(0x376BFF)},
        {1.0, QColor(0xD64DFF)}
    };
};

class SpectrumRenderer {
public:
    static qreal reflected(qreal phase) {
        qreal t = std::fmod(phase, 2.0);
        if (t < 0) t += 2.0;
        return t <= 1.0 ? t : 2.0 - t;
    }

    static QColor colorAt(qreal t, const SpectrumRenderSettings &s) {
        if (!s.gradient || s.stops.isEmpty()) return s.solidColor;
        const auto &stops = s.stops;
        if (t <= stops.first().first) return stops.first().second;
        if (t >= stops.last().first) return stops.last().second;
        for (int i = 1; i < stops.size(); ++i) {
            const auto &a = stops[i - 1], &b = stops[i];
            if (t > b.first) continue;
            qreal u = qBound(0.0, (t - a.first) / qMax<qreal>(0.0001, b.first - a.first), 1.0);
            if (s.interpolation == 1) u = u * u * (3.0 - 2.0 * u);
            if (s.interpolation == 2) {
                const QColor ah = a.second.toHsv(), bh = b.second.toHsv();
                qreal h1 = qMax(0.0, ah.hueF()), h2 = qMax(0.0, bh.hueF());
                qreal dh = h2 - h1;
                if (dh > 0.5) dh -= 1.0;
                if (dh < -0.5) dh += 1.0;
                return QColor::fromHsvF(std::fmod(h1 + dh * u + 1.0, 1.0),
                    ah.saturationF() + (bh.saturationF() - ah.saturationF()) * u,
                    ah.valueF() + (bh.valueF() - ah.valueF()) * u,
                    a.second.alphaF() + (b.second.alphaF() - a.second.alphaF()) * u);
            }
            return QColor::fromRgbF(
                a.second.redF() + (b.second.redF() - a.second.redF()) * u,
                a.second.greenF() + (b.second.greenF() - a.second.greenF()) * u,
                a.second.blueF() + (b.second.blueF() - a.second.blueF()) * u,
                a.second.alphaF() + (b.second.alphaF() - a.second.alphaF()) * u);
        }
        return stops.last().second;
    }

    static QColor bandColor(int index, int count, const SpectrumRenderSettings &s) {
        const qreal t = count <= 1 ? 0 : qreal(index) / (count - 1);
        return colorAt(reflected(t + s.gradientPhase), s);
    }

    // B4.3.3: separable box-blur approximation to a Gaussian. Run on a
    // reduced-resolution layer for a soft halo at a reasonable export cost.
    static QImage blurGlowLayer(const QImage &source, int radius) {
        if (source.isNull() || radius <= 0) return source;
        QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const int w = image.width(), h = image.height();
        QImage temp(image.size(), QImage::Format_ARGB32_Premultiplied);
        radius = qBound(1, radius, 32);
        for (int y = 0; y < h; ++y) {
            const QRgb *src = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            QRgb *dst = reinterpret_cast<QRgb *>(temp.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int a=0,r=0,g=0,b=0,n=0;
                for (int k=qMax(0,x-radius); k<=qMin(w-1,x+radius); ++k) {
                    const QRgb px=src[k]; a+=qAlpha(px); r+=qRed(px); g+=qGreen(px); b+=qBlue(px); ++n;
                }
                dst[x]=qRgba(r/n,g/n,b/n,a/n);
            }
        }
        for (int y = 0; y < h; ++y) {
            QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int a=0,r=0,g=0,b=0,n=0;
                for (int k=qMax(0,y-radius); k<=qMin(h-1,y+radius); ++k) {
                    const QRgb px=reinterpret_cast<const QRgb *>(temp.constScanLine(k))[x];
                    a+=qAlpha(px); r+=qRed(px); g+=qGreen(px); b+=qBlue(px); ++n;
                }
                dst[x]=qRgba(r/n,g/n,b/n,a/n);
            }
        }
        return image;
    }

    static void paint(QPainter &p, const QRectF &stage,
                      const QVector<float> &bands, const SpectrumRenderSettings &s,
                      const QVector<float> &peaks = {},
                                QImage::Format outputFormat = QImage::Format_ARGB32_Premultiplied) {
        if (bands.isEmpty() || stage.width() < 2 || stage.height() < 2) return;
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setClipRect(stage);
        p.setOpacity(qBound(0, s.opacity, 100) / 100.0);
        // Draw a true soft glow layer first, then draw crisp foreground shapes
        // below with glow disabled to avoid the old hard-edged halo.
        if (s.glow > 0 && s.glowIntensity > 0 &&
            (s.mode == 1 || s.mode == 3)) {
            const QSize layerSize(qMax(1, int(stage.width())), qMax(1, int(stage.height())));
            QImage glowLayer(layerSize, QImage::Format_ARGB32_Premultiplied);
            glowLayer.fill(Qt::transparent);
            SpectrumRenderSettings glowSettings = s;
            glowSettings.glow = 0;
            glowSettings.opacity = 100;
            if (s.mode == 1) {
                qreal audioPeak = 0.0;
                for (float band : bands) audioPeak = qMax(audioPeak, qBound(0.0, qreal(band), 1.0));
                // Let the line halo breathe with the music, while retaining a small idle glow.
                glowSettings.glowIntensity = qRound(s.glowIntensity * (0.12 + 0.88 * audioPeak));
            }
            QPainter gp(&glowLayer);
            gp.setRenderHint(QPainter::Antialiasing);
            gp.setOpacity(qBound(0, s.opacity, 100) / 100.0 *
                          qBound(0, s.glow, 100) / 100.0 *
                          qBound(0, s.glowIntensity, 100) / 100.0);
            paint(gp, QRectF(QPointF(0, 0), QSizeF(layerSize)), bands, glowSettings, peaks);
            gp.end();
            const int blurRadius = qBound(2, qRound((2.0 + 18.0 * s.glowRadius / 100.0) *
                qMin(1.0, stage.width() / 900.0)), 24);
            const QImage blurred = blurGlowLayer(glowLayer, blurRadius);
            p.save();
            p.setOpacity(1.0);
            p.drawImage(stage.topLeft(), blurred);
            p.restore();
            SpectrumRenderSettings coreSettings = s;
            coreSettings.glow = 0;
            paint(p, stage, bands, coreSettings, peaks);
            p.restore();
            return;
        }
        const qreal cx = stage.left() + stage.width() * s.positionX / 100.0;
        const qreal cy = stage.top() + stage.height() * s.positionY / 100.0;
        const qreal maxW = stage.width() * s.scale / 100.0;
        const qreal maxH = stage.height() * 0.42 * (s.scale / 72.0) * (s.height / 100.0);
        const qreal startX = cx - maxW / 2.0;
        auto sample = [](const QVector<float> &v, qreal index) -> qreal {
            if (v.isEmpty()) return 0;
            const int lo = qBound(0, int(index), int(v.size()) - 1);
            const int hi = qMin(lo + 1, int(v.size()) - 1);
            return qBound(0.0, qreal(v[lo]) * (1 - (index - lo)) +
                qreal(v[hi]) * (index - lo), 1.0);
        };
        auto horizontalGradient = [&]() {
            QLinearGradient g(startX, cy, startX + maxW, cy);
            for (int j = 0; j <= 48; ++j) {
                const qreal t = j / 48.0;
                g.setColorAt(t, s.gradient ?
                    colorAt(reflected(t + s.gradientPhase), s) : s.solidColor);
            }
            return g;
        };
        auto smoothPath = [](const QVector<QPointF> &points) {
            QPainterPath path;
            if (points.isEmpty()) return path;
            path.moveTo(points.first());
            for (int i = 1; i < points.size(); ++i) {
                const QPointF a = points[i - 1], b = points[i];
                path.quadTo(a, QPointF((a.x() + b.x()) * 0.5,
                                      (a.y() + b.y()) * 0.5));
            }
            if (points.size() > 1) path.lineTo(points.last());
            return path;
        };

        if (s.mode == 0) {
            const int count = qBound(8, s.columns, 128);
            const qreal cell = maxW / count;
            const qreal barW = qMax(0.6, cell * s.thickness / 100.0);
            p.setPen(Qt::NoPen);
            for (int i = 0; i < count; ++i) {
                const qreal idx = qreal(i) * (bands.size() - 1) / qMax(1, count - 1);
                const qreal h = qMax(1.0, sample(bands, idx) * maxH);
                const QRectF bar(startX + i * cell + (cell - barW) / 2.0,
                                 cy - h, barW, h);
                const QColor c = bandColor(i, count, s);
                if (s.glow > 0 && s.glowIntensity > 0) {
                    // B4.3.2: broad-to-tight translucent layers form a
                    // soft outward falloff while leaving the core crisp.
                    const qreal strength = s.glow / 100.0 * s.glowIntensity / 100.0;
                    const qreal spread = 1.0 + 12.0 * s.glowRadius / 100.0;
                    const qreal radii[] = {1.0, 0.72, 0.46, 0.22};
                    const qreal alphas[] = {0.045, 0.085, 0.16, 0.30};
                    for (int layer = 0; layer < 4; ++layer) {
                        QColor halo = c;
                        halo.setAlpha(qBound(0, qRound(255.0 * strength * alphas[layer]), 90));
                        p.setBrush(halo);
                        const qreal r = spread * radii[layer];
                        p.drawRoundedRect(bar.adjusted(-r, -r, r, r), 3, 3);
                    }
                }
                p.setBrush(c);
                p.drawRoundedRect(bar, qMin(2.5, barW / 2.0), qMin(2.5, barW / 2.0));
                if (s.peakHold && !peaks.isEmpty()) {
                    p.setBrush(c);
                    p.drawRect(QRectF(bar.left(), cy - qMax(1.0, sample(peaks,
                        qreal(i) * (peaks.size() - 1) / qMax(1, count - 1)) * maxH) - 3,
                        barW, 1.6));
                }
            }
        } else if (s.mode == 4) {
            // Dancing Particles: Spectrum FFT sampling and shared frame/export
            // renderer. Each band is a hairline stem capped by a round dot.
            const int count = qBound(8, s.columns, 128);
            const qreal cell = maxW / count;
            const qreal stemWidth = qMax<qreal>(0.75,
                qMin<qreal>(3.0, stage.width() * 0.0011 * s.thickness / 75.0));
            // Minimum particle diameter is twice the previous default.
            // The cap grows continuously with its own FFT band energy.
            const qreal minDotRadius = qMax<qreal>(3.6,
                qMin<qreal>(10.0, stemWidth * 3.10));
            const qreal strength = s.glow / 100.0 * s.glowIntensity / 100.0;
            const qreal spread = 1.0 + 8.0 * s.glowRadius / 100.0;
            for (int i = 0; i < count; ++i) {
                const qreal index = qreal(i) * (bands.size() - 1) / qMax(1, count - 1);
                const qreal level = qBound(0.0, sample(bands, index), 1.0);
                const qreal dotRadius = minDotRadius * (1.0 + 0.85 * level);
                const qreal height = qMax<qreal>(1.0, level * maxH);
                const qreal x = startX + (i + 0.5) * cell;
                const qreal y = cy - height;
                const QColor color = bandColor(i, count, s);
                if (strength > 0) {
                    QColor halo = color;
                    halo.setAlphaF(qBound(0.0, strength * 0.16, 1.0));
                    p.setPen(QPen(halo, stemWidth + spread * 0.65,
                                  Qt::SolidLine, Qt::RoundCap));
                    p.drawLine(QPointF(x, cy), QPointF(x, y));
                    p.setPen(Qt::NoPen);
                    p.setBrush(halo);
                    p.drawEllipse(QPointF(x, y), dotRadius + spread * 0.65,
                                  dotRadius + spread * 0.65);
                }
                p.setPen(QPen(color, stemWidth, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(x, cy), QPointF(x, y));
                p.setPen(Qt::NoPen);
                p.setBrush(color);
                p.drawEllipse(QPointF(x, y), dotRadius, dotRadius);
            }
        } else if (s.mode == 1) {
            // Four coincident straight lines at silence. During music, every
            // line has TWO localized bends: a full-strength bend and another
            // at precisely one third of that strength.
            qreal audioPeak = 0.0;
            for (float band : bands)
                audioPeak = qMax(audioPeak, qBound(0.0, qreal(band), 1.0));

            // No peak-hold or temporal smoothing here: each frame's current
            // audio energy determines the curve height, so it straightens
            // immediately when the signal falls.
            const qreal amplitude = maxH * 1.74 * audioPeak * audioPeak;
            const qreal travel = 2.0 * reflected(s.curvePhase) - 1.0;
            // Both bend locations travel TOGETHER; the endpoints never move.
            const qreal travelOffset = 0.095 * travel;
            const qreal bendCenters[2] = {0.32 + travelOffset,
                                           0.68 + travelOffset};
            const qreal bendStrengths[2] = {1.0, 1.0 / 3.0};
            // Offsetting two equally energetic paths causes them to cross
            // naturally on the slopes of each localized bend.
            const qreal lineOffsets[2] = {-0.018, 0.018};
            const qreal bendHalfWidth = 0.105;
            const qreal lineWidth = qMax<qreal>(1.25,
                stage.width() * 0.0018 * s.thickness / 75.0);
            const QLinearGradient gradient = horizontalGradient();

            auto makeLine = [&](int lineIndex, qreal verticalSign) {
                QPainterPath path;
                constexpr int segments = 256;
                for (int i = 0; i <= segments; ++i) {
                    const qreal t = qreal(i) / segments;
                    qreal bend = 0.0;
                    for (int k = 0; k < 2; ++k) {
                        const qreal u = (t - bendCenters[k] -
                            lineOffsets[lineIndex]) / bendHalfWidth;
                        if (qAbs(u) < 1.0) {
                            const qreal falloff = 1.0 - u * u;
                            bend += bendStrengths[k] * falloff * falloff;
                        }
                    }
                    const qreal x = startX + maxW * t;
                    const qreal y = cy + verticalSign * amplitude * bend;
                    if (i == 0) path.moveTo(x, y);
                    else path.lineTo(x, y);
                }
                return path;
            };

            const QPainterPath line1 = makeLine(0, -1.0);
            const QPainterPath line2 = makeLine(1, -1.0);
            const QPainterPath line3 = makeLine(0,  1.0);
            const QPainterPath line4 = makeLine(1,  1.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(gradient, lineWidth, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(line1);
            p.drawPath(line2);
            p.drawPath(line3);
            p.drawPath(line4);
        } else if (s.mode == 2) {
            const qreal radius = qMin(stage.width(), stage.height()) * 0.17 * s.scale / 72.0;
            const qreal barMax = qMin(stage.width(), stage.height()) * 0.12 *
                s.scale / 72.0 * s.height / 100.0;
            p.setPen(QPen(QColor(255, 255, 255, 35), 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(cx, cy), radius, radius);
            for (int i = 0; i < bands.size(); ++i) {
                const qreal a = qDegreesToRadians(360.0 * i / bands.size() - 90.0);
                const qreal outer = radius + qMax(1.0, sample(bands, i) * barMax);
                const QPointF innerPoint(cx + qCos(a) * radius, cy + qSin(a) * radius);
                const QPointF outerPoint(cx + qCos(a) * outer, cy + qSin(a) * outer);
                const QColor coreColor = bandColor(i, bands.size(), s);
                const qreal coreWidth = qMax(1.0, stage.width() * 0.004 * s.thickness / 100.0);
                if (s.glow > 0 && s.glowIntensity > 0) {
                    const qreal strength = s.glow / 100.0 * s.glowIntensity / 100.0;
                    const qreal spread = 2.0 + 16.0 * s.glowRadius / 100.0;
                    const qreal widths[] = {1.0, 0.65, 0.32};
                    const qreal alphas[] = {0.045, 0.11, 0.25};
                    for (int layer = 0; layer < 3; ++layer) {
                        QColor halo = coreColor;
                        halo.setAlpha(qBound(0, qRound(255.0 * strength * alphas[layer]), 90));
                        p.setPen(QPen(halo, coreWidth + spread * widths[layer],
                                      Qt::SolidLine, Qt::RoundCap));
                        p.drawLine(innerPoint, outerPoint);
                    }
                }
                p.setPen(QPen(coreColor, coreWidth, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(innerPoint, outerPoint);
            }
        } else {
            const int n = qMax(2, int(bands.size()));
            // B4.1.5: reserve exact point capacity up front. This avoids
            // repeated QVector reallocations while constructing every frame's
            // waveform/filled paths, without changing point order or geometry.
            QVector<QPointF> upper, lower;
            upper.reserve(n);
            lower.reserve(n);
            for (int i = 0; i < n; ++i) {
                const qreal t = qreal(i) / (n - 1);
                const qreal taper = s.mode == 3 ?
                    qMin(qBound(0.0, t / 0.08, 1.0),
                         qBound(0.0, (1.0 - t) / 0.08, 1.0)) : 1.0;
                const qreal amp = qMax(s.mode == 1 ? 0.75 : 0.0,
                    sample(bands, qreal(i) * (bands.size() - 1) / (n - 1)) *
                    maxH * 0.46 * taper);
                const qreal x = startX + maxW * t;
                upper.push_back(QPointF(x, cy - amp));
                lower.push_back(QPointF(x, cy + amp));
            }
            const QPainterPath top = smoothPath(upper), bottom = smoothPath(lower);
            const QLinearGradient gradient = horizontalGradient();
            if (s.mode == 1) {
                if (s.glow > 0 && s.glowIntensity > 0) {
                    QColor halo = bandColor(bands.size() / 2, bands.size(), s);
                    halo.setAlpha(qBound(0, qRound(22.0 * s.glow *
                        s.glowIntensity / 10000.0), 22));
                    p.setPen(QPen(halo, 1.2 + 5.0 * s.glowRadius / 100.0));
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(top);
                    p.drawPath(bottom);
                }
                p.setPen(QPen(gradient, qMax(1.25,
                    stage.width() * 0.0018 * s.thickness / 75.0),
                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(top);
                p.drawPath(bottom);
            } else {
                QPainterPath filled = top;
                for (int i = lower.size() - 1; i >= 0; --i)
                    filled.lineTo(lower[i]);
                filled.closeSubpath();
                p.setPen(Qt::NoPen);
                p.setBrush(gradient);
                p.drawPath(filled);
                if (s.glow > 0 && s.glowIntensity > 0) {
                    QColor halo = bandColor(bands.size() / 2, bands.size(), s);
                    halo.setAlpha(qBound(0, qRound(60.0 * s.glow *
                        s.glowIntensity / 10000.0), 60));
                    p.setPen(QPen(halo, 2.0 + 16.0 * s.glowRadius / 100.0,
                                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(filled);
                }
            }
        }
        p.restore();
    }

    // B3.1: render a complete output frame from the original video frame.
    // This function is deterministic for a given frame, FFT bands and settings,
    // making it suitable for both preview-independent export and repeatable tests.
    static QImage renderFrame(const QImage &videoFrame,
                              const QVector<float> &bands,
                              const SpectrumRenderSettings &settings,
                              const QVector<float> &peaks = {}) {
        if (videoFrame.isNull()) return {};
        QImage frame = videoFrame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&frame);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        paint(painter, QRectF(QPointF(0, 0), QSizeF(frame.size())), bands, settings, peaks);
        painter.end();
        return frame;
    }

    // Render an RGBA overlay only. Transparent pixels preserve the source video
    // when composited by FFmpeg or another frame encoder.
    // B4.1.2: render into a caller-owned image so sequential export can reuse
    // its pixel allocation when dimensions and format remain unchanged.
    static bool renderOverlayInto(QImage &image, const QSize &size,
                                  const QVector<float> &bands,
                                  const SpectrumRenderSettings &settings,
                                  const QVector<float> &peaks = {},
                                  QImage::Format outputFormat = QImage::Format_ARGB32_Premultiplied) {
        if (size.isEmpty() || bands.isEmpty()) return false;
        if (image.size() != size || image.format() != outputFormat)
            image = QImage(size, outputFormat);
        if (image.isNull()) return false;
        // B4.1.6: profile stages without changing pixels or paint geometry.
        QElapsedTimer stageTimer;
        stageTimer.start();
        // B4.1.7: transparent black is an all-zero pixel for these 32-bit
        // formats. Clear their contiguous storage directly; retain Qt's
        // format-aware fill for every other format.
        if (image.format() == QImage::Format_ARGB32_Premultiplied ||
            image.format() == QImage::Format_ARGB32) {
            const qsizetype bytes = image.sizeInBytes();
            if (bytes > 0)
                std::memset(image.bits(), 0, size_t(bytes));
        } else {
            image.fill(Qt::transparent);
        }
        const qint64 fillNs = stageTimer.nsecsElapsed();
        stageTimer.restart();
        QPainter painter(&image);
        const qint64 beginNs = stageTimer.nsecsElapsed();
        stageTimer.restart();
        paint(painter, QRectF(QPointF(0, 0), QSizeF(size)), bands, settings, peaks);
        const qint64 drawNs = stageTimer.nsecsElapsed();
        stageTimer.restart();
        painter.end();
        const qint64 endNs = stageTimer.nsecsElapsed();
        // Export calls are sequential; the counters are diagnostic only.
        static qint64 frames = 0;
        static qint64 fillTotal = 0, beginTotal = 0, drawTotal = 0, endTotal = 0;
        fillTotal += fillNs;
        beginTotal += beginNs;
        drawTotal += drawNs;
        endTotal += endNs;
        ++frames;
        if (frames == 1 || frames % 100 == 0) {
            qDebug().noquote() << "[B4.1.7] PAINT STAGES"
                << "frames=" << frames << "mode=" << settings.mode
                << "fill_total_ms=" << fillTotal / 1000000.0
                << "begin_total_ms=" << beginTotal / 1000000.0
                << "draw_total_ms=" << drawTotal / 1000000.0
                << "end_total_ms=" << endTotal / 1000000.0;
        }
        return true;
    }

    static QImage renderOverlay(const QSize &size, const QVector<float> &bands,
                                const SpectrumRenderSettings &settings,
                                const QVector<float> &peaks = {},
                                QImage::Format outputFormat = QImage::Format_ARGB32_Premultiplied) {
        QImage image;
        if (!renderOverlayInto(image, size, bands, settings, peaks, outputFormat))
            return {};
        return image;
    }
};

#endif // SPECTRUMRENDERER_H
