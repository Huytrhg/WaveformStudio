#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "spectrumanalyzer.h"
#include "spectrumrenderer.h"
#include "spectrumexportframes.h" // B3.2: frame generator; encoder wiring pending.
#include "gpurenderer.h"
#include "nativegpupresenter.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QDebug>
#include <QLinearGradient>
#include <QAbstractItemView>
#include <algorithm>
#include <QTimer>
#include <QScrollArea>
#include <cmath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QElapsedTimer>

#include <QAudioOutput>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QListWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QSignalBlocker>
#include <QDir>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QStackedLayout>
#include <QPolygonF>
#include <QtMath>
#include <QMouseEvent>
#include <QSettings>
#include <QInputDialog>
#include <QProcess>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSpinBox>
#include <functional>
#include <memory>

// Interactive Photoshop-inspired gradient bar: click to add, drag stops,
// double-click a stop to choose its color. Keeps rendering independent of FFT.
class GradientEditor : public QWidget
{
public:
    using Stops = QVector<QPair<qreal, QColor>>;
    explicit GradientEditor(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(77);
        setMaximumHeight(77);
        setMouseTracking(true);
        setToolTip("Click below the gradient to add a stop; drag to move; double-click to edit color");
    }
    Stops stops;
    int selected = 0;
    std::function<void()> changed;
    std::function<void(int)> selectionChanged;
    std::function<void(int)> editColor;
    void refresh() { update(); }
    void setStops(const Stops &v) { stops = v; selected = 0; update(); if (changed) changed(); }

protected:
    QRectF bar() const { return QRectF(12, 10, qMax(1, width()-24), 28); }
    qreal positionAt(qreal x) const { const auto r = bar(); return qBound(0.0, (x-r.left())/r.width(), 1.0); }
    int hitStop(const QPointF &point) const
    {
        const auto r = bar();
        if (point.y() < 35 || point.y() > 73) return -1;
        for (int i = stops.size()-1; i >= 0; --i)
            if (qAbs(point.x() - (r.left() + stops[i].first*r.width())) < 10) return i;
        return -1;
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto r = bar();
        QLinearGradient grad(r.topLeft(), r.topRight());
        for (const auto &stop : stops) grad.setColorAt(qBound(0.0,stop.first,1.0),stop.second);
        painter.setPen(QPen(QColor("#70757A"), 1));
        painter.setBrush(grad);
        painter.drawRoundedRect(r, 3, 3);
        for (int i=0;i<stops.size();++i) {
            const qreal x = r.left()+stops[i].first*r.width();
            QPolygonF triangle;
            triangle << QPointF(x,40) << QPointF(x-7,49) << QPointF(x+7,49);
            painter.setPen(QPen(i==selected ? QColor("#FFFFFF") : QColor("#A0A5AA"),i==selected?2:1));
            painter.setBrush(stops[i].second);
            painter.drawPolygon(triangle);
            painter.drawRect(QRectF(x-7,49,14,12));
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button()!=Qt::LeftButton) return;
        int hit=hitStop(event->position());
        if (hit<0 && event->position().y() >= 38 && event->position().y() <= 73) {
            const qreal pos=positionAt(event->position().x());
            QColor c = stops.isEmpty()? QColor("#FFFFFF") : stops.first().second;
            for(int i=1;i<stops.size();++i) if(stops[i].first>=pos) {
                const auto &a=stops[i-1], &b=stops[i];
                const qreal t=(pos-a.first)/qMax(0.00001,b.first-a.first);
                c=QColor::fromRgbF(a.second.redF()*(1-t)+b.second.redF()*t,
                    a.second.greenF()*(1-t)+b.second.greenF()*t,
                    a.second.blueF()*(1-t)+b.second.blueF()*t); break;
            }
            stops.append({pos,c});
            std::sort(stops.begin(),stops.end(),[](const auto &a,const auto &b){return a.first<b.first;});
            for(int i=0;i<stops.size();++i) if(qAbs(stops[i].first-pos)<0.00001){hit=i;break;}
            if(changed) changed();
        }
        if(hit>=0) {selected=hit; dragging=true; if(selectionChanged) selectionChanged(selected); update();}
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if(!dragging || selected<0 || selected>=stops.size()) return;
        qreal pos=positionAt(event->position().x());
        const qreal lo=selected>0?stops[selected-1].first+0.001:0.0;
        const qreal hi=selected+1<stops.size()?stops[selected+1].first-0.001:1.0;
        stops[selected].first=qBound(lo,pos,hi);
        update(); if(changed) changed();
    }
    void mouseReleaseEvent(QMouseEvent *) override {dragging=false;}
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int i=hitStop(event->position());
        if(i>=0) { selected=i; update(); if(selectionChanged) selectionChanged(i); if(editColor) editColor(i); }
    }
private:
    bool dragging=false;
};

