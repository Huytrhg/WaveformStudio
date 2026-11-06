#ifndef SPECTRUMEXPORTFRAMES_H
#define SPECTRUMEXPORTFRAMES_H

#include "spectrumanalyzer.h"
#include "spectrumrenderer.h"
#include "effectoverlays.h"

#include <QImage>
#include <QElapsedTimer>
#include <QDebug>
#include <QSize>
#include <QVector>
#include <QtGlobal>
#include <QPolygonF>
#include <QRadialGradient>
#include <QtMath>
#include <cmath>

// Export-side snapshot of the Preview Bokeh controls. Animation is evaluated
// from output-frame time so the result is deterministic and independent of Preview.
struct BokehExportSettings {
    bool enabled = false;
    bool audioReactive = false;
    int count = 28, size = 35, speed = 35;
    int opacity = 55, sensitivity = 60;
    QColor color = QColor("#A9D9FF");
    QColor secondaryColor = QColor("#E8A5FF");
    bool gradientEnabled = false;
    int shape = 0, blur = 70, glow = 60;
    int depth = 45, direction = 0;
    int twinkle = 22, audioSmoothing = 65;
    int pulseSmoothing = 70;
};

// B3.2: sequential frame generator for the FFmpeg overlay input.
// No QWidget or video playback dependency. One call = one video frame.
// The encoder integration is a separate step: do not use this on the UI
// thread to pre-render an entire long video.
class SpectrumExportFrames {
public:
    SpectrumExportFrames(SpectrumAnalyzer *analyzer,
                         const SpectrumRenderSettings &settings,
                         const QSize &frameSize,
                         int fps = 30,
                         const BokehExportSettings &bokehSettings = {},
                         const EffectOverlaySettings &effectSettings = {})
        : m_analyzer(analyzer), m_settings(settings), m_size(frameSize),
          m_fps(qMax(1, fps)), m_bokehSettings(bokehSettings),
          m_effectSettings(effectSettings) {}

    void reset() {
        m_nextFrame = 0;
        m_smoothed.clear();
        m_peaks.clear();
        m_peakTicks.clear();
        m_overlayBuffer = QImage();
        m_spectrumBuffer = QImage();
        m_bokehSprite = QImage();
        m_effectOverlayRenderer.reset();
        m_framePulseEnergy = 0.0;
        m_bokehSmoothedEnergy = 0.0;
        m_profileFrames = 0;
        m_profileSpectrumNs = m_profileStateNs = m_profilePaintNs = 0;
        m_profileSpectrumMaxNs = m_profileStateMaxNs = m_profilePaintMaxNs = 0;
    }

    qint64 nextFrameIndex() const { return m_nextFrame; }
    int framesPerSecond() const { return m_fps; }

