#include "spectrumanalyzer.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

    namespace {

    constexpr int FFT_SIZE = 2048;
    constexpr int DEFAULT_SAMPLE_RATE = 44100;
    constexpr int DEFAULT_CHANNELS = 2;

    struct ComplexValue {
        float real;
        float imag;
    };

    void fft(ComplexValue *data, int size)
    {
        int j = 0;

        for (int i = 1; i < size; ++i) {
            int bit = size >> 1;

            while (j & bit) {
                j ^= bit;
                bit >>= 1;
            }

            j ^= bit;

            if (i < j) {
                std::swap(data[i], data[j]);
            }
        }

        for (int length = 2; length <= size; length <<= 1) {
            const float angle =
                -2.0f * float(M_PI) / float(length);

            const float wLenReal = std::cos(angle);
            const float wLenImag = std::sin(angle);

            for (int i = 0; i < size; i += length) {
                float wReal = 1.0f;
                float wImag = 0.0f;

                const int half = length / 2;

                for (int k = 0; k < half; ++k) {
                    const ComplexValue u = data[i + k];
                    const ComplexValue v = {
                        data[i + k + half].real * wReal
                            - data[i + k + half].imag * wImag,
                        data[i + k + half].real * wImag
                            + data[i + k + half].imag * wReal
                    };

                    data[i + k].real = u.real + v.real;
                    data[i + k].imag = u.imag + v.imag;

                    data[i + k + half].real = u.real - v.real;
                    data[i + k + half].imag = u.imag - v.imag;

                    const float nextReal =
                        wReal * wLenReal - wImag * wLenImag;

                    const float nextImag =
                        wReal * wLenImag + wImag * wLenReal;

                    wReal = nextReal;
                    wImag = nextImag;
                }
            }
        }
    }

} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(QObject *parent)
    : QObject(parent)
    , process(new QProcess(this))
    , audioSampleRate(DEFAULT_SAMPLE_RATE)
    , audioChannels(DEFAULT_CHANNELS)
    , totalDurationMs(0)
    , ready(false)
    , parsingOutput(false)
{
    connect(
        process,
        &QProcess::finished,
        this,
        [this](int exitCode, QProcess::ExitStatus status) {
            processFinished(exitCode, status);
        }
        );

    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this](QProcess::ProcessError error) {
            processError(error);
        }
        );

    connect(
        process,
        &QProcess::readyReadStandardOutput,
        this,
        [this]() {
            if (parsingOutput) {
                pcmData.append(process->readAllStandardOutput());
            }
        }
        );
}

SpectrumAnalyzer::~SpectrumAnalyzer()
{
    stop();
}

bool SpectrumAnalyzer::isReady() const
{
    return ready;
}

int SpectrumAnalyzer::durationMs() const
{
    return totalDurationMs;
}

int SpectrumAnalyzer::sampleRate() const
{
    return audioSampleRate;
}

void SpectrumAnalyzer::analyzeFile(const QString &videoFile)
{
    stop();

    ready = false;
    pcmData.clear();
    totalDurationMs = 0;

    const QFileInfo info(videoFile);

    if (!info.exists() || !info.isFile()) {
        emit analysisError("Không tìm thấy tệp video.");
        return;
    }

    const QString ffmpegPath =
        QStringLiteral("C:/ffmpeg/bin/ffmpeg.exe");

    if (!QFileInfo::exists(ffmpegPath)) {
        emit analysisError(
            "Không tìm thấy ffmpeg.exe tại C:/ffmpeg/bin/."
            );
        return;
    }

    parsingOutput = true;

    QStringList arguments;
    arguments << "-hide_banner"
              << "-loglevel" << "error"
              << "-nostdin"
              << "-i" << videoFile
              << "-map" << "0:a:0"
              << "-vn"
              << "-ac" << QString::number(audioChannels)
              << "-ar" << QString::number(audioSampleRate)
              << "-f" << "f32le"
              << "-acodec" << "pcm_f32le"
              << "pipe:1";

    emit analysisStarted();

    process->setProgram(ffmpegPath);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();
}

void SpectrumAnalyzer::stop()
{
    parsingOutput = false;

    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
}

void SpectrumAnalyzer::processFinished(
    int exitCode,
    QProcess::ExitStatus status)
{
    parsingOutput = false;

    pcmData.append(process->readAllStandardOutput());

    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString errorText =
            QString::fromLocal8Bit(
                process->readAllStandardError()
                ).trimmed();

        pcmData.clear();

        emit analysisError(
            errorText.isEmpty()
                ? "FFmpeg không thể trích xuất âm thanh."
                : errorText
            );

        return;
    }

    const qint64 bytesPerSecond =
        qint64(audioSampleRate) * audioChannels * sizeof(float);

    if (bytesPerSecond <= 0) {
        emit analysisError("Thông số âm thanh không hợp lệ.");
        return;
    }

    totalDurationMs = int(
        (qint64(pcmData.size()) * 1000) / bytesPerSecond
        );

    if (pcmData.size() < FFT_SIZE * audioChannels
                             * int(sizeof(float))) {
        emit analysisError(
            "Âm thanh quá ngắn để phân tích phổ tần."
            );
        return;
    }

    ready = true;

    emit analysisProgress(100);
    emit analysisFinished();
}