// Audio-reactive overlay supporting bars, waveform and circular styles.
// Position values are percentages of the video stage.
class SpectrumDisplay : public QWidget
{
public:
    explicit SpectrumDisplay(QWidget *parent = nullptr)
        : QWidget(parent), bands(64, 0.0f), visualMode(0),
          positionX(50), positionY(78), opacityPercent(92), scalePercent(72),
          solidColor(QColor("#35B8FF")), useGradient(true),
          columnCount(64), heightPercent(100), thicknessPercent(75), glowPercent(0),
          glowRadius(50), glowIntensity(100), gradientSpeed(0), gradientPhase(0.0), peakHoldEnabled(false)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
#ifdef Q_OS_WIN
        setAttribute(Qt::WA_NativeWindow, true);
#endif
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_AlwaysStackOnTop, false);
        setAutoFillBackground(false);
        setToolTip("Spectrum Overlay — Bars / Waveform / Circular");
        videoSink = new QVideoSink(this);
        connect(videoSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &frame) {
                    if (!frame.isValid()) return;
                    // Keep the preview video close to 30 FPS when frames are available.
                    // A 41 ms gate capped the video near 24 FPS and could look choppy
                    // even while Spectrum continued repainting smoothly.
                    if (frameClock.isValid() && frameClock.elapsed() < 33) return;
                    frameClock.restart();
                    QImage image = frame.toImage();
                    if (image.isNull()) return;
                    // Preview only: cap image size to reduce CPU painting cost.
                    const QSize limit(960, 540);
                    if (image.width() > limit.width() || image.height() > limit.height())
                        image = image.scaled(limit, Qt::KeepAspectRatio, Qt::FastTransformation);
                    videoImage = std::move(image);
                    gpuBackgroundDirty = true;
                    update();
                });
        // Phase 4D.1: animate between 20 Hz FFT samples at ~30 Hz.
        // Stop the animation when the analyzer clears its bands.
        motionTimer = new QTimer(this);
        motionTimer->setInterval(33);
        connect(motionTimer, &QTimer::timeout, this, [this]() {
            if (targetBands.isEmpty()) { motionTimer->stop(); return; }
            bool changed = false;
            for (int i = 0; i < bands.size(); ++i) {
                const float delta = targetBands[i] - bands[i];
                const float response = delta > 0.0f ? 0.52f : 0.32f;
                const float next = std::abs(delta) < 0.0005f
                    ? targetBands[i] : bands[i] + delta * response;
                if (next != bands[i]) { bands[i] = next; changed = true; }
            }
            if (peakHoldEnabled && !bands.isEmpty()) {
                if (peakLevels.size() != bands.size()) {
                    peakLevels = bands;
                    peakTicks.fill(0, bands.size());
                }
                for (int i = 0; i < bands.size(); ++i) {
                    if (targetBands[i] >= peakLevels[i]) {
                        peakLevels[i] = targetBands[i];
                        peakTicks[i] = 8; // About 264 ms at 33 ms/tick.
                    } else if (peakTicks[i] > 0) {
                        --peakTicks[i];
                    } else {
                        peakLevels[i] = qMax(bands[i], peakLevels[i] - 0.008f);
                    }
                }
                changed = true;
            }
            if (changed) update();
        });
        gradientStops = {
            {0.0, QColor("#00D5FF")},
            {0.5, QColor("#376BFF")},
            {1.0, QColor("#D64DFF")}
        };
    }

    QVideoSink *getVideoSink() const { return videoSink; }

    void setBands(const QVector<float> &values)
    {
        if (values.isEmpty()) {
            motionTimer->stop();
            targetBands.clear();
            bands.clear();
            peakLevels.clear();
            peakTicks.clear();
            update();
            return;
        }
        targetBands = values;
        if (targetBands.size() > 64) targetBands.resize(64);
        if (bands.size() != targetBands.size()) {
            bands = targetBands; // First sample: no ramp from stale values.
            peakLevels = targetBands;
            peakTicks.fill(0, bands.size());
            update();
        }
        if (!motionTimer->isActive()) motionTimer->start();
    }
    void setMessage(const QString &text) { message = text; update(); }
    void setVisualMode(int mode) { visualMode = qBound(0, mode, 3); update(); }
    void setPosition(int x, int y) { positionX = qBound(0, x, 100); positionY = qBound(0, y, 100); update(); }
    void setOpacityPercent(int value) { opacityPercent = qBound(0, value, 100); update(); }
    void setForceCpuFallback(bool enabled) { forceCpuFallback = enabled; update(); }
    QString diagnosticsText() const
    {
        return QStringLiteral("Preview mode: %1\nMeasured paint rate: %2 FPS\n\n%3")
            .arg(forceCpuFallback ? QStringLiteral("CPU fallback (QPainter)")
                                  : QStringLiteral("Hybrid GPU (D3D11 composite + readback)"))
            .arg(measuredFps, 0, 'f', 1)
            .arg(GpuRenderer::instance().diagnostics());
    }
    void setScalePercent(int value) { scalePercent = qBound(20, value, 100); update(); }
    void setColumnCount(int value) { columnCount = qBound(8, value, 128); update(); }
    void setHeightPercent(int value) { heightPercent = qBound(10, value, 200); update(); }
    void setThicknessPercent(int value) { thicknessPercent = qBound(10, value, 100); update(); }
    void setGlowPercent(int value) { glowPercent = qBound(0, value, 100); update(); }
    void setGlowRadius(int value) { glowRadius = qBound(0, value, 100); update(); }
    void setGlowIntensity(int value) { glowIntensity = qBound(0, value, 100); update(); }
    void setPeakHoldEnabled(bool enabled) { peakHoldEnabled = enabled; peakLevels = bands; peakTicks.fill(0, bands.size()); update(); }
    void setGradientSpeed(int value) { gradientSpeed = qBound(0, value, 100); update(); }
    void advanceGradient(qreal seconds)
    {
        if (!useGradient || gradientSpeed == 0) return;
        gradientPhase = std::fmod(gradientPhase + seconds * gradientSpeed / 100.0 * 0.45, 2.0);
        update();
    }
    void setSolidColor(const QColor &color) { if (color.isValid()) solidColor = color; update(); }
    void setUseGradient(bool enabled) { useGradient = enabled; update(); }
    bool isGradientEnabled() const { return useGradient; }
    QColor getSolidColor() const { return solidColor; }
    QVector<QPair<qreal, QColor>> getGradientStops() const { return gradientStops; }
    void setGradientStops(const QVector<QPair<qreal,QColor>> &v) {
        if(v.size()<2) return;
        gradientStops=v; selectedStopIndex=qBound(0,selectedStopIndex,v.size()-1); update();
    }
    void setInterpolation(int mode) { interpolation=qBound(0,mode,2); update(); }

    void addGradientStop()
    {
        qreal newPos = 0.5;
        if (gradientStops.size() >= 2) {
            int index = qBound(0, selectedStopIndex, gradientStops.size() - 2);
            newPos = (gradientStops[index].first + gradientStops[index + 1].first) / 2.0;
        }
        QColor c = colorAt(newPos);
        gradientStops.push_back({newPos, c});
        std::sort(gradientStops.begin(), gradientStops.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        selectedStopIndex = 0;
        for (int i = 0; i < gradientStops.size(); ++i)
            if (qAbs(gradientStops[i].first - newPos) < 0.0001) selectedStopIndex = i;
        update();
    }
    bool removeGradientStop(int index)
    {
        if (gradientStops.size() <= 2 || index < 0 || index >= gradientStops.size()) return false;
        gradientStops.removeAt(index);
        selectedStopIndex = qBound(0, index, gradientStops.size() - 1);
        update();
        return true;
    }
    void setGradientStopColor(int index, const QColor &color)
    {
        if (color.isValid() && index >= 0 && index < gradientStops.size()) {
            gradientStops[index].second = color;
            update();
        }
    }
    int selectedStopIndex = 0;
    SpectrumRenderSettings exportSettings() const { return currentRenderSettings(); }
    int exportGradientSpeed() const { return gradientSpeed; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        ++diagnosticFrameCount;
        if (!diagnosticsClock.isValid()) diagnosticsClock.start();
        else if (diagnosticsClock.elapsed() >= 1000) {
            measuredFps = diagnosticFrameCount * 1000.0 / diagnosticsClock.elapsed();
            diagnosticFrameCount = 0;
            diagnosticsClock.restart();
        }
        // Stability fix: do not present a D3D11 swap chain directly into this
        // Qt-owned widget HWND. On some Windows/Qt Multimedia configurations,
        // Qt repaints and the native swap chain compete for the same surface,
        // causing the preview to flash while audio continues normally.
        // Keep the native presenter implementation available for a future
        // integration using a dedicated native child window, but use the
        // stable Qt paint path for all preview modes for now.

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.fillRect(rect(), QColor("#101114"));
        // Paint the video frame and the audio visualization in the SAME widget.
        // This avoids QVideoWidget's native surface covering the overlay.
        if (!videoImage.isNull()) {
            const QSize fitted = videoImage.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect target((width() - fitted.width()) / 2,
                               (height() - fitted.height()) / 2,
                               fitted.width(), fitted.height());
            p.drawImage(target, videoImage);
        }
        if (!message.isEmpty()) {
            p.setPen(QColor(235, 238, 242, 210));
            p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignCenter, message);
        }
        if (bands.isEmpty() || width() < 2 || height() < 2) return;

        p.save();
        p.setOpacity(opacityPercent / 100.0);
        const qreal cx = width() * positionX / 100.0;
        const qreal cy = height() * positionY / 100.0;
        const qreal maxW = width() * (scalePercent / 100.0);
        const qreal maxH = height() * 0.42 * (scalePercent / 72.0) * (heightPercent / 100.0);

        if (visualMode == 0) { // A: vertical bars; attempt D3D11 shader rendering first.
            const int count = columnCount;
            const qreal cellW = maxW / count;
            const qreal barW = qMax(0.6, cellW * thicknessPercent / 100.0);
            const qreal startX = cx - maxW / 2.0;
            const qreal baseline = cy;
            QVector<QRectF> gpuRects;
            QVector<QColor> gpuColors;
            QVector<QColor> gpuTopColors;
            gpuRects.reserve(count * ((glowPercent > 0 ? 5 : 1) + (peakHoldEnabled ? 1 : 0)));
            gpuColors.reserve(gpuRects.capacity());
            gpuTopColors.reserve(gpuRects.capacity());
            for (int i = 0; i < count; ++i) {
                const qreal sourceIndex = qreal(i) * (bands.size() - 1) / qMax(1, count - 1);
                const int lo = qBound(0, int(sourceIndex), bands.size() - 1);
                const int hi = qMin(lo + 1, bands.size() - 1);
                const qreal v = qBound(0.0, qreal(bands[lo]) * (1.0 - (sourceIndex - lo))
                                              + qreal(bands[hi]) * (sourceIndex - lo), 1.0);
                const qreal h = qMax(1.0, v * maxH);
                const QRectF bar(startX + i * cellW + (cellW - barW) / 2.0,
                                 baseline - h, barW, h);
                const QColor color = colorForBand(i, count);
                // Phase 4C.2: top and bottom colors are interpolated by the
                // existing D3D11 vertex-color shader across each bar.
                const qreal bandT = count <= 1 ? 0.0 : qreal(i) / (count - 1);
                const QColor topColor = useGradient
                    ? colorAt(gradientCoordinate(bandT + gradientPhase + 0.18)) : color;
                if (glowPercent > 0) {
                    // Phase 4C.4: radius and intensity are independent controls.
                    // Four lightweight layers approximate falloff; this is not a Gaussian blur.
                    const qreal strength = (glowPercent / 100.0) * (glowIntensity / 100.0);
                    const qreal spread = 0.5 + 8.0 * (glowRadius / 100.0);
                    const qreal layerScale[] = {1.0, 0.74, 0.48, 0.24};
                    const qreal alphaScale[] = {0.07, 0.12, 0.19, 0.28};
                    for (int layer = 0; layer < 4; ++layer) {
                        QColor halo = color;
                        const int alpha = qBound(0, qRound(255.0 * strength * alphaScale[layer]), 36);
                        halo.setAlpha(alpha);
                        const qreal radius = spread * layerScale[layer];
                        gpuRects.push_back(bar.adjusted(-radius, -radius, radius, radius));
                        gpuColors.push_back(halo);
                        QColor topHalo = topColor;
                        topHalo.setAlpha(alpha);
                        gpuTopColors.push_back(topHalo);
                    }
                }
                gpuRects.push_back(bar);
                gpuColors.push_back(color);
                gpuTopColors.push_back(topColor);
                if (peakHoldEnabled && !peakLevels.isEmpty()) {
                    const qreal idx = qreal(i) * (peakLevels.size() - 1) / qMax(1, count - 1);
                    const int loPeak = qBound(0, int(idx), peakLevels.size() - 1);
                    const int hiPeak = qMin(loPeak + 1, peakLevels.size() - 1);
                    const qreal peak = qBound(0.0, qreal(peakLevels[loPeak]) * (1.0 - (idx - loPeak))
                        + qreal(peakLevels[hiPeak]) * (idx - loPeak), 1.0);
                    // A slim peak cap, colored like the Spectrum instead of a white block.
                    const qreal markerH = 1.6;
                    const qreal markerGap = 1.5;
                    gpuRects.push_back(QRectF(bar.left(), baseline - qMax(1.0, peak * maxH) - markerGap - markerH, barW, markerH));
                    QColor marker = useGradient ? topColor : color;
                    marker.setAlpha(qMin(marker.alpha(), 220));
                    gpuColors.push_back(marker);
                    gpuTopColors.push_back(marker);
                }
            }
            // Phase 4A hybrid compositing: prepare a correctly letterboxed,
            // opaque background, upload it to D3D11 and draw bars on the same
            // render target. The final readback remains until native presentation.
            // Phase 4B.3: cache the CPU-prepared background. Spectrum/audio
            // updates repaint frequently, but the video image and stage size often
            // stay unchanged between video frames. Avoid allocating and repainting
            // this full-size image on every Spectrum tick.
            if (gpuBackgroundDirty || cachedGpuBackground.size() != size()
                || cachedGpuBackground.format() != QImage::Format_ARGB32) {
                cachedGpuBackground = QImage(size(), QImage::Format_ARGB32);
                cachedGpuBackground.fill(QColor("#101114"));
                if (!videoImage.isNull()) {
                    QPainter bgPainter(&cachedGpuBackground);
                    const QSize fitted = videoImage.size().scaled(size(), Qt::KeepAspectRatio);
                    const QRect target((width() - fitted.width()) / 2,
                                       (height() - fitted.height()) / 2,
                                       fitted.width(), fitted.height());
                    bgPainter.drawImage(target, videoImage);
                }
                gpuBackgroundDirty = false;
                ++gpuBackgroundBuilds;
            }
            // The GPU composite contains an opaque video background, so
            // QPainter::setOpacity() cannot fade only the Spectrum after the
            // composite has been rendered. Apply the opacity to each bar/glow
            // color before GPU rendering; leave the video background untouched.
            const qreal spectrumOpacity = opacityPercent / 100.0;
            for (QColor &gpuColor : gpuColors) {
                gpuColor.setAlphaF(qBound(0.0, gpuColor.alphaF() * spectrumOpacity, 1.0));
            }
            for (QColor &gpuColor : gpuTopColors) {
                gpuColor.setAlphaF(qBound(0.0, gpuColor.alphaF() * spectrumOpacity, 1.0));
            }

            QImage gpuComposite;
            const bool gpuRendered = !forceCpuFallback && GpuRenderer::instance().renderColoredRects(
                width(), height(), gpuRects, gpuColors, &gpuComposite, &cachedGpuBackground,
                &gpuTopColors);
            if (gpuRendered) {
                p.setOpacity(1.0); // background must remain opaque
                p.drawImage(QPoint(0, 0), gpuComposite);
            } else {
                // B2: CPU Bars fallback shares the same drawing engine as export.
                // Reset opacity: SpectrumRenderer applies its own opacity once.
                p.setOpacity(1.0);
                SpectrumRenderer::paint(p, QRectF(rect()), bands,
                                        currentRenderSettings(), peakLevels);
            }
        } else {
            // B2: Lines, Circular and Filled share one offscreen-capable renderer.
            p.setOpacity(1.0);
            SpectrumRenderer::paint(p, QRectF(rect()), bands,
                                    currentRenderSettings(), peakLevels);
        }
        if (visualMode == 0) {
            const QString gpuFrameLine = GpuRenderer::instance().runtimeStats().section('\n', 0, 0);
            const QString fpsLine = QStringLiteral("Preview FPS: %1")
                .arg(measuredFps, 0, 'f', 1);
            const QString hudText = gpuFrameLine + QStringLiteral("  |  ") + fpsLine;
            p.save();
            p.setOpacity(0.92);
            p.setFont(QFont(QStringLiteral("Consolas"), 9));
            p.setPen(QColor(230, 240, 245));
            p.setBrush(QColor(10, 14, 20, 205));
            const QRect hudRect(8, 8, qMin(width() - 16, 640), 28);
            p.drawRoundedRect(hudRect, 5, 5);
            p.drawText(hudRect.adjusted(8, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, hudText);
            p.restore();
        }
        p.restore();
    }

private:
    SpectrumRenderSettings currentRenderSettings() const
    {
        SpectrumRenderSettings s;
        s.mode = visualMode;
        s.positionX = positionX;
        s.positionY = positionY;
        s.opacity = opacityPercent;
        s.scale = scalePercent;
        s.columns = columnCount;
        s.height = heightPercent;
        s.thickness = thicknessPercent;
        s.glow = glowPercent;
        s.glowRadius = glowRadius;
        s.glowIntensity = glowIntensity;
        s.peakHold = peakHoldEnabled;
        s.gradient = useGradient;
        s.interpolation = interpolation;
        s.gradientPhase = gradientPhase;
        s.solidColor = solidColor;
        s.stops = gradientStops;
        return s;
    }

    bool tryNativeGpuPaint()
    {
#ifdef Q_OS_WIN
        if (!nativeGpuAttempted) {
            nativeGpuAttempted = true;
            nativeGpuReady = nativeGpuPresenter.initialize(
                reinterpret_cast<void *>(winId()), qMax(1, width()), qMax(1, height()));
            if (!nativeGpuReady)
                qWarning() << "Native GPU preview unavailable; keeping Phase 4A fallback:"
                           << nativeGpuPresenter.lastError();
        }
        if (!nativeGpuReady || width() < 2 || height() < 2)
            return false;
        if (!nativeGpuPresenter.resize(width(), height())) {
            qWarning() << "Native GPU resize failed; switching to Phase 4A:"
                       << nativeGpuPresenter.lastError();
            nativeGpuReady = false;
            nativeGpuPresenter.shutdown();
            return false;
        }

        const int count = columnCount;
        const qreal maxW = width() * (scalePercent / 100.0);
        const qreal maxH = height() * 0.42 * (scalePercent / 72.0) * (heightPercent / 100.0);
        const qreal cx = width() * positionX / 100.0;
        const qreal cy = height() * positionY / 100.0;
        const qreal cellW = maxW / count;
        const qreal barW = qMax(0.6, cellW * thicknessPercent / 100.0);
        const qreal startX = cx - maxW / 2.0;
        QVector<QRectF> rects;
        QVector<QColor> colors;
        rects.reserve(count * (glowPercent > 0 ? 2 : 1));
        colors.reserve(rects.capacity());
        for (int i = 0; i < count; ++i) {
            const qreal sourceIndex = qreal(i) * (bands.size() - 1) / qMax(1, count - 1);
            const int lo = qBound(0, int(sourceIndex), bands.size() - 1);
            const int hi = qMin(lo + 1, bands.size() - 1);
            const qreal frac = sourceIndex - lo;
            const qreal value = qBound(0.0, qreal(bands[lo]) * (1.0 - frac)
                                              + qreal(bands[hi]) * frac, 1.0);
            const qreal h = qMax(1.0, value * maxH);
            const QRectF bar(startX + i * cellW + (cellW - barW) / 2.0,
                             cy - h, barW, h);
            const QColor color = colorForBand(i, count);
            if (glowPercent > 0) {
                QColor halo = color;
                halo.setAlpha(qBound(0, qRound(55.0 * glowPercent / 100.0 * glowIntensity / 100.0), 55));
                const qreal radius = 0.5 + 4.0 * (glowRadius / 100.0);
                rects.push_back(bar.adjusted(-radius, -radius, radius, radius));
                colors.push_back(halo);
            }
            rects.push_back(bar);
            colors.push_back(color);
        }
        QImage background = videoImage;
        if (background.isNull()) {
            background = QImage(size(), QImage::Format_ARGB32);
            background.fill(QColor("#101114"));
        }
        if (!nativeGpuPresenter.presentFrame(background, rects, colors, opacityPercent / 100.0)) {
            qWarning() << "Native GPU present failed; switching to Phase 4A:"
                       << nativeGpuPresenter.lastError();
            nativeGpuReady = false;
            nativeGpuPresenter.shutdown();
            return false;
        }
        return true;
#else
        return false;
#endif
    }

    QVideoSink *videoSink = nullptr;
#ifdef Q_OS_WIN
    NativeGpuPresenter nativeGpuPresenter;
    bool nativeGpuAttempted = false;
    bool nativeGpuReady = false;
#endif
    QImage videoImage;
    QImage cachedGpuBackground;
    bool gpuBackgroundDirty = true;
    quint64 gpuBackgroundBuilds = 0;
    QElapsedTimer frameClock;
    QElapsedTimer diagnosticsClock;
    int diagnosticFrameCount = 0;
    double measuredFps = 0.0;
    bool forceCpuFallback = false;
    QVector<float> bands;
    QVector<float> targetBands;
    QVector<float> peakLevels;
    QVector<int> peakTicks;
    QTimer *motionTimer = nullptr;
    bool peakHoldEnabled;
    QString message;
    int visualMode;
    int positionX, positionY, opacityPercent, scalePercent;
    QColor solidColor;
    bool useGradient;
    int columnCount, heightPercent, thicknessPercent, glowPercent, glowRadius, glowIntensity, gradientSpeed;
    int interpolation = 1;
    qreal gradientPhase;
    QVector<QPair<qreal, QColor>> gradientStops;

    QColor colorAt(qreal t) const
    {
        if (!useGradient || gradientStops.isEmpty()) return solidColor;
        if (t <= gradientStops.first().first) return gradientStops.first().second;
        if (t >= gradientStops.last().first) return gradientStops.last().second;
        for (int i = 0; i < gradientStops.size() - 1; ++i) {
            const auto a = gradientStops[i]; const auto b = gradientStops[i + 1];
            if (t >= a.first && t <= b.first) {
                const qreal span = qMax<qreal>(0.0001, b.first - a.first);
                qreal u=(t-a.first)/span;
                if(interpolation==1) u=u*u*(3.0-2.0*u);
                if(interpolation==2) {
                    const QColor ah=a.second.toHsv(), bh=b.second.toHsv();
                    qreal h1=ah.hueF(),h2=bh.hueF();
                    if(h1<0) h1=0; if(h2<0) h2=0;
                    qreal dh=h2-h1;
                    if(dh>0.5) dh-=1.0; if(dh< -0.5) dh+=1.0;
                    qreal h=std::fmod(h1+dh*u+1.0,1.0);
                    return QColor::fromHsvF(h, ah.saturationF()+(bh.saturationF()-ah.saturationF())*u,
                       ah.valueF()+(bh.valueF()-ah.valueF())*u,
                       a.second.alphaF()+(b.second.alphaF()-a.second.alphaF())*u);
                }
                return QColor::fromRgbF(
                    a.second.redF()+(b.second.redF()-a.second.redF())*u,
                    a.second.greenF()+(b.second.greenF()-a.second.greenF())*u,
                    a.second.blueF()+(b.second.blueF()-a.second.blueF())*u,
                    a.second.alphaF()+(b.second.alphaF()-a.second.alphaF())*u);
            }
        }
        return gradientStops.last().second;
    }
    // Reflect the gradient at its endpoints instead of jumping from last color
    // to first color. This keeps neighboring bars continuous while animating.
    static qreal gradientCoordinate(qreal phase)
    {
        qreal t = std::fmod(phase, 2.0);
        if (t < 0.0) t += 2.0;
        return t <= 1.0 ? t : 2.0 - t;
    }

    QColor colorForBand(int index, int count) const
    {
        const qreal t = count <= 1 ? 0.0 : qreal(index) / (count - 1);
        return useGradient ? colorAt(gradientCoordinate(t + gradientPhase)) : solidColor;
    }
};
// Keep the actual preview canvas at 16:9 within the available center panel.
// Empty space outside the canvas belongs to the workspace, not the video.
class PreviewAspectContainer : public QWidget
{
public:
    explicit PreviewAspectContainer(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setStyleSheet("background:transparent;");
    }