    QImage nextOverlay() {
        if (!m_analyzer || !m_analyzer->isReady() || m_size.isEmpty())
            return {};
        const qint64 index = m_nextFrame++;
        // B3.3.3: use the presentation timestamp of the output frame.
        // Round to nearest millisecond instead of consistently flooring.
        const qint64 presentationMs =
            (index * 1000 + m_fps / 2) / m_fps;
        const int timeMs = int(qMin<qint64>(
            presentationMs, m_analyzer->durationMs()));
        QElapsedTimer profileClock;
        profileClock.start();
        const QVector<float> target = m_analyzer->spectrumAt(timeMs, 64);
        const qint64 spectrumNs = profileClock.nsecsElapsed();
        profileClock.restart();
        if (target.isEmpty()) return {};
        if (m_smoothed.size() != target.size()) {
            m_smoothed = target;
            m_peaks = target;
            m_peakTicks.fill(0, target.size());
        } else {
            for (int i = 0; i < target.size(); ++i) {
                const float delta = target[i] - m_smoothed[i];
                const float response = delta > 0.0f ? 0.52f :
                    (m_settings.mode == 1
                        ? float(0.08 + 0.92 * m_settings.curveReleaseSpeed / 100.0)
                        : 0.32f);
                m_smoothed[i] = std::abs(delta) < 0.0005f
                    ? target[i] : m_smoothed[i] + delta * response;
                if (m_settings.peakHold) {
                    if (target[i] >= m_peaks[i]) {
                        m_peaks[i] = target[i];
                        m_peakTicks[i] = 8;
                    } else if (m_peakTicks[i] > 0) {
                        --m_peakTicks[i];
                    } else {
                        m_peaks[i] = qMax(m_smoothed[i], m_peaks[i] - 0.008f);
                    }
                }
            }
        }
        SpectrumRenderSettings timed = m_settings;
        // Phase is derived from output time, not the live preview timer.
        // The caller sets gradientPhase to its starting value.
        timed.gradientPhase = std::fmod(
            m_settings.gradientPhase +
                (qreal(index) / m_fps) * m_gradientSpeed / 100.0 * 0.45, 2.0);
        timed.curvePhase = std::fmod(
            m_settings.curvePhase + (qreal(index) / m_fps) *
                m_settings.curveTravelSpeed / 100.0 * 0.42, 2.0);
        const qint64 stateNs = profileClock.nsecsElapsed();
        profileClock.restart();
        const QImage::Format outputFormat = m_directRgba
            ? QImage::Format_RGBA8888 : QImage::Format_ARGB32_Premultiplied;
        bool rendered = false;
        const bool hasAmbientEffects = m_effectSettings.rainGlassEnabled ||
            m_effectSettings.particlesEnabled || m_effectSettings.glowEnabled;
        if ((m_bokehSettings.enabled && m_bokehSettings.opacity > 0) || hasAmbientEffects) {
            // Render the Spectrum independently, then composite Bokeh first so
            // the exported layer order matches Preview: video -> Bokeh -> Spectrum.
            rendered = SpectrumRenderer::renderOverlayInto(
                m_spectrumBuffer, m_size, m_smoothed, timed,
                timed.peakHold ? m_peaks : QVector<float>(), outputFormat);
            if (rendered) {
                if (m_overlayBuffer.size() != m_size ||
                    m_overlayBuffer.format() != outputFormat)
                    m_overlayBuffer = QImage(m_size, outputFormat);
                if (m_overlayBuffer.isNull()) return {};
                m_overlayBuffer.fill(Qt::transparent);
                QPainter composite(&m_overlayBuffer);
                composite.setCompositionMode(QPainter::CompositionMode_SourceOver);
                const QRectF effectArea(QPointF(0, 0), QSizeF(m_size));
                const qreal effectTime = qreal(index) / m_fps;
                if (m_bokehSettings.enabled && m_bokehSettings.opacity > 0)
                    paintBokeh(composite, effectArea, target, effectTime);
                const qreal energy = spectrumEnergy(target);
                m_effectOverlayRenderer.paintRainGlass(composite, effectArea,
                    m_effectSettings, effectTime, energy);
                m_effectOverlayRenderer.paintParticles(composite, effectArea,
                    m_effectSettings, effectTime, energy);
                EffectOverlayRenderer::paintGlow(composite, effectArea,
                    m_effectSettings, energy);
                composite.drawImage(QPoint(0, 0), m_spectrumBuffer);
                composite.end();
            }
        } else {
            rendered = SpectrumRenderer::renderOverlayInto(
                m_overlayBuffer, m_size, m_smoothed, timed,
                timed.peakHold ? m_peaks : QVector<float>(), outputFormat);
        }
        const qint64 paintNs = profileClock.nsecsElapsed();
        if (!rendered) return {};
        ++m_profileFrames;
        m_profileSpectrumNs += spectrumNs;
        m_profileStateNs += stateNs;
        m_profilePaintNs += paintNs;
        m_profileSpectrumMaxNs = qMax(m_profileSpectrumMaxNs, spectrumNs);
        m_profileStateMaxNs = qMax(m_profileStateMaxNs, stateNs);
        m_profilePaintMaxNs = qMax(m_profilePaintMaxNs, paintNs);
        // Emit occasional cumulative samples without logging every video frame.
        if (m_profileFrames == 1 || m_profileFrames % 100 == 0) {
            qDebug().noquote() << "[B4.1.1] RENDER PROFILE"
                << "frames=" << m_profileFrames
                << "spectrum_total_ms=" << (m_profileSpectrumNs / 1000000.0)
                << "state_total_ms=" << (m_profileStateNs / 1000000.0)
                << "paint_total_ms=" << (m_profilePaintNs / 1000000.0)
                << "spectrum_max_ms=" << (m_profileSpectrumMaxNs / 1000000.0)
                << "state_max_ms=" << (m_profileStateMaxNs / 1000000.0)
                << "paint_max_ms=" << (m_profilePaintMaxNs / 1000000.0);
        }
        return m_overlayBuffer;
    }

    void setDirectRgba(bool enabled) { m_directRgba = enabled; }

