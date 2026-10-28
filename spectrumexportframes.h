#ifndef SPECTRUMEXPORTFRAMES_H
#define SPECTRUMEXPORTFRAMES_H

#include "spectrumanalyzer.h"
#include "spectrumrenderer.h"

#include <QImage>
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
        const QVector<float> target = m_analyzer->spectrumAt(timeMs, 64);
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
        return SpectrumRenderer::renderOverlay(m_size, m_smoothed, timed,
                                               timed.peakHold ? m_peaks : QVector<float>());
    }

    void setGradientSpeed(int speed) { m_gradientSpeed = qBound(0, speed, 100); }

private:
    SpectrumAnalyzer *m_analyzer = nullptr; // owned by MainWindow
    SpectrumRenderSettings m_settings;
    QSize m_size;
    int m_fps = 30;
    int m_gradientSpeed = 0;
    qint64 m_nextFrame = 0;
    QVector<float> m_smoothed;
    QVector<float> m_peaks;
    QVector<int> m_peakTicks;
};

#endif // SPECTRUMEXPORTFRAMES_H