    void setCanvas(QWidget *widget)
    {
        canvas = widget;
        if (canvas) {
            canvas->setParent(this);
            canvas->show();
        }
        layoutCanvas();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        layoutCanvas();
    }

private:
    QWidget *canvas = nullptr;

    void layoutCanvas()
    {
        if (!canvas || width() <= 0 || height() <= 0) return;
        int w = qMin(width(), int(qint64(height()) * 16 / 9));
        int h = qMin(height(), int(qint64(w) * 9 / 16));
        w = int(qint64(h) * 16 / 9);
        canvas->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
    }
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , player(new QMediaPlayer(this))
    , audioOutput(new QAudioOutput(this))
    , videoWidget(new QVideoWidget(this))
    , playButton(nullptr)
    , positionSlider(nullptr)
    , timeLabel(nullptr)
    , videoInfoLabel(nullptr)
    , spectrumAnalyzer(new SpectrumAnalyzer(this))
    , spectrumDisplay(nullptr)
    , spectrumOverlay(nullptr)
    , spectrumTimer(new QTimer(this))
    , analyzerStatusLabel(nullptr)
    , isSeeking(false)
{
    ui->setupUi(this);

    setWindowTitle("WaveformStudio");
    resize(1440, 900);
    setMinimumSize(1100, 700);

    player->setAudioOutput(audioOutput);
    // Video output is connected to SpectrumDisplay's QVideoSink below, after
    // the preview stage has been created. Do not use QVideoWidget here.

    audioOutput->setVolume(0.8);

    setStyleSheet(R"(
        QMainWindow, QWidget {
            background-color: #202124;
            color: #E8EAED;
            font-family: "Segoe UI";
            font-size: 10pt;
        }

        QMenuBar {
            background-color: #292A2D;
            border-bottom: 1px solid #3C4043;
        }

        QMenuBar::item {
            padding: 6px 12px;
        }

        QMenuBar::item:selected {
            background-color: #3C4043;
        }

        QMenu {
            background-color: #292A2D;
            border: 1px solid #45474C;
        }

        QMenu::item {
            padding: 7px 24px;
        }

        QMenu::item:selected {
            background-color: #234A83;
        }

        QPushButton {
            background-color: #35363A;
            color: #F1F3F4;
            border: 1px solid #4A4C50;
            border-radius: 5px;
            padding: 7px 12px;
        }

        QPushButton:hover {
            background-color: #45474C;
        }

        QPushButton:pressed {
            background-color: #2D6CDF;
        }

        QLabel#sectionTitle {
            color: #9AA0A6;
            font-weight: bold;
            font-size: 9pt;
        }

        QFrame#panel {
            background-color: #292A2D;
            border: 1px solid #3C4043;
            border-radius: 6px;
        }

        QListWidget {
            background-color: #202124;
            border: 1px solid #3C4043;
            border-radius: 4px;
            padding: 4px;
            outline: none;
        }

        QListWidget::item {
            padding: 9px 6px;
            border-radius: 4px;
        }

        QListWidget::item:selected {
            background-color: #234A83;
        }

        QSlider::groove:horizontal {
            height: 5px;
            background: #4A4C50;
            border-radius: 2px;
        }

        QSlider::handle:horizontal {
            background: #8AB4F8;
            width: 13px;
            margin: -4px 0;
            border-radius: 6px;
        }