    void setGradientSpeed(int speed) { m_gradientSpeed = qBound(0, speed, 100); }

private:
    static qreal spectrumEnergy(const QVector<float> &spectrum)
    {
        if (spectrum.isEmpty()) return 0.0;
        qreal sum = 0.0;
        for (float sample : spectrum) {
            const qreal v = qBound(0.0, qreal(sample), 1.0);
            sum += v * v;
        }
        return qBound(0.0, qSqrt(sum / spectrum.size()) * 2.4, 1.0);
    }

    static qreal bokehHash(int n)
    {
        const qreal value = qSin(n * 127.1 + 78.233) * 43758.5453;
        return value - qFloor(value);
    }

    void rebuildBokehSprite()
    {
        constexpr int diameter = 128;
        m_bokehSprite = QImage(diameter, diameter, QImage::Format_ARGB32_Premultiplied);
        m_bokehSprite.fill(Qt::transparent);
        QPainter painter(&m_bokehSprite);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF center(diameter / 2.0, diameter / 2.0);
        const qreal radius = diameter / 2.0;
        QPainterPath shape;
        if (m_bokehSettings.shape == 1) {
            QPolygonF polygon;
            for (int i = 0; i < 6; ++i) {
                const qreal angle = qDegreesToRadians(i * 60.0 - 30.0);
                polygon << QPointF(center.x() + radius * 0.91 * qCos(angle),
                                   center.y() + radius * 0.91 * qSin(angle));
            }
            shape.addPolygon(polygon);
            shape.closeSubpath();
        } else if (m_bokehSettings.shape == 2) {
            shape.moveTo(center.x(), center.y() + radius * 0.72);
            shape.cubicTo(center.x() - radius * 1.12, center.y() + radius * 0.05,
                          center.x() - radius * 0.82, center.y() - radius * 0.88,
                          center.x(), center.y() - radius * 0.38);
            shape.cubicTo(center.x() + radius * 0.82, center.y() - radius * 0.88,
                          center.x() + radius * 1.12, center.y() + radius * 0.05,
                          center.x(), center.y() + radius * 0.72);
        } else {
            shape.addEllipse(QRectF(2, 2, diameter - 4, diameter - 4));
        }
        QRadialGradient glow(center, radius);
        const qreal blur = m_bokehSettings.blur / 100.0;
        const qreal glowAmount = m_bokehSettings.glow / 100.0;
        QColor inner = m_bokehSettings.color;
        inner.setAlpha(qBound(0, qRound(225 * (0.65 + glowAmount * 0.35)), 255));
        QColor outer = m_bokehSettings.gradientEnabled
            ? m_bokehSettings.secondaryColor : m_bokehSettings.color;
        outer.setAlpha(qBound(0, qRound(150 * (0.4 + glowAmount * 0.6)), 255));
        QColor edge = outer;
        edge.setAlpha(qRound((1.0 - blur) * 65));
        QColor clear = outer;
        clear.setAlpha(0);
        glow.setColorAt(0.0, inner);
        glow.setColorAt(0.18 + (1.0 - blur) * 0.30, outer);
        glow.setColorAt(0.78 + (1.0 - blur) * 0.15, edge);
        glow.setColorAt(1.0, clear);
        painter.setPen(Qt::NoPen);
        painter.fillPath(shape, glow);
    }

