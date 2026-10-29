#ifndef SPECTRUMEXPORTFRAMES_H
#define SPECTRUMEXPORTFRAMES_H

#include "spectrumanalyzer.h"
#include "spectrumrenderer.h"

#include <QImage>
#include <QElapsedTimer>
#include <QDebug>
#include <QSize>
#include <QVector>
#include <QtGlobal>
#include <cmath>

// B3.2: sequential frame generator for the FFmpeg overlay input.
// No QWidget or video playback dependency. One call = one video frame.
// The encoder integration is a separate step: do not use this on the UI
// thread to pre-render an entire long video.
class SpectrumExportFrames {
public:
    SpectrumExportFrames(SpectrumAnalyzer *analyzer,
                         const SpectrumRenderSettings &settings,
                         const QSize &frameSize,
                         int fps = 30)
        : m_analyzer(analyzer), m_settings(settings), m_size(frameSize),
          m_fps(qMax(1, fps)) {}

    void reset() {
        m_nextFrame = 0;
        m_smoothed.clear();
        m_peaks.clear();
        m_peakTicks.clear();
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
                const float response = delta > 0.0f ? 0.52f : 0.32f;
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
        const qint64 stateNs = profileClock.nsecsElapsed();
        profileClock.restart();
        QImage overlay = SpectrumRenderer::renderOverlay(m_size, m_smoothed, timed,
                                               timed.peakHold ? m_peaks : QVector<float>(),
                                               m_directRgba ? QImage::Format_RGBA8888 : QImage::Format_ARGB32_Premultiplied);
        const qint64 paintNs = profileClock.nsecsElapsed();
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
        return overlay;
    }

    void setDirectRgba(bool enabled) { m_directRgba = enabled; }

    void setGradientSpeed(int speed) { m_gradientSpeed = qBound(0, speed, 100); }

private:
    SpectrumAnalyzer *m_analyzer = nullptr; // owned by MainWindow
    SpectrumRenderSettings m_settings;
    QSize m_size;
    int m_fps = 30;
    int m_gradientSpeed = 0;
    bool m_directRgba = false;
    qint64 m_nextFrame = 0;
    QVector<float> m_smoothed;
    QVector<float> m_peaks;
    QVector<int> m_peakTicks;
    qint64 m_profileFrames = 0;
    qint64 m_profileSpectrumNs = 0, m_profileStateNs = 0, m_profilePaintNs = 0;
    qint64 m_profileSpectrumMaxNs = 0, m_profileStateMaxNs = 0, m_profilePaintMaxNs = 0;
};

#endif // SPECTRUMEXPORTFRAMES_H