        QStatusBar {
            background-color: #292A2D;
            border-top: 1px solid #3C4043;
        }
    )");

    // =========================================================
    // MENU BAR
    // =========================================================

    QMenu *fileMenu = menuBar()->addMenu("File");

    QAction *newAction =
        fileMenu->addAction("New Project");

    QAction *openAction =
        fileMenu->addAction("Import Video...");

    QAction *importAudioAction =
        fileMenu->addAction("Import Audio...");

    fileMenu->addSeparator();

    QAction *exportAction =
        fileMenu->addAction("Export Video...");

    fileMenu->addSeparator();

    QAction *exitAction =
        fileMenu->addAction("Exit");

    QMenu *editMenu = menuBar()->addMenu("Edit");
    editMenu->addAction("Undo");
    editMenu->addAction("Redo");

    QMenu *viewMenu = menuBar()->addMenu("View");
    viewMenu->addAction("Reset Workspace");

    QMenu *helpMenu = menuBar()->addMenu("Help");
    helpMenu->addAction("About WaveformStudio");

    // =========================================================
    // CENTRAL WIDGET
    // =========================================================

    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    // =========================================================
    // TOP TOOLBAR
    // =========================================================

    QFrame *toolbarPanel = new QFrame;
    toolbarPanel->setObjectName("panel");

    QHBoxLayout *toolbarLayout =
        new QHBoxLayout(toolbarPanel);

    toolbarLayout->setContentsMargins(10, 7, 10, 7);
    toolbarLayout->setSpacing(8);

    QLabel *appLogo = new QLabel("W");
    appLogo->setAlignment(Qt::AlignCenter);
    appLogo->setFixedSize(34, 34);

    appLogo->setStyleSheet(
        "background-color:#2D6CDF;"
        "border-radius:7px;"
        "font-size:18pt;"
        "font-weight:bold;"
        );

    QLabel *appName =
        new QLabel("WaveformStudio");

    appName->setStyleSheet(
        "font-size:14pt;font-weight:bold;"
        );

    toolbarLayout->addWidget(appLogo);
    toolbarLayout->addWidget(appName);
    toolbarLayout->addSpacing(16);

    QPushButton *importVideoButton =
        new QPushButton("Import Video");

    QPushButton *importAudioButton =
        new QPushButton("Import Audio");

    QPushButton *saveProjectButton =
        new QPushButton("Save Project");

    toolbarLayout->addWidget(importVideoButton);
    toolbarLayout->addWidget(importAudioButton);
    toolbarLayout->addWidget(saveProjectButton);

    toolbarLayout->addStretch();

    QLabel *resolutionLabel =
        new QLabel("1920 × 1080  |  60 FPS");

    resolutionLabel->setStyleSheet(
        "color:#8AB4F8;font-weight:bold;"
        );

    toolbarLayout->addWidget(resolutionLabel);

    QPushButton *exportButton =
        new QPushButton("Export Video");

    exportButton->setStyleSheet(
        "QPushButton {"
        "background-color:#2D6CDF;"
        "border:1px solid #4A83E8;"
        "font-weight:bold;"
        "padding:8px 16px;"
        "}"
        "QPushButton:hover {"
        "background-color:#4285F4;"
        "}"
        );

    toolbarLayout->addWidget(exportButton);

    rootLayout->addWidget(toolbarPanel);

    // =========================================================
    // MAIN WORKSPACE
    // =========================================================

    QSplitter *workspaceSplitter =
        new QSplitter(Qt::Horizontal);

    workspaceSplitter->setChildrenCollapsible(false);

    // ---------------------------------------------------------
    // LEFT: EFFECT LIBRARY
    // ---------------------------------------------------------

    QFrame *effectsPanel = new QFrame;
    effectsPanel->setObjectName("panel");
    effectsPanel->setMinimumWidth(175);
    effectsPanel->setMaximumWidth(260);

    QVBoxLayout *effectsLayout =
        new QVBoxLayout(effectsPanel);

    effectsLayout->setContentsMargins(10, 12, 10, 12);
    effectsLayout->setSpacing(8);

    QLabel *effectsTitle =
        new QLabel("EFFECT LIBRARY");

    effectsTitle->setObjectName("sectionTitle");

    effectsLayout->addWidget(effectsTitle);

    QListWidget *effectsList =
        new QListWidget;

    effectsList->addItem("Audio Waveform");
    effectsList->addItem("Spectrum Analyzer");
    effectsList->addItem("Circular Waveform");
    effectsList->addItem("Audio Bars");
    effectsList->addItem("Bokeh Overlay");
    effectsList->addItem("Rain Glass");
    effectsList->addItem("Glow");
    effectsList->addItem("Particles");

    effectsList->setCurrentRow(0);

    effectsLayout->addWidget(effectsList, 1);

    QPushButton *addEffectButton =
        new QPushButton("+ Add Effect Layer");

    effectsLayout->addWidget(addEffectButton);

    workspaceSplitter->addWidget(effectsPanel);

    // ---------------------------------------------------------
    // CENTER: VIDEO PREVIEW
    // ---------------------------------------------------------

    QFrame *previewPanel = new QFrame;
    previewPanel->setObjectName("panel");

    QVBoxLayout *previewLayout =
        new QVBoxLayout(previewPanel);

    previewLayout->setContentsMargins(12, 12, 12, 12);
    previewLayout->setSpacing(10);

    QHBoxLayout *previewHeader =
        new QHBoxLayout;

    QLabel *previewTitle =
        new QLabel("PREVIEW");

    previewTitle->setObjectName("sectionTitle");

    videoInfoLabel =
        new QLabel("No video loaded");

    videoInfoLabel->setStyleSheet(
        "color:#9AA0A6;"
        );

    previewHeader->addWidget(previewTitle);
    previewHeader->addSpacing(12);
    QLabel *rendererLabel = new QLabel("Renderer");
    rendererLabel->setStyleSheet("color:#9AA0A6;");
    QComboBox *rendererModeCombo = new QComboBox;
    rendererModeCombo->addItem("Hybrid GPU");
    rendererModeCombo->addItem("CPU fallback");
    rendererModeCombo->setToolTip("Hybrid GPU composites Spectrum with D3D11 then reads back to Qt. CPU fallback uses QPainter.");
    rendererModeCombo->setMinimumWidth(125);
    QPushButton *diagnosticsButton = new QPushButton("Diagnostics");
    diagnosticsButton->setToolTip("Show renderer performance and Direct3D 11 diagnostics");
    previewHeader->addWidget(rendererLabel);
    previewHeader->addWidget(rendererModeCombo);
    previewHeader->addWidget(diagnosticsButton);
    previewHeader->addStretch();
    previewHeader->addWidget(videoInfoLabel);

    previewLayout->addLayout(previewHeader);

    // Composite video and Spectrum Overlay in one QWidget. QVideoWidget can
    // use a native surface that sits above ordinary child widgets on Windows.
    // QVideoSink gives us frames so SpectrumDisplay paints video + effect itself.
    PreviewAspectContainer *stageContainer = new PreviewAspectContainer(previewPanel);
    QFrame *videoStage = new QFrame(stageContainer);
    videoStage->setStyleSheet("background:#101114;border:1px solid #45474C;border-radius:4px;");
    QVBoxLayout *stageLayout = new QVBoxLayout(videoStage);
    stageLayout->setContentsMargins(0, 0, 0, 0);
    stageLayout->setSpacing(0);

    spectrumOverlay = new SpectrumDisplay(videoStage);
    stageLayout->addWidget(spectrumOverlay);
    stageContainer->setCanvas(videoStage);
    player->setVideoOutput(spectrumOverlay->getVideoSink());
    previewLayout->addWidget(stageContainer, 1);

    // The separate 64-band monitor is unnecessary while the effect is already
    // rendered on the video preview. Keep its data object for compatibility,
    // but hide the monitor to reclaim preview space.
    QHBoxLayout *spectrumHeader = new QHBoxLayout;
    QLabel *spectrumTitle = new QLabel("SPECTRUM ANALYZER · 64 BANDS");
    spectrumTitle->setObjectName("sectionTitle");
    analyzerStatusLabel = new QLabel("Waiting for media");
    analyzerStatusLabel->setStyleSheet("color:#9AA0A6;font-size:8pt;");
    spectrumHeader->addWidget(spectrumTitle);
    spectrumHeader->addStretch();
    spectrumHeader->addWidget(analyzerStatusLabel);
    // Do not add spectrumHeader to the visible preview layout.
    // Analyzer state continues to be reported in the status bar.
    spectrumTitle->setParent(previewPanel);
    analyzerStatusLabel->setParent(previewPanel);
    spectrumTitle->hide();
    analyzerStatusLabel->hide();
    delete spectrumHeader;

    spectrumDisplay = new SpectrumDisplay(previewPanel);
    spectrumDisplay->hide();

    // ---------------------------------------------------------
    // PLAYBACK CONTROLS
    // ---------------------------------------------------------

    QHBoxLayout *playbackLayout =
        new QHBoxLayout;

    QPushButton *openPreviewButton =
        new QPushButton("Open Video");

    QPushButton *startButton =
        new QPushButton("|◀");

    QPushButton *previousButton =
        new QPushButton("◀");

    playButton =
        new QPushButton("▶ Play");

    QPushButton *nextButton =
        new QPushButton("▶|");

    timeLabel =
        new QLabel("00:00:00 / 00:00:00");

    timeLabel->setStyleSheet(
        "font-family:Consolas;"
        "font-size:9pt;"
        "color:#8AB4F8;"
        );

    playbackLayout->addWidget(openPreviewButton);
    playbackLayout->addWidget(startButton);
    playbackLayout->addWidget(previousButton);
    playbackLayout->addWidget(playButton);
    playbackLayout->addWidget(nextButton);
    playbackLayout->addSpacing(8);
    playbackLayout->addWidget(timeLabel);
    playbackLayout->addStretch();

    previewLayout->addLayout(playbackLayout);

    // ---------------------------------------------------------
    // VIDEO POSITION SLIDER
    // ---------------------------------------------------------

    positionSlider = new QSlider(Qt::Horizontal);

    positionSlider->setRange(0, 0);
    positionSlider->setSingleStep(1000);
    positionSlider->setPageStep(5000);
    positionSlider->setEnabled(false);

    previewLayout->addWidget(positionSlider);

    workspaceSplitter->addWidget(previewPanel);

    // ---------------------------------------------------------
    // RIGHT: PROPERTIES
    // ---------------------------------------------------------

    QFrame *propertiesPanel = new QFrame;
    propertiesPanel->setObjectName("panel");
    propertiesPanel->setMinimumWidth(230);
    propertiesPanel->setMaximumWidth(340);

    QVBoxLayout *propertiesLayout =
        new QVBoxLayout(propertiesPanel);

    propertiesLayout->setContentsMargins(12, 12, 12, 12);
    propertiesLayout->setSpacing(12);

    QLabel *propertiesTitle =
        new QLabel("PROPERTIES");

    propertiesTitle->setObjectName("sectionTitle");

    propertiesLayout->addWidget(propertiesTitle);

    QLabel *selectedEffectLabel = new QLabel("Spectrum Overlay");
    selectedEffectLabel->setStyleSheet("font-size:12pt;font-weight:bold;");
    propertiesLayout->addWidget(selectedEffectLabel);

    QLabel *styleLabel = new QLabel("Visualization Style");
    propertiesLayout->addWidget(styleLabel);
    QComboBox *visualModeCombo = new QComboBox;
    visualModeCombo->addItems({"A · Vertical Bars", "B · Waveform Lines", "C · Circular", "D · Filled Waveform"});
    visualModeCombo->setCurrentIndex(0);
    propertiesLayout->addWidget(visualModeCombo);

    QLabel *colorModeLabel = new QLabel("Color Mode");
    propertiesLayout->addWidget(colorModeLabel);
    QComboBox *colorModeCombo = new QComboBox;
    colorModeCombo->addItems({"Gradient", "Single Color"});
    propertiesLayout->addWidget(colorModeCombo);

    QPushButton *colorButton = new QPushButton("Choose Single Color…");
    propertiesLayout->addWidget(colorButton);

    QLabel *gradientTitle = new QLabel("Gradient Color Stops");
    gradientTitle->setStyleSheet("font-weight:bold;");
    propertiesLayout->addWidget(gradientTitle);
    GradientEditor *gradientEditor = new GradientEditor;
    gradientEditor->stops = spectrumOverlay->getGradientStops();
    propertiesLayout->addWidget(gradientEditor);
    QHBoxLayout *stopPositionRow = new QHBoxLayout;
    stopPositionRow->addWidget(new QLabel("Stop position"));
    QSpinBox *stopPosition = new QSpinBox;
    stopPosition->setRange(0,100); stopPosition->setSuffix("%");
    stopPositionRow->addWidget(stopPosition);
    propertiesLayout->addLayout(stopPositionRow);
    propertiesLayout->addWidget(new QLabel("Interpolation"));
    QComboBox *interpolationCombo = new QComboBox;
    interpolationCombo->addItems({"Linear", "Smooth", "HSV (shortest hue)"});
    interpolationCombo->setCurrentIndex(1);
    propertiesLayout->addWidget(interpolationCombo);
    propertiesLayout->addWidget(new QLabel("Gradient Presets"));
    QComboBox *presetCombo = new QComboBox;
    presetCombo->addItems({"Ocean Blue", "Sunset", "Neon", "Rainbow"});
    propertiesLayout->addWidget(presetCombo);
    QPushButton *savePresetButton = new QPushButton("Save Current Preset…");
    propertiesLayout->addWidget(savePresetButton);
    QListWidget *gradientStopsList = new QListWidget;
    gradientStopsList->setMaximumHeight(112);
    gradientStopsList->setSelectionMode(QAbstractItemView::SingleSelection);
    gradientStopsList->hide(); // Internal selection mirror; editor is the visible UI.

    QHBoxLayout *gradientButtons = new QHBoxLayout;
    QPushButton *addStopButton = new QPushButton("+ Add");
    QPushButton *removeStopButton = new QPushButton("− Remove");
    QPushButton *changeStopColorButton = new QPushButton("Change Color…");
    gradientButtons->addWidget(addStopButton);
    gradientButtons->addWidget(removeStopButton);
    propertiesLayout->addLayout(gradientButtons);
    propertiesLayout->addWidget(changeStopColorButton);

    auto addPercentSlider = [propertiesLayout](const QString &title, int value,
                                                QLabel **valueLabelOut = nullptr) {
        QHBoxLayout *row = new QHBoxLayout;
        QLabel *label = new QLabel(title);
        QLabel *valueLabel = new QLabel(QString::number(value) + "%");
        valueLabel->setMinimumWidth(38);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(label);
        row->addStretch();
        row->addWidget(valueLabel);
        propertiesLayout->addLayout(row);
        QSlider *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(value);
        propertiesLayout->addWidget(slider);
        if (valueLabelOut) *valueLabelOut = valueLabel;
        return slider;
    };

    QLabel *xValueLabel = nullptr;
    QLabel *yValueLabel = nullptr;
    QLabel *scaleValueLabel = nullptr;
    QLabel *opacityValueLabel = nullptr;
    QLabel *positionTitle = new QLabel("Position (video frame)");
    positionTitle->setStyleSheet("font-weight:bold;");
    propertiesLayout->addWidget(positionTitle);
    QSlider *xPositionSlider = addPercentSlider("Horizontal · X", 50, &xValueLabel);
    QSlider *yPositionSlider = addPercentSlider("Vertical · Y", 78, &yValueLabel);
    QSlider *scaleSlider = addPercentSlider("Size", 72, &scaleValueLabel);
    scaleSlider->setMinimum(20);
    QSlider *opacitySlider = addPercentSlider("Opacity", 92, &opacityValueLabel);

    // Waveform geometry and animated gradient controls.
    QLabel *waveTitle = new QLabel("WAVEFORM SETTINGS");
    waveTitle->setObjectName("sectionTitle");
    propertiesLayout->addWidget(waveTitle);

    auto addWaveSlider = [propertiesLayout](const QString &name, int min, int max,
                                            int initial, const QString &suffix,
                                            auto callback) {
        QHBoxLayout *row = new QHBoxLayout;
        QLabel *label = new QLabel(name);
        QLabel *value = new QLabel(QString::number(initial) + suffix);
        value->setMinimumWidth(50);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(label);
        row->addStretch();
        row->addWidget(value);
        propertiesLayout->addLayout(row);
        QSlider *slider = new QSlider(Qt::Horizontal);
        slider->setRange(min, max);
        slider->setValue(initial);
        propertiesLayout->addWidget(slider);
        QObject::connect(slider, &QSlider::valueChanged, slider,
                         [value, suffix, callback](int v) {
                             value->setText(QString::number(v) + suffix);
                             callback(v);
                         });
        return slider;
    };

    addWaveSlider("Number of Bars", 8, 128, 64, "", [this](int v) {
        spectrumOverlay->setColumnCount(v);
        spectrumDisplay->setColumnCount(v);
    });
    addWaveSlider("Bar Height", 10, 200, 100, "%", [this](int v) {
        spectrumOverlay->setHeightPercent(v);
        spectrumDisplay->setHeightPercent(v);
    });
    addWaveSlider("Bar Thickness", 10, 100, 75, "%", [this](int v) {
        spectrumOverlay->setThicknessPercent(v);
        spectrumDisplay->setThicknessPercent(v);
    });
    addWaveSlider("Glow", 0, 100, 0, "%", [this](int v) {
        spectrumOverlay->setGlowPercent(v);
        spectrumDisplay->setGlowPercent(v);
    });
    addWaveSlider("Glow Radius", 0, 100, 50, "%", [this](int v) {
        spectrumOverlay->setGlowRadius(v);
        spectrumDisplay->setGlowRadius(v);
    });
    addWaveSlider("Glow Intensity", 0, 100, 100, "%", [this](int v) {
        spectrumOverlay->setGlowIntensity(v);
        spectrumDisplay->setGlowIntensity(v);
    });
    QCheckBox *peakHoldToggle = new QCheckBox("Peak Hold (GPU Bars)");
    peakHoldToggle->setChecked(false);
    propertiesLayout->addWidget(peakHoldToggle);
    connect(peakHoldToggle, &QCheckBox::toggled, this, [this](bool enabled) {
        spectrumDisplay->setPeakHoldEnabled(enabled);
        spectrumOverlay->setPeakHoldEnabled(enabled);
    });
    addWaveSlider("Gradient Speed", 0, 100, 0, "%", [this](int v) {
        spectrumOverlay->setGradientSpeed(v);
        spectrumDisplay->setGradientSpeed(v);
    });

    // Local alias: class members cannot be captured directly by lambdas.
    SpectrumDisplay *overlay = spectrumOverlay;
    connect(rendererModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this, overlay](int index) {
        const bool cpuFallback = (index == 1);
        overlay->setForceCpuFallback(cpuFallback);
        spectrumDisplay->setForceCpuFallback(cpuFallback);
        statusBar()->showMessage(cpuFallback
            ? "Preview renderer: CPU fallback (QPainter)"
            : "Preview renderer: Hybrid GPU (D3D11 composite + readback)", 4000);
    });
    connect(diagnosticsButton, &QPushButton::clicked, this, [overlay]() {
        QMessageBox::information(nullptr, "WaveformStudio — Renderer Diagnostics",
                                 overlay->diagnosticsText());
    });

    // Populate the Photoshop-like editable gradient-stop list.
    auto refreshGradientList = [overlay, gradientStopsList, gradientEditor, stopPosition]() {
        gradientStopsList->clear();
        const auto stops = overlay->getGradientStops();
        for (int i = 0; i < stops.size(); ++i) {
            const QColor c = stops[i].second;
            auto *item = new QListWidgetItem(
                QString("%1%   %2").arg(qRound(stops[i].first * 100)).arg(c.name().toUpper()),
                gradientStopsList);
            item->setBackground(c);
            item->setForeground((c.lightness() > 140) ? QColor("#202124") : QColor("#FFFFFF"));
            item->setData(Qt::UserRole, i);
        }
        const int selected = qBound(0, overlay->selectedStopIndex, gradientStopsList->count() - 1);
        if (gradientStopsList->count() > 0) gradientStopsList->setCurrentRow(selected);
        gradientEditor->stops=stops;
        gradientEditor->selected=selected;
        gradientEditor->refresh();
        QSignalBlocker block(stopPosition);
        if(!stops.isEmpty()) stopPosition->setValue(qRound(stops[selected].first*100));
    };
    refreshGradientList();

    auto updateColorControls = [colorModeCombo, colorButton, gradientTitle,
                                 gradientStopsList, gradientEditor, stopPosition, interpolationCombo, presetCombo, savePresetButton, addStopButton, removeStopButton,
                                 changeStopColorButton]() {
        const bool gradient = (colorModeCombo->currentIndex() == 0);
        colorButton->setVisible(!gradient);
        gradientTitle->setVisible(gradient);
        gradientStopsList->setVisible(false);
        gradientEditor->setVisible(gradient);
        stopPosition->setVisible(gradient);
        interpolationCombo->setVisible(gradient);
        presetCombo->setVisible(gradient);
        savePresetButton->setVisible(gradient);
        addStopButton->setVisible(gradient);
        removeStopButton->setVisible(gradient);
        changeStopColorButton->setVisible(gradient);
    };
    updateColorControls();

    // Live gradient editor: synchronize drag, selection and color edits.
    gradientEditor->changed = [overlay, gradientEditor, gradientStopsList, stopPosition]() {
        overlay->setGradientStops(gradientEditor->stops);
        overlay->selectedStopIndex=gradientEditor->selected;
        QSignalBlocker block1(gradientStopsList);
        QSignalBlocker block2(stopPosition);
        gradientStopsList->setCurrentRow(gradientEditor->selected);
        if(gradientEditor->selected>=0 && gradientEditor->selected<gradientEditor->stops.size())
            stopPosition->setValue(qRound(gradientEditor->stops[gradientEditor->selected].first*100));
    };
    gradientEditor->selectionChanged = [overlay, gradientStopsList, stopPosition, gradientEditor](int index) {
        overlay->selectedStopIndex=index;
        QSignalBlocker block(gradientStopsList);
        gradientStopsList->setCurrentRow(index);
        if(index>=0 && index<gradientEditor->stops.size()) stopPosition->setValue(qRound(gradientEditor->stops[index].first*100));
    };
    gradientEditor->editColor = [this, gradientEditor](int) {
        const int i=gradientEditor->selected;
        if(i<0 || i>=gradientEditor->stops.size()) return;
        const QColor c=QColorDialog::getColor(gradientEditor->stops[i].second,this,"Gradient Stop Color");
        if(c.isValid()) {gradientEditor->stops[i].second=c; gradientEditor->refresh(); if(gradientEditor->changed) gradientEditor->changed();}
    };
    connect(stopPosition,qOverload<int>(&QSpinBox::valueChanged),this,[gradientEditor](int v){
        const int i=gradientEditor->selected;
        if(i<0 || i>=gradientEditor->stops.size()) return;
        const qreal lo=i>0?gradientEditor->stops[i-1].first+0.001:0;
        const qreal hi=i+1<gradientEditor->stops.size()?gradientEditor->stops[i+1].first-0.001:1;
        gradientEditor->stops[i].first=qBound(lo,v/100.0,hi);
        gradientEditor->refresh(); if(gradientEditor->changed) gradientEditor->changed();
    });
    connect(interpolationCombo,qOverload<int>(&QComboBox::currentIndexChanged),overlay,&SpectrumDisplay::setInterpolation);
    auto applyPreset = [gradientEditor](const QString &name) {
        GradientEditor::Stops stops;
        if(name=="Ocean Blue") stops={{0,QColor("#00D5FF")},{0.5,QColor("#376BFF")},{1,QColor("#D64DFF")}};
        else if(name=="Sunset") stops={{0,QColor("#FFCB52")},{0.5,QColor("#FF5368")},{1,QColor("#8A2BE2")}};
        else if(name=="Neon") stops={{0,QColor("#39FF14")},{0.5,QColor("#00D9FF")},{1,QColor("#FF20C5")}};
        else if(name=="Rainbow") stops={{0,QColor("#FF3030")},{0.25,QColor("#FFD500")},{0.5,QColor("#00DD77")},{0.75,QColor("#307BFF")},{1,QColor("#C03CFF")}};
        else {
            QSettings settings("WaveformStudio","WaveformStudio");
            const QVariantList stored=settings.value("gradients/"+name).toList();
            for(const QVariant &v:stored) {
                const QVariantMap m=v.toMap();
                const QColor c(m.value("color").toString());
                if(c.isValid()) stops.append({m.value("position").toDouble(),c});
            }
        }
        if(stops.size()>=2) gradientEditor->setStops(stops);
    };
    {
        QSettings settings("WaveformStudio","WaveformStudio");
        settings.beginGroup("gradients");
        for(const QString &name:settings.childKeys()) if(presetCombo->findText(name)<0) presetCombo->addItem(name);
        settings.endGroup();
    }
    connect(presetCombo,&QComboBox::currentTextChanged,this,applyPreset);
    connect(savePresetButton,&QPushButton::clicked,this,[this,gradientEditor,presetCombo](){
        bool ok=false;
        const QString name=QInputDialog::getText(this,"Save Gradient Preset","Preset name:",QLineEdit::Normal,QString(),&ok).trimmed();
        if(!ok || name.isEmpty() || name.contains('/')) return;
        QVariantList saved;
        for(const auto &stop:gradientEditor->stops) {
            QVariantMap m; m.insert("position",stop.first); m.insert("color",stop.second.name(QColor::HexArgb)); saved.append(m);
        }
        QSettings settings("WaveformStudio","WaveformStudio");
        settings.setValue("gradients/"+name,saved);
        if(presetCombo->findText(name)<0) presetCombo->addItem(name);
        presetCombo->setCurrentText(name);
    });
    connect(visualModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            overlay, &SpectrumDisplay::setVisualMode);
    connect(colorModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [overlay, updateColorControls](int index) {
        overlay->setUseGradient(index == 0);
        updateColorControls();
    });
    connect(colorButton, &QPushButton::clicked, this, [this, overlay]() {
        const QColor color = QColorDialog::getColor(overlay->getSolidColor(), this,
                                                      "Choose Spectrum Color");
        if (color.isValid()) overlay->setSolidColor(color);
    });
    connect(addStopButton, &QPushButton::clicked, this,
            [overlay, refreshGradientList]() {
        overlay->addGradientStop(); refreshGradientList();
    });
    connect(removeStopButton, &QPushButton::clicked, this,
            [overlay, gradientStopsList, refreshGradientList]() {
        const int row = gradientStopsList->currentRow();
        if (!overlay->removeGradientStop(row)) return;
        refreshGradientList();
    });
    connect(changeStopColorButton, &QPushButton::clicked, this,
            [this, overlay, gradientStopsList, refreshGradientList]() {
        const int row = gradientStopsList->currentRow();
        const auto stops = overlay->getGradientStops();
        if (row < 0 || row >= stops.size()) return;
        const QColor color = QColorDialog::getColor(stops[row].second, this,
                                                      "Change Gradient Stop Color");
        if (color.isValid()) { overlay->setGradientStopColor(row, color); refreshGradientList(); }
    });
    connect(gradientStopsList, &QListWidget::currentRowChanged,
            overlay, [overlay](int row) {
        if (row >= 0) overlay->selectedStopIndex = row;
    });
    connect(xPositionSlider, &QSlider::valueChanged, this,
            [overlay, xPositionSlider, yPositionSlider, xValueLabel](int value) {
        xValueLabel->setText(QString::number(value) + "%");
        overlay->setPosition(value, yPositionSlider->value());
    });
    connect(yPositionSlider, &QSlider::valueChanged, this,
            [overlay, xPositionSlider, yValueLabel](int value) {
        yValueLabel->setText(QString::number(value) + "%");
        overlay->setPosition(xPositionSlider->value(), value);
    });
    connect(scaleSlider, &QSlider::valueChanged, this, [overlay, scaleValueLabel](int value) {
        scaleValueLabel->setText(QString::number(value) + "%"); overlay->setScalePercent(value);
    });
    connect(opacitySlider, &QSlider::valueChanged, this, [overlay, opacityValueLabel](int value) {
        opacityValueLabel->setText(QString::number(value) + "%"); overlay->setOpacityPercent(value);
    });

    propertiesLayout->addStretch();

    QScrollArea *propertiesScroll = new QScrollArea;
    propertiesScroll->setWidgetResizable(true);
    propertiesScroll->setFrameShape(QFrame::NoFrame);
    propertiesScroll->setMinimumWidth(230);
    propertiesScroll->setMaximumWidth(340);
    propertiesScroll->setWidget(propertiesPanel);
    workspaceSplitter->addWidget(propertiesScroll);

    workspaceSplitter->setStretchFactor(0, 0);
    workspaceSplitter->setStretchFactor(1, 1);
    workspaceSplitter->setStretchFactor(2, 0);

    workspaceSplitter->setSizes(
        {210, 800, 240}
        );

    rootLayout->addWidget(workspaceSplitter, 1);

    // =========================================================
    // TIMELINE
    // =========================================================

    QFrame *timelinePanel = new QFrame;
    timelinePanel->setObjectName("panel");
    timelinePanel->setMinimumHeight(190);
    timelinePanel->setMaximumHeight(260);

    QVBoxLayout *timelineLayout =
        new QVBoxLayout(timelinePanel);

    timelineLayout->setContentsMargins(10, 8, 10, 10);
    timelineLayout->setSpacing(8);

    QHBoxLayout *timelineHeader =
        new QHBoxLayout;

    QLabel *timelineTitle =
        new QLabel("TIMELINE");

    timelineTitle->setObjectName("sectionTitle");

    QPushButton *addTrackButton =
        new QPushButton("+ Add Track");

    QPushButton *zoomOutButton =
        new QPushButton("−");

    QPushButton *zoomInButton =
        new QPushButton("+");

    timelineHeader->addWidget(timelineTitle);
    timelineHeader->addStretch();
    timelineHeader->addWidget(addTrackButton);
    timelineHeader->addWidget(zoomOutButton);
    timelineHeader->addWidget(zoomInButton);

    timelineLayout->addLayout(timelineHeader);

    // Timeline ruler
    QHBoxLayout *rulerLayout =
        new QHBoxLayout;

    rulerLayout->setSpacing(0);

    QLabel *rulerLabel =
        new QLabel("TRACK");

    rulerLabel->setFixedWidth(110);

    rulerLabel->setStyleSheet(
        "color:#9AA0A6;font-size:8pt;"
        );

    rulerLayout->addWidget(rulerLabel);

    QWidget *ruler = new QWidget;

    QHBoxLayout *rulerTicks =
        new QHBoxLayout(ruler);

    rulerTicks->setContentsMargins(0, 0, 0, 0);
    rulerTicks->setSpacing(0);

    for (int i = 0; i <= 10; ++i) {
        QLabel *tick =
            new QLabel(QString("%1 s").arg(i * 5));

        tick->setAlignment(
            Qt::AlignLeft | Qt::AlignVCenter
            );

        tick->setStyleSheet(
            "color:#9AA0A6;"
            "font-size:8pt;"
            "border-left:1px solid #45474C;"
            "padding-left:3px;"
            );

        rulerTicks->addWidget(tick, 1);
    }

    rulerLayout->addWidget(ruler, 1);

    timelineLayout->addLayout(rulerLayout);

    auto createTrack =
        [](const QString &name, const QString &color) {
            QWidget *track = new QWidget;

            QHBoxLayout *layout =
                new QHBoxLayout(track);

            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(5);

            QLabel *trackName =
                new QLabel(name);

            trackName->setFixedWidth(105);

            trackName->setStyleSheet(
                "color:#E8EAED;font-size:9pt;"
                );

            layout->addWidget(trackName);

            QFrame *clip = new QFrame;
            clip->setMinimumHeight(27);

            clip->setStyleSheet(
                QString(
                    "background-color:%1;"
                    "border:1px solid #55585E;"
                    "border-radius:3px;"
                    ).arg(color)
                );

            QLabel *clipLabel =
                new QLabel(name);

            clipLabel->setAlignment(Qt::AlignCenter);

            clipLabel->setStyleSheet(
                "color:white;"
                "background:transparent;"
                "border:none;"
                "font-size:8pt;"
                );

            QHBoxLayout *clipLayout =
                new QHBoxLayout(clip);

            clipLayout->setContentsMargins(4, 0, 4, 0);
            clipLayout->addWidget(clipLabel);

            layout->addWidget(clip, 1);

            return track;
        };

    timelineLayout->addWidget(
        createTrack("Video 1", "#345B78")
        );

    timelineLayout->addWidget(
        createTrack("Audio 1", "#386B55")
        );

    timelineLayout->addWidget(
        createTrack("Waveform 1", "#59478A")
        );

    timelineLayout->addWidget(
        createTrack("Overlay 1", "#805A35")
        );

    rootLayout->addWidget(timelinePanel);

    statusBar()->showMessage(
        "Ready | WaveformStudio | Import a video to begin"
        );

    // =========================================================
    // CONNECT VIDEO CONTROLS
    // =========================================================

    connect(
        importVideoButton,
        &QPushButton::clicked,
        this,
        &MainWindow::openVideo
        );

    connect(
        openPreviewButton,
        &QPushButton::clicked,
        this,
        &MainWindow::openVideo
        );

    connect(
        openAction,
        &QAction::triggered,
        this,
        &MainWindow::openVideo
        );

    connect(
        playButton,
        &QPushButton::clicked,
        this,
        &MainWindow::togglePlayback
        );

    connect(
        startButton,
        &QPushButton::clicked,
        this,
        [this]() {
            player->setPosition(0);
        }
        );

    connect(
        previousButton,
        &QPushButton::clicked,
        this,
        [this]() {
            player->setPosition(
                qMax(qint64(0), player->position() - 5000)
                );
        }
        );

    connect(
        nextButton,
        &QPushButton::clicked,
        this,
        [this]() {
            const qint64 duration = player->duration();

            if (duration > 0) {
                player->setPosition(
                    qMin(duration, player->position() + 5000)
                    );
            }
        }
        );

    connect(
        positionSlider,
        &QSlider::sliderPressed,
        this,
        [this]() {
            isSeeking = true;
        }
        );

    connect(
        positionSlider,
        &QSlider::sliderReleased,
        this,
        [this]() {
            player->setPosition(positionSlider->value());
            isSeeking = false;
        }
        );

    connect(
        player,
        &QMediaPlayer::positionChanged,
        this,
        &MainWindow::updatePosition
        );

    connect(
        player,
        &QMediaPlayer::durationChanged,
        this,
        &MainWindow::updateDuration
        );

    connect(
        player,
        &QMediaPlayer::playbackStateChanged,
        this,
        &MainWindow::updatePlaybackState
        );

    connect(
        player,
        &QMediaPlayer::errorOccurred,
        this,
        [this](QMediaPlayer::Error error,
               const QString &errorString) {
            Q_UNUSED(error);

            if (!errorString.isEmpty()) {
                statusBar()->showMessage(
                    "Playback error: " + errorString,
                    10000
                    );
            }
        }
        );

    // Spectrum analysis runs asynchronously through FFmpeg.
    spectrumTimer->setInterval(50); // 20 spectrum updates per second
    connect(spectrumTimer, &QTimer::timeout, this, [this]() {
        if (spectrumAnalyzer->isReady() &&
            player->playbackState() == QMediaPlayer::PlayingState) {
            const QVector<float> values = spectrumAnalyzer->spectrumAt(
                static_cast<int>(player->position()), 64);
            spectrumOverlay->setBands(values);
            spectrumOverlay->advanceGradient(0.05);
        }
    });

    connect(spectrumAnalyzer, &SpectrumAnalyzer::analysisStarted,
            this, [this]() {
        analyzerStatusLabel->setText("Analyzing audio…");
        spectrumDisplay->setBands(QVector<float>());
        spectrumOverlay->setBands(QVector<float>());
        spectrumDisplay->setMessage("Extracting audio for spectrum analysis…");
        spectrumOverlay->setMessage("Analyzing audio spectrum…");
        statusBar()->showMessage("Spectrum Analyzer: extracting audio with FFmpeg…");
    });

    connect(spectrumAnalyzer, &SpectrumAnalyzer::analysisProgress,
            this, [this](int percentage) {
        analyzerStatusLabel->setText(QString("Analyzing %1%").arg(percentage));
        statusBar()->showMessage(QString("Spectrum Analyzer: %1%").arg(percentage));
    });

    connect(spectrumAnalyzer, &SpectrumAnalyzer::analysisFinished,
            this, [this]() {
        analyzerStatusLabel->setText("Ready · 64 bands");
        spectrumDisplay->setMessage(QString());
        spectrumOverlay->setMessage(QString());
        const QVector<float> values = spectrumAnalyzer->spectrumAt(
            static_cast<int>(player->position()), 64);
        spectrumDisplay->setBands(values);
        spectrumOverlay->setBands(values);
        spectrumTimer->start();
        statusBar()->showMessage("Spectrum Analyzer ready — 64 frequency bands", 5000);
    });

    connect(spectrumAnalyzer, &SpectrumAnalyzer::analysisError,
            this, [this](const QString &message) {
        analyzerStatusLabel->setText("Analysis unavailable");
        spectrumDisplay->setBands(QVector<float>());
        spectrumOverlay->setBands(QVector<float>());
        spectrumDisplay->setMessage("Audio analysis failed — see status bar");
        spectrumOverlay->setMessage("Audio analysis failed");
        statusBar()->showMessage("Spectrum Analyzer: " + message, 10000);
    });

    connect(
        exitAction,
        &QAction::triggered,
        this,
        &QWidget::close
        );

    // Reserved controls for future development.
    connect(
        importAudioButton,
        &QPushButton::clicked,
        this,
        [this]() {
            QMessageBox::information(
                this,
                "Import Audio",
                "Audio import and waveform analysis "
                "will be implemented in a later step."
                );
        }
        );

    connect(
        importAudioAction,
        &QAction::triggered,
        importAudioButton,
        &QPushButton::click
        );

    connect(
        exportButton,
        &QPushButton::clicked,
        this,
        [this]() {
            const QString inputPath = player->source().toLocalFile();
            if (inputPath.isEmpty() || !QFileInfo::exists(inputPath)) {
                QMessageBox::warning(
                    this,
                    "Export Video",
                    "Please import a local video before exporting."
                    );
                return;
            }

            const QString outputPath = QFileDialog::getSaveFileName(
                this,
                "Export MP4 Video",
                QDir::homePath() + "/" +
                    QFileInfo(inputPath).completeBaseName() + "_export.mp4",
                "MP4 Video (*.mp4)"
                );
            if (outputPath.isEmpty())
                return;

            QString finalOutputPath = outputPath;
            if (QFileInfo(finalOutputPath).suffix().isEmpty())
                finalOutputPath += ".mp4";

            bool qualityAccepted = false;
            const QStringList qualityOptions = {
                "High quality (CRF 18)",
                "Balanced (CRF 23)",
                "Smaller file (CRF 28)"
            };
            const QString quality = QInputDialog::getItem(
                this,
                "Export Quality",
                "Choose video quality:",
                qualityOptions,
                1,
                false,
                &qualityAccepted
                );
            if (!qualityAccepted)
                return;

            int crf = 23;
            if (quality.startsWith("High"))
                crf = 18;
            else if (quality.startsWith("Smaller"))
                crf = 28;

            // B3.4.1: selectable hardware encoder; CPU remains available.
            bool encoderAccepted = false;
            const QStringList encoderOptions = {
                "Intel Quick Sync H.264 (GPU)",
                "CPU H.264 (libx264)"
            };
            const QString encoderChoice = QInputDialog::getItem(
                this, "Video Encoder", "Choose video encoder:",
                encoderOptions, 0, false, &encoderAccepted);
            if (!encoderAccepted) return;
            const bool useQsv = (encoderChoice == encoderOptions.first());
            bool rgbaAccepted = false;
            const QStringList rgbaOptions = {
                "Stable: premultiplied render + RGBA conversion",
                "Experimental: direct RGBA render (no conversion)"
            };
            const QString rgbaChoice = QInputDialog::getItem(
                this, "Spectrum Pixel Pipeline", "Choose pixel pipeline:",
                rgbaOptions, 0, false, &rgbaAccepted);
            if (!rgbaAccepted) return;
            const bool directRgba = (rgbaChoice == rgbaOptions.last());
            qDebug().noquote() << "[B3.4.2.2] Pixel pipeline:"
                               << (directRgba ? "direct_rgba" : "stable_conversion");

            const QString ffmpegPath = "C:/ffmpeg/bin/ffmpeg.exe";
            if (!QFileInfo::exists(ffmpegPath)) {
                QMessageBox::critical(
                    this,
                    "FFmpeg Not Found",
                    "FFmpeg was not found at:\n" + ffmpegPath +
                        "\n\nPlease install FFmpeg or update its path in the source."
                    );
                return;
            }

            // Pause live preview during encoding to reduce GPU/CPU load.
            const bool resumePreviewAfterExport =
                player->playbackState() == QMediaPlayer::PlayingState;
            const bool resumeSpectrumTimerAfterExport = spectrumTimer->isActive();
            player->pause();
            spectrumTimer->stop();
            statusBar()->showMessage(
                "Preview paused while exporting to reduce system load.",
                5000
                );

            auto *progress = new QProgressDialog(
                "Preparing video export...",
                "Cancel",
                0,
                100,
                this
                );
            progress->setWindowTitle("Exporting MP4");
            progress->setWindowModality(Qt::WindowModal);
            progress->setMinimumDuration(0);
            progress->setValue(0);

            auto *process = new QProcess(this);
            process->setProcessChannelMode(QProcess::SeparateChannels);

            connect(progress, &QProgressDialog::canceled, process, [process]() {
                if (process->state() != QProcess::NotRunning)
                    process->terminate();
            });

            connect(process, &QProcess::readyReadStandardOutput, this,
                    [this, process, progress]() {
                const QByteArray output = process->readAllStandardOutput();
                const QList<QByteArray> lines = output.split('\n');
                for (const QByteArray &line : lines) {
                    if (!line.startsWith("out_time_ms="))
                        continue;
                    bool ok = false;
                    const qint64 timeUs = line.mid(12).trimmed().toLongLong(&ok);
                    const qint64 durationMs = player->duration();
                    if (ok && durationMs > 0) {
                        const int percent = qBound(
                            0,
                            static_cast<int>((timeUs / 1000.0) * 100.0 / durationMs),
                            99
                            );
                        progress->setValue(percent);
                    }
                }
            });

            connect(process, &QProcess::readyReadStandardError, this,
                    [process]() {
                const QByteArray errors = process->readAllStandardError();
                if (!errors.isEmpty())
                    qDebug().noquote() << "FFmpeg:" << QString::fromLocal8Bit(errors);
            });

            connect(process,
                    qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    this,
                    [this, process, progress, finalOutputPath, useQsv,
                     resumePreviewAfterExport, resumeSpectrumTimerAfterExport](
                        int exitCode, QProcess::ExitStatus exitStatus) {
                progress->setValue(100);
                progress->close();

                if (resumePreviewAfterExport)
                    player->play();
                if (resumeSpectrumTimerAfterExport)
                    spectrumTimer->start();

                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    statusBar()->showMessage("MP4 export completed", 5000);
                    QMessageBox::information(
                        this,
                        "Export Complete",
                        "The MP4 video with an audio-reactive Spectrum Bars overlay was exported successfully to:\n" +
                            finalOutputPath +
                            "\n\nPhase 1 uses FFmpeg's Spectrum Bars renderer. Its appearance is an initial preset; matching Preview styles and color controls will be added later."
                        );
                } else {
                    const QString errorText = QString::fromLocal8Bit(
                        process->readAllStandardError()
                        ).trimmed();
                    QMessageBox::warning(
                        this,
                        "Export Failed",
                        (useQsv ? "Intel Quick Sync export failed. Retry with CPU H.264 (libx264).\n\n" : "FFmpeg could not complete the export.\n\n") +
                            (errorText.isEmpty()
                                 ? QString("The operation was cancelled or FFmpeg returned an error.")
                                 : errorText.left(1800))
                        );
                    QFile::remove(finalOutputPath);
                }

                process->deleteLater();
                progress->deleteLater();
            });

            connect(process, &QProcess::errorOccurred, this,
                    [this, process, progress,
                     resumePreviewAfterExport, resumeSpectrumTimerAfterExport](
                        QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    progress->close();
                    if (resumePreviewAfterExport)
                        player->play();
                    if (resumeSpectrumTimerAfterExport)
                        spectrumTimer->start();
                    QMessageBox::critical(
                        this,
                        "Could Not Start FFmpeg",
                        "FFmpeg could not be started. Check the FFmpeg path and permissions."
                        );
                    process->deleteLater();
                    progress->deleteLater();
                }
            });

            // Phase 1 visualizer export: generate an audio-reactive bar spectrum
            // from the source audio and composite it over the source video.
            if (!spectrumAnalyzer->isReady()) {
                QMessageBox::warning(
                    this,
                    "Spectrum Analysis Not Ready",
                    "Wait until Spectrum Analyzer finishes analyzing the video's audio, then export again."
                );
                if (resumePreviewAfterExport)
                    player->play();
                if (resumeSpectrumTimerAfterExport)
                    spectrumTimer->start();
                process->deleteLater();
                progress->deleteLater();
                return;
            }

            // B3.3.1: render the Spectrum at the source video's native resolution.
            // This avoids scaling a 960x540 overlay up during export, which can soften
            // thin lines, circular curves, and other fine Spectrum details.
            QSize sourceSize = spectrumDisplay->getVideoSink()->videoSize();
            if (!sourceSize.isValid() || sourceSize.isEmpty())
                sourceSize = QSize(1920, 1080);
            const QSize overlaySize = sourceSize;

            const int exportFps = 30;
            qint64 totalFrames = qMax<qint64>(
                1, (qMax<qint64>(1, player->duration()) * exportFps + 999) / 1000);

            // B3.3.2.1: count actual source-video packets when the source is
            // constant 30 FPS. Container duration can round up by several frames;
            // -shortest then closes stdin before the extra overlay frames arrive.
            // Counting packets avoids decoding the video a second time.
            const QString ffprobePath = QFileInfo(ffmpegPath).dir().filePath("ffprobe.exe");
            if (QFileInfo::exists(ffprobePath)) {
                QProcess probe;
                probe.start(ffprobePath, QStringList{
                    "-v", "error", "-select_streams", "v:0",
                    "-count_packets", "-show_entries",
                    "stream=r_frame_rate,avg_frame_rate,nb_read_packets",
                    "-of", "default=noprint_wrappers=1", inputPath
                });
                if (probe.waitForStarted(3000)) {
                    if (!probe.waitForFinished(15000)) {
                        probe.kill();
                        probe.waitForFinished(1000);
                        qWarning() << "[B3.3.2.1] ffprobe timed out; using duration estimate";
                    } else if (probe.exitStatus() == QProcess::NormalExit &&
                               probe.exitCode() == 0) {
                        const QList<QByteArray> fields =
                            probe.readAllStandardOutput().trimmed().split('\n');
                        QByteArray rate, avgRate;
                        qint64 packetCount = 0;
                        for (const QByteArray &field : fields) {
                            if (field.startsWith("r_frame_rate="))
                                rate = field.mid(sizeof("r_frame_rate=") - 1).trimmed();
                            else if (field.startsWith("avg_frame_rate="))
                                avgRate = field.mid(sizeof("avg_frame_rate=") - 1).trimmed();
                            else if (field.startsWith("nb_read_packets="))
                                packetCount = field.mid(sizeof("nb_read_packets=") - 1)
                                                  .trimmed().toLongLong();
                        }
                        if (rate == "30/1" && avgRate == "30/1" &&
                            packetCount > 0 &&
                            packetCount <= totalFrames + exportFps * 2 &&
                            packetCount >= totalFrames - exportFps * 2) {
                            qDebug() << "[B3.3.2.1] Source packet count:"
                                     << packetCount << "duration estimate:" << totalFrames;
                            totalFrames = packetCount;
                        } else {
                            qDebug() << "[B3.3.2.1] Using duration estimate:"
                                     << totalFrames << "source rate:" << rate
                                     << "average rate:" << avgRate
                                     << "packet count:" << packetCount;
                        }
                    }
                }
            }
            auto frameGenerator = std::make_shared<SpectrumExportFrames>(
                spectrumAnalyzer, spectrumOverlay->exportSettings(),
                overlaySize, exportFps);
            frameGenerator->setGradientSpeed(spectrumOverlay->exportGradientSpeed());
            frameGenerator->setDirectRgba(directRgba);

            // The overlay is already rendered at source resolution; do not scale it.
            const QString filterGraph =
                "[1:v]setpts=PTS-STARTPTS,format=yuv420p[base];"
                "[0:v]format=rgba[spec];"
                "[base][spec]overlay=0:0:shortest=1:format=auto,"
                "format=yuv420p[outv]";

            progress->setLabelText("Rendering Spectrum frames into MP4...");
            QStringList arguments = {
                "-y", "-hide_banner", "-nostats", "-benchmark",
                "-progress", "pipe:1",
                "-f", "rawvideo", "-pixel_format", "rgba",
                "-video_size", QString::number(overlaySize.width()) + "x" +
                               QString::number(overlaySize.height()),
                "-framerate", QString::number(exportFps),
                "-i", "pipe:0",
                "-i", inputPath,
                "-filter_complex", filterGraph,
                "-map", "[outv]", "-map", "1:a:0",
                "-c:a", "aac", "-b:a", "192k",
                "-shortest", "-movflags", "+faststart",
                finalOutputPath
            };

            // QSV global_quality and CPU CRF are not identical scales.
            const int qsvQuality = crf == 18 ? 20 : (crf == 28 ? 30 : 25);
            const QStringList videoCodecArguments = useQsv
                ? QStringList{"-c:v", "h264_qsv", "-preset", "veryfast",
                              "-global_quality", QString::number(qsvQuality),
                              "-pix_fmt", "nv12"}
                : QStringList{"-c:v", "libx264", "-preset", "veryfast",
                              "-crf", QString::number(crf),
                              "-pix_fmt", "yuv420p"};
            const int codecInsertAt = arguments.indexOf("-c:a");
            for (int i = 0; i < videoCodecArguments.size(); ++i)
                arguments.insert(codecInsertAt + i, videoCodecArguments[i]);
            qDebug().noquote() << "[B3.4.1] Encoder:"
                               << (useQsv ? "h264_qsv" : "libx264");

            // B3.3.2.1: event-driven pipe feed. Produce a new frame only after
            // Qt has drained the previous frame, avoiding 10 ms polling.
            struct ExportMetrics {
                QElapsedTimer wall;
                qint64 renderMs = 0;
                qint64 convertMs = 0;
                qint64 frames = 0;
                qint64 lastReportMs = 0;
                qint64 firstFrameMs = -1;
                qint64 lastFrameQueuedMs = 0;
                qint64 pipeBytes = 0;
                qint64 pipeWriteMs = 0;
                qint64 maxPipeWriteMs = 0;
                qint64 renderMaxMs = 0;
                qint64 convertMaxMs = 0;
                bool inputClosed = false;
                bool feedScheduled = false;
                bool processFinished = false;
            };
            auto metrics = std::make_shared<ExportMetrics>();
            metrics->wall.start();

            auto pump = std::make_shared<std::function<void()>>();
            std::weak_ptr<std::function<void()>> weakPump = pump;
            *pump = [process, frameGenerator, totalFrames, metrics, weakPump]() {
                metrics->feedScheduled = false;
                if (metrics->inputClosed || metrics->processFinished ||
                    process->state() != QProcess::Running)
                    return;
                if (process->bytesToWrite() != 0)
                    return;

                if (frameGenerator->nextFrameIndex() >= totalFrames) {
                    metrics->inputClosed = true;
                    process->closeWriteChannel();
                    qDebug().noquote() << "[B3.3.2] All overlay frames queued:"
                                       << metrics->frames
                                       << "elapsed_ms=" << metrics->wall.elapsed()
                                       << "render_ms=" << metrics->renderMs
                                       << "convert_ms=" << metrics->convertMs;
                    return;
                }

                // B3.4.2.3 experiment: batch two RGBA frames into one QProcess::write
                // to reduce per-call overhead. Keep batches small (16 MiB at 1080p)
                // so memory use stays bounded and the output byte stream remains exact.
                constexpr int kFramesPerWriteBatch = 2;
                QByteArray batch;

                int framesInBatch = 0;
                while (framesInBatch < kFramesPerWriteBatch &&
                       frameGenerator->nextFrameIndex() < totalFrames) {
                    QElapsedTimer step;
                    step.start();
                    QImage overlay = frameGenerator->nextOverlay();
                    const qint64 renderStepMs = step.elapsed();
                    metrics->renderMs += renderStepMs;
                    metrics->renderMaxMs = qMax(metrics->renderMaxMs, renderStepMs);
                    if (overlay.isNull()) {
                        metrics->inputClosed = true;
                        process->closeWriteChannel();
                        qWarning() << "[B3.3.2] Overlay generator returned an empty frame";
                        return;
                    }

                    step.restart();
                    if (overlay.format() != QImage::Format_RGBA8888)
                        overlay = overlay.convertToFormat(QImage::Format_RGBA8888);
                    const qint64 convertStepMs = step.elapsed();
                    metrics->convertMs += convertStepMs;
                    metrics->convertMaxMs = qMax(metrics->convertMaxMs, convertStepMs);
                    batch.append(reinterpret_cast<const char *>(overlay.constBits()),
                                 int(overlay.sizeInBytes()));
                    ++framesInBatch;
                }

                if (framesInBatch == 0)
                    return;

                QElapsedTimer pipeStepTimer;
                pipeStepTimer.start();
                const qint64 accepted = process->write(batch.constData(), batch.size());
                const qint64 pipeStepMs = pipeStepTimer.elapsed();
                metrics->pipeWriteMs += pipeStepMs;
                metrics->maxPipeWriteMs = qMax(metrics->maxPipeWriteMs, pipeStepMs);
                if (accepted != batch.size()) {
                    metrics->inputClosed = true;
                    qWarning() << "[B3.4.2.3] Batched pipe write failed:" << accepted
                               << "of" << batch.size() << "bytes";
                    if (process->state() == QProcess::Running)
                        process->closeWriteChannel();
                    return;
                }
                if (metrics->firstFrameMs < 0)
                    metrics->firstFrameMs = metrics->wall.elapsed();
                metrics->lastFrameQueuedMs = metrics->wall.elapsed();
                metrics->pipeBytes += accepted;
                metrics->frames += framesInBatch;
                const qint64 elapsed = metrics->wall.elapsed();
                if (elapsed - metrics->lastReportMs >= 10000) {
                    metrics->lastReportMs = elapsed;
                    qDebug().noquote() << "[B3.3.2] export diagnostic:"
                                       << "frames=" << metrics->frames << "/" << totalFrames
                                       << "elapsed_ms=" << elapsed
                                       << "render_ms=" << metrics->renderMs
                                       << "convert_ms=" << metrics->convertMs
                                       << "queued_bytes=" << process->bytesToWrite();
                }
                // If Qt accepted the frame without buffering, schedule the next
                // one asynchronously. Otherwise bytesWritten wakes the pump.
                if (process->bytesToWrite() == 0 && !metrics->feedScheduled) {
                    metrics->feedScheduled = true;
                    QTimer::singleShot(0, process, [weakPump]() {
                        if (auto fn = weakPump.lock()) (*fn)();
                    });
                }
            };

            connect(process, &QProcess::bytesWritten, process,
                    [process, metrics, weakPump](qint64) {
                if (metrics->inputClosed || metrics->feedScheduled ||
                    process->state() != QProcess::Running ||
                    process->bytesToWrite() != 0)
                    return;
                metrics->feedScheduled = true;
                QTimer::singleShot(0, process, [weakPump]() {
                    if (auto fn = weakPump.lock()) (*fn)();
                });
            });
            connect(process, &QProcess::started, process,
                    [process, metrics, weakPump]() {
                metrics->feedScheduled = true;
                QTimer::singleShot(0, process, [weakPump]() {
                    if (auto fn = weakPump.lock()) (*fn)();
                });
            });
            connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    process, [metrics, pump, useQsv, directRgba, totalFrames](int, QProcess::ExitStatus) {
                metrics->inputClosed = true;
                qDebug().noquote() << "[B3.3.2] FFmpeg finished:"
                                   << "frames_queued=" << metrics->frames
                                   << "elapsed_ms=" << metrics->wall.elapsed()
                                   << "render_ms=" << metrics->renderMs
                                   << "convert_ms=" << metrics->convertMs;
                qDebug().noquote()
                    << "[B3.4.2.1] PERFORMANCE SUMMARY"
                    << "encoder=" << (useQsv ? "h264_qsv" : "libx264")
                    << "pixel_pipeline=" << (directRgba ? "direct_rgba" : "stable_conversion")
                    << "write_batch_frames=2"
                    << "frames=" << metrics->frames << "/" << totalFrames
                    << "wall_ms=" << metrics->wall.elapsed()
                    << "first_frame_ms=" << metrics->firstFrameMs
                    << "last_frame_queued_ms=" << metrics->lastFrameQueuedMs
                    << "render_total_ms=" << metrics->renderMs
                    << "render_max_ms=" << metrics->renderMaxMs
                    << "convert_total_ms=" << metrics->convertMs
                    << "convert_max_ms=" << metrics->convertMaxMs
                    << "pipe_write_total_ms=" << metrics->pipeWriteMs
                    << "pipe_write_max_ms=" << metrics->maxPipeWriteMs
                    << "pipe_bytes=" << metrics->pipeBytes
                    << "ffmpeg_finish_after_last_queue_ms="
                    << (metrics->wall.elapsed() - metrics->lastFrameQueuedMs);
                // FFmpeg -benchmark prints aggregate user/system CPU time and peak RSS.
                // These measurements overlap with rendering and pipe delivery;
                // they must not be added together as sequential phases.
            });
            statusBar()->showMessage("Starting MP4 Spectrum export...", 3000);
            process->start(ffmpegPath, arguments);
        }
        );

    connect(
        exportAction,
        &QAction::triggered,
        exportButton,
        &QPushButton::click
        );

    connect(
        newAction,
        &QAction::triggered,
        this,
        [this]() {
            player->stop();
            player->setSource(QUrl());
            spectrumAnalyzer->stop();
            spectrumDisplay->setBands(QVector<float>());
            spectrumOverlay->setBands(QVector<float>());
            spectrumDisplay->setMessage("Import a video to analyze its audio spectrum");
            spectrumOverlay->setMessage("Import a video to preview Spectrum Overlay");
            analyzerStatusLabel->setText("Waiting for media");

            videoInfoLabel->setText("No video loaded");
            positionSlider->setRange(0, 0);
            positionSlider->setEnabled(false);

            updateTimeLabel(0, 0);

            statusBar()->showMessage(
                "New project started",
                3000
                );
        }
        );

    connect(
        saveProjectButton,
        &QPushButton::clicked,
        this,
        [this]() {
            QMessageBox::information(
                this,
                "Save Project",
                "Project saving will be implemented "
                "in a later step."
                );
        }
        );

    connect(
        addEffectButton,
        &QPushButton::clicked,
        this,
        [this, effectsList]() {
            const QString effectName =
                effectsList->currentItem()
                    ? effectsList->currentItem()->text()
                    : QString("Audio Waveform");

            statusBar()->showMessage(
                effectName +
                    " selected. Effect processing "
                    "will be implemented next.",
                4000
                );
        }
        );

    Q_UNUSED(addTrackButton);
    Q_UNUSED(zoomOutButton);
    Q_UNUSED(zoomInButton);
    Q_UNUSED(editMenu);
    Q_UNUSED(viewMenu);
    // GPU foundation diagnostics; existing CPU video/Spectrum rendering stays intact.
    QAction *gpuDiagnosticsAction = helpMenu->addAction("GPU Renderer Diagnostics...");
    connect(gpuDiagnosticsAction, &QAction::triggered, this, [this]() {
        GpuRenderer &gpu = GpuRenderer::instance();
        gpu.initialize();
        QMessageBox::information(this, "Direct3D 11 Diagnostics", gpu.diagnostics());
    });
    QTimer::singleShot(0, this, [this]() {
        GpuRenderer &gpu = GpuRenderer::instance();
        gpu.initialize();
        statusBar()->showMessage(gpu.isHardwareAccelerated()
            ? "Direct3D 11 hardware device ready | Spectrum Bars: GPU test (Phase 2)"
            : "GPU diagnostics available in Help > GPU Renderer Diagnostics", 9000);
    });
    Q_UNUSED(helpMenu);
}