    void paintBokeh(QPainter &painter, const QRectF &area,
                    const QVector<float> &spectrum, qreal timeSeconds)
    {
        if (!m_bokehSettings.enabled || area.isEmpty() ||
            m_bokehSettings.opacity <= 0 || m_bokehSettings.count <= 0)
            return;

        qreal energy = 0.0;
        if (m_bokehSettings.audioReactive && !spectrum.isEmpty()) {
            qreal sum = 0.0;
            for (float sample : spectrum) {
                const qreal value = qBound(0.0, qreal(sample), 1.0);
                sum += value * value;
            }
            const qreal rawEnergy = qBound(0.0,
                qSqrt(sum / spectrum.size()) * 2.4, 1.0);
            const qreal pulseResponse =
                1.0 - (qBound(0, m_bokehSettings.pulseSmoothing, 100) / 100.0) * 0.90;
            m_framePulseEnergy += (rawEnergy - m_framePulseEnergy) * pulseResponse;
            const qreal gain = 0.25 +
                qBound(0, m_bokehSettings.sensitivity, 100) / 50.0;
            const qreal target = qBound(0.0, m_framePulseEnergy * gain, 1.0);
            const qreal smoothing = m_bokehSettings.audioSmoothing / 100.0;
            const qreal attack = 0.42 - smoothing * 0.32;
            const qreal release = 0.18 - smoothing * 0.14;
            const qreal response = target > m_bokehSmoothedEnergy ? attack : release;
            m_bokehSmoothedEnergy += (target - m_bokehSmoothedEnergy) * response;
            energy = m_bokehSmoothedEnergy;
        }

        if (m_bokehSprite.isNull()) rebuildBokehSprite();
        const qreal minSide = qMin(area.width(), area.height());
        const qreal speed = m_bokehSettings.speed / 50.0;
        painter.save();
        painter.setClipRect(area);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int i = 0; i < m_bokehSettings.count; ++i) {
            const qreal seedX = bokehHash(i * 7 + 1);
            const qreal seedY = bokehHash(i * 7 + 2);
            const qreal depth = bokehHash(i * 7 + 6);
            const qreal depthFactor =
                1.0 + (depth - 0.5) * (m_bokehSettings.depth / 100.0) * 1.7;
            const qreal sizeVariance =
                (0.45 + bokehHash(i * 7 + 3) * 1.1) * depthFactor;
            const qreal velocity =
                (0.012 + bokehHash(i * 7 + 4) * 0.025) * speed * depthFactor;
            const qreal cycle = std::fmod(seedY + timeSeconds * velocity, 1.35);
            const qreal sway = qSin(timeSeconds * (0.22 + depth * 0.12) + i * 2.3);
            qreal x = seedX, y = seedY;
            if (m_bokehSettings.direction == 0) y = 1.15 - cycle;
            else if (m_bokehSettings.direction == 1) y = cycle - 0.15;
            else if (m_bokehSettings.direction == 2) x = cycle - 0.15;
            else x = 1.15 - cycle;
            if (m_bokehSettings.direction < 2)
                x += sway * 0.022 * depthFactor;
            else
                y += sway * 0.022 * depthFactor;
            const qreal phase = timeSeconds * (1.3 + bokehHash(i * 7 + 8) * 2.8)
                + bokehHash(i * 7 + 9) * 6.28318530718;
            const qreal wave = 0.5 + 0.5 * qSin(phase);
            const qreal twinkle = 1.0 - (m_bokehSettings.twinkle / 100.0) * 0.58
                + (m_bokehSettings.twinkle / 100.0) * 0.58 * wave;
            const qreal radius = minSide *
                (0.008 + m_bokehSettings.size * 0.00035) *
                sizeVariance * (1.0 + energy * 0.28);
            const QPointF center(x * area.width(), y * area.height());
            const qreal layerFade = 0.62 + depth * 0.38;
            const qreal alpha = qBound(0.0,
                (m_bokehSettings.opacity / 100.0) * (0.55 + energy * 0.45) *
                layerFade * twinkle * (0.55 + bokehHash(i * 7 + 5) * 0.45),
                0.90);
            painter.setOpacity(alpha);
            painter.drawImage(QRectF(center.x() - radius + area.left(),
                                     center.y() - radius + area.top(),
                                     radius * 2.0, radius * 2.0), m_bokehSprite);
        }
        painter.restore();
    }

    SpectrumAnalyzer *m_analyzer = nullptr; // owned by MainWindow
    SpectrumRenderSettings m_settings;
    BokehExportSettings m_bokehSettings;
    EffectOverlaySettings m_effectSettings;
    EffectOverlayRenderer m_effectOverlayRenderer;
    QSize m_size;
    int m_fps = 30;
    int m_gradientSpeed = 0;
    bool m_directRgba = false;
    qint64 m_nextFrame = 0;
    QVector<float> m_smoothed;
    QVector<float> m_peaks;
    QVector<int> m_peakTicks;
    QImage m_overlayBuffer; // Reused across sequential export frames.
    QImage m_spectrumBuffer;
    QImage m_bokehSprite;
    qreal m_framePulseEnergy = 0.0;
    qreal m_bokehSmoothedEnergy = 0.0;
    qint64 m_profileFrames = 0;
    qint64 m_profileSpectrumNs = 0, m_profileStateNs = 0, m_profilePaintNs = 0;
    qint64 m_profileSpectrumMaxNs = 0, m_profileStateMaxNs = 0, m_profilePaintMaxNs = 0;
};

#endif // SPECTRUMEXPORTFRAMES_H
