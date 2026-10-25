#ifndef SPECTRUMANALYZER_H
#define SPECTRUMANALYZER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QProcess>
#include <QByteArray>

    class SpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    explicit SpectrumAnalyzer(QObject *parent = nullptr);
    ~SpectrumAnalyzer();

    bool isReady() const;

    void analyzeFile(const QString &videoFile);
    void stop();

    int durationMs() const;
    int sampleRate() const;

    QVector<float> spectrumAt(int positionMs, int bandCount = 64);

signals:
    void analysisStarted();
    void analysisProgress(int percentage);
    void analysisFinished();
    void analysisError(const QString &message);

private:
    QProcess *process;
    QByteArray pcmData;

    int audioSampleRate;
    int audioChannels;
    int totalDurationMs;

    bool ready;
    bool parsingOutput;

    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);

    QVector<float> computeSpectrum(
        qint64 startSample,
        int bandCount
        ) const;
};

#endif