MainWindow::~MainWindow()
{
    spectrumTimer->stop();
    spectrumAnalyzer->stop();
    player->stop();
    delete ui;
}

// =============================================================
// OPEN VIDEO
// =============================================================

void MainWindow::openVideo()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        "Import Video",
        QDir::homePath(),
        "Video Files (*.mp4 *.m4v *.mov *.mkv *.avi *.wmv);;"
        "All Files (*.*)"
        );

    if (fileName.isEmpty()) {
        return;
    }

    player->stop();

    videoInfoLabel->setText(
        QFileInfo(fileName).fileName()
        );

    player->setSource(QUrl::fromLocalFile(fileName));

    spectrumTimer->stop();
    spectrumAnalyzer->analyzeFile(fileName);

    positionSlider->setValue(0);
    positionSlider->setEnabled(true);

    updateTimeLabel(0, 0);

    statusBar()->showMessage(
        "Loading video: " + QFileInfo(fileName).fileName(),
        5000
        );

    player->play();
}

// =============================================================
// PLAY / PAUSE
// =============================================================

void MainWindow::togglePlayback()
{
    if (player->source().isEmpty()) {
        openVideo();
        return;
    }

    if (player->playbackState()
        == QMediaPlayer::PlayingState) {
        player->pause();
    } else {
        player->play();
    }
}

