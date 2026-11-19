#ifndef TIMELINEWIDGET_H
#define TIMELINEWIDGET_H
#include <QWidget>
#include <QColor>
#include <QVector>
class TimelineWidget:public QWidget{
 Q_OBJECT
public: explicit TimelineWidget(QWidget* p=nullptr); bool canUndo()const; bool canRedo()const; void undo(); void redo(); void addTrack(const QString& name=QString()); void addClip(const QString& track,const QString& name,qint64 duration); void addImportedVideoClip(const QString& sourcePath,const QString& name,qint64 duration); void setPlayhead(qint64); void setTimelineDuration(qint64); void zoomBy(qreal);
signals: void seekRequested(qint64); void historyChanged(bool canUndo,bool canRedo);
protected: void paintEvent(QPaintEvent*)override; void mousePressEvent(QMouseEvent*)override; void mouseMoveEvent(QMouseEvent*)override; void mouseReleaseEvent(QMouseEvent*)override; void keyPressEvent(QKeyEvent*)override; void contextMenuEvent(QContextMenuEvent*)override;
private:
 struct Clip{QString name,sourcePath;qint64 start=0,duration=10000;QColor color;};struct Track{QString name;QColor color;QVector<Clip> clips;};struct State{QVector<Track> tracks;qint64 duration=60000;};
 int trackAt(int)const;int clipAt(int,int)const;qreal timeToX(qint64)const;qint64 xToTime(qreal)const;void resizeContent();void deleteSelectedClip();void recordHistory();void restoreHistoryState();
 QVector<Track> tracks;QVector<State> history;int historyIndex=-1;qint64 duration=60000,playhead=0;qreal scale=48;int labelW=116,rulerH=28,rowH=42,selectedTrack=-1,selectedClip=-1,dragTargetTrack=-1,mode=0;QPoint origin;qint64 oldStart=0,oldDuration=0;
};
#endif