void SpectrumAnalyzer::processError(
    QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        parsingOutput = false;

        emit analysisError(
            "Không khởi động được FFmpeg. "
            "Hãy kiểm tra đường dẫn C:/ffmpeg/bin/ffmpeg.exe."
            );
    }
}

QVector<float> SpectrumAnalyzer::spectrumAt(
    int positionMs,
    int bandCount)
{
    if (!ready || bandCount <= 0) {
        return {};
    }

    // B3.3.3: center the FFT window on the media timestamp.
    // Preview and export share this exact audio sampling convention.
    const qint64 centerSample =
        (qint64(qMax(0, positionMs)) * audioSampleRate) / 1000;
    const qint64 windowStart = qMax<qint64>(0, centerSample - FFT_SIZE / 2);

    return computeSpectrum(windowStart, bandCount);
}

QVector<float> SpectrumAnalyzer::computeSpectrum(
    qint64 startSample,
    int bandCount) const
{
    QVector<float> result(bandCount, 0.0f);

    const qint64 totalSamples =
        pcmData.size() / (audioChannels * int(sizeof(float)));

    if (totalSamples <= 0) {
        return result;
    }

    const qint64 maxStart =
        qMax(qint64(0), totalSamples - FFT_SIZE);

    startSample = qBound(
        qint64(0),
        startSample,
        maxStart
        );

    QVector<ComplexValue> buffer(FFT_SIZE);

    for (int i = 0; i < FFT_SIZE; ++i) {
        float left = 0.0f;
        float right = 0.0f;

        const qint64 sampleIndex = startSample + i;

        const qint64 byteOffset =
            sampleIndex * audioChannels * int(sizeof(float));

        if (byteOffset + 2 * int(sizeof(float)) <= pcmData.size()) {
            std::memcpy(
                &left,
                pcmData.constData() + byteOffset,
                sizeof(float)
                );

            std::memcpy(
                &right,
                pcmData.constData() + byteOffset + sizeof(float),
                sizeof(float)
                );
        }

        const float mono = 0.5f * (left + right);

        const float window =
            0.5f - 0.5f * std::cos(
                       2.0f * float(M_PI) * float(i)
                       / float(FFT_SIZE - 1)
                       );

        buffer[i] = {mono * window, 0.0f};
    }

    fft(buffer.data(), FFT_SIZE);

    const int maxBin = FFT_SIZE / 2;
    const float minFrequency = 30.0f;
    const float maxFrequency = 18000.0f;

    for (int band = 0; band < bandCount; ++band) {
        const float t0 = float(band) / float(bandCount);
        const float t1 = float(band + 1) / float(bandCount);

        const float lowFrequency =
            minFrequency * std::pow(
                maxFrequency / minFrequency,
                t0
                );

        const float highFrequency =
            minFrequency * std::pow(
                maxFrequency / minFrequency,
                t1
                );

        int lowBin = qMax(
            1,
            int(lowFrequency * FFT_SIZE / audioSampleRate)
            );

        int highBin = qMax(
            lowBin + 1,
            int(highFrequency * FFT_SIZE / audioSampleRate)
            );

        highBin = qMin(highBin, maxBin);

        float sum = 0.0f;
        int count = 0;

        for (int bin = lowBin; bin < highBin; ++bin) {
            const float real = buffer[bin].real;
            const float imag = buffer[bin].imag;

            const float magnitude =
                std::sqrt(real * real + imag * imag)
                / float(FFT_SIZE);

            sum += magnitude;
            ++count;
        }

        float magnitude = count > 0
                              ? sum / float(count)
                              : 0.0f;

        // B4.3.1: compensate gently for bass-heavy music without fabricating
        // frequencies or mirroring the spectrum. Both preview and export use
        // this analyzer, so Bars and Circular receive identical balanced data.
        const float t = (float(band) + 0.5f) / float(bandCount);
        const float spectralLift = 1.0f + 3.0f * std::pow(t, 1.35f);
        const float balancedMagnitude = magnitude * spectralLift;
        const float value = std::log1p(balancedMagnitude * 120.0f)
                          / std::log1p(120.0f);
        result[band] = qBound(0.0f, value, 1.0f);
    }
    // B4.3.1.1: smooth per-frame spectral envelope compensation.
    // Keep actual FFT peaks; never invent or mirror frequency content.
    if (bandCount >= 8) {
        QVector<float> envelope(bandCount, 0.0f);
        const int radius = qMax(3, bandCount / 10);
        float total = 0.0f;
        for (int i = 0; i < bandCount; ++i) {
            float weighted = 0.0f, weights = 0.0f;
            for (int j = qMax(0, i - radius); j <= qMin(bandCount - 1, i + radius); ++j) {
                const float d = float(j - i) / float(radius);
                const float w = std::exp(-2.0f * d * d);
                weighted += result[j] * w;
                weights += w;
            }
            envelope[i] = weighted / qMax(0.0001f, weights);
            total += envelope[i];
        }
        const float average = total / float(bandCount);
        if (average > 0.012f) {
            for (int i = 0; i < bandCount; ++i) {
                const float ratio = (average + 0.035f) / (envelope[i] + 0.035f);
                const float gain = qBound(0.72f, std::pow(ratio, 0.85f), 3.2f);
                const float adjusted = result[i] * gain;
                result[i] = qBound(0.0f, 0.18f * result[i] + 0.82f * adjusted, 1.0f);
            }
        }
    }

    return result;
}