// =============================================================
// UPDATE PLAYBACK POSITION
// =============================================================

void MainWindow::updatePosition(qint64 position)
{
    if (!isSeeking) {
        QSignalBlocker blocker(positionSlider);

        positionSlider->setValue(
            static_cast<int>(position)
            );
    }

    updateTimeLabel(position, player->duration());
}

// =============================================================
// UPDATE VIDEO DURATION
// =============================================================

void MainWindow::updateDuration(qint64 duration)
{
    positionSlider->setRange(
        0,
        static_cast<int>(
            qMin(duration, qint64(2147483647))
            )
        );

    positionSlider->setEnabled(duration > 0);

    updateTimeLabel(player->position(), duration);
}

// =============================================================
// UPDATE PLAY BUTTON
// =============================================================

void MainWindow::updatePlaybackState(
    QMediaPlayer::PlaybackState state)
{
    if (state == QMediaPlayer::PlayingState) {
        playButton->setText("⏸ Pause");
    } else {
        playButton->setText("▶ Play");
    }
}

// =============================================================
// TIME DISPLAY
// =============================================================

QString MainWindow::formatTime(qint64 milliseconds) const
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }

    const qint64 totalSeconds = milliseconds / 1000;

    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

void MainWindow::updateTimeLabel(
    qint64 position,
    qint64 duration)
{
    timeLabel->setText(
        formatTime(position) +
        " / " +
        formatTime(duration)
        );
}