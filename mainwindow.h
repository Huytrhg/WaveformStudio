#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMediaPlayer>
#include <QVector>

class QAudioOutput;
class QVideoWidget;
class QLabel;
class QPushButton;
class QSlider;
class QCloseEvent;
class QTimer;
class SpectrumAnalyzer;
class SpectrumDisplay;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindow *ui;

    QMediaPlayer *player;
    QAudioOutput *audioOutput;
    QVideoWidget *videoWidget;

    QPushButton *playButton;
    QSlider *positionSlider;
    QLabel *timeLabel;
    QLabel *videoInfoLabel;

    SpectrumAnalyzer *spectrumAnalyzer;
    SpectrumDisplay *spectrumDisplay;
    SpectrumDisplay *spectrumOverlay;
    QTimer *spectrumTimer;
    QLabel *analyzerStatusLabel;

    bool isSeeking;

    void openVideo();
    void togglePlayback();
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    void updatePlaybackState(QMediaPlayer::PlaybackState state);
    void updateTimeLabel(qint64 position, qint64 duration);
    QString formatTime(qint64 milliseconds) const;
};

#endif // MAINWINDOW_H
