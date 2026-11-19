#include "timelinewidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QKeySequence>
#include <QSizePolicy>
#include <QPolygon>
#include <QtMath>
TimelineWidget::TimelineWidget(QWidget*p):QWidget(p){setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Minimum);addTrack("Video 1");addTrack("Audio 1");addTrack("Spectrum 1");addTrack("Overlay 1");history.clear();historyIndex=-1;recordHistory();resizeContent();}
void TimelineWidget::addTrack(const QString&n){static const QColor cs[]={QColor("#345B78"),QColor("#386B55"),QColor("#59478A"),QColor("#805A35"),QColor("#476D78")};Track t;t.name=n.isEmpty()?QString("Track %1").arg(tracks.size()+1):n;t.color=cs[tracks.size()%5];tracks.append(t);recordHistory();resizeContent();update();}
void TimelineWidget::addClip(const QString&t,const QString&n,qint64 d){int i=-1;for(int j=0;j<tracks.size();++j)if(tracks[j].name.startsWith(t,Qt::CaseInsensitive)){i=j;break;}if(i<0){addTrack(t);i=tracks.size()-1;}Clip c;c.name=n;c.duration=qMax<qint64>(500,d);c.color=tracks[i].color;tracks[i].clips.append(c);duration=qMax(duration,c.duration+10000);recordHistory();resizeContent();update();}
void TimelineWidget::addImportedVideoClip(const QString&path,const QString&name,qint64 d){if(path.isEmpty()||d<=0)return;int i=-1;for(int j=0;j<tracks.size();++j)if(tracks[j].name.compare("Video 1",Qt::CaseInsensitive)==0){i=j;break;}if(i<0){addTrack("Video 1");i=tracks.size()-1;}for(const Clip&existing:tracks[i].clips)if(existing.sourcePath==path)return;Clip c;c.name=name;c.sourcePath=path;c.duration=qMax<qint64>(1,d);c.color=tracks[i].color;for(const Clip&existing:tracks[i].clips)c.start=qMax(c.start,existing.start+existing.duration);tracks[i].clips.append(c);duration=qMax(duration,c.start+c.duration+10000);recordHistory();resizeContent();update();}
bool TimelineWidget::canUndo()const{return historyIndex>0;}
bool TimelineWidget::canRedo()const{return historyIndex>=0&&historyIndex+1<history.size();}
void TimelineWidget::recordHistory(){State s;s.tracks=tracks;s.duration=duration;if(historyIndex>=0&&historyIndex<history.size()){history.resize(historyIndex+1);}history.append(s);if(history.size()>100)history.removeFirst();historyIndex=history.size()-1;emit historyChanged(canUndo(),canRedo());}
void TimelineWidget::restoreHistoryState(){if(historyIndex<0||historyIndex>=history.size())return;tracks=history[historyIndex].tracks;duration=history[historyIndex].duration;selectedTrack=-1;selectedClip=-1;dragTargetTrack=-1;mode=0;playhead=qBound<qint64>(0,playhead,duration);resizeContent();update();emit historyChanged(canUndo(),canRedo());}
void TimelineWidget::undo(){if(!canUndo())return;--historyIndex;restoreHistoryState();}
void TimelineWidget::redo(){if(!canRedo())return;++historyIndex;restoreHistoryState();}
void TimelineWidget::setPlayhead(qint64 p){playhead=qBound<qint64>(0,p,duration);update();}
void TimelineWidget::setTimelineDuration(qint64 d){if(d>0)duration=qMax(duration,d);resizeContent();update();}
void TimelineWidget::zoomBy(qreal f){scale=qBound<qreal>(12,scale*f,240);resizeContent();update();}
qreal TimelineWidget::timeToX(qint64 t)const{return labelW+t/1000.0*scale;}qint64 TimelineWidget::xToTime(qreal x)const{return qMax<qint64>(0,qRound64((x-labelW)*1000/scale));}
int TimelineWidget::trackAt(int y)const{int i=(y-rulerH)/rowH;return y>=rulerH&&i>=0&&i<tracks.size()?i:-1;}
int TimelineWidget::clipAt(int x,int y)const{int r=trackAt(y);if(r<0)return -1;for(int i=tracks[r].clips.size()-1;i>=0;--i){auto c=tracks[r].clips[i];if(x>=timeToX(c.start)&&x<=timeToX(c.start+c.duration))return i;}return -1;}
void TimelineWidget::resizeContent(){int w=qMax(900,qRound(timeToX(duration+30000)));setMinimumSize(w,rulerH+tracks.size()*rowH);resize(w,rulerH+tracks.size()*rowH);}
void TimelineWidget::paintEvent(QPaintEvent*){QPainter p(this);p.fillRect(rect(),QColor("#17191E"));p.fillRect(QRect(0,0,width(),rulerH),QColor("#22252B"));p.fillRect(QRect(0,rulerH,labelW,height()),QColor("#202329"));p.setFont(QFont("Segoe UI",8));qint64 step=scale<28?10000:5000;for(qint64 t=0;t<=duration+30000;t+=step){int x=qRound(timeToX(t));p.setPen(QColor("#3A3E47"));p.drawLine(x,rulerH,x,height());p.setPen(QColor("#AAB1BC"));p.drawText(x+4,18,QString("%1s").arg(t/1000));}for(int r=0;r<tracks.size();++r){int y=rulerH+r*rowH;p.fillRect(QRect(0,y,labelW,rowH),QColor("#22262D"));p.fillRect(QRect(labelW,y,width()-labelW,rowH),QColor("#191C22"));p.setPen(QColor("#D9DEE7"));p.drawText(QRect(10,y,labelW-15,rowH),Qt::AlignVCenter,tracks[r].name);p.setPen(QColor("#30343D"));p.drawLine(0,y+rowH-1,width(),y+rowH-1);for(int j=0;j<tracks[r].clips.size();++j){auto c=tracks[r].clips[j];QRect b(qRound(timeToX(c.start)),y+5,qMax(8,qRound(c.duration/1000.0*scale)),rowH-10);p.setPen(QPen(r==selectedTrack&&j==selectedClip?Qt::white:c.color.lighter(140),1));p.setBrush(c.color);p.drawRoundedRect(b,4,4);p.setPen(Qt::white);p.drawText(b.adjusted(6,0,-6,0),Qt::AlignVCenter|Qt::AlignLeft,c.name);p.fillRect(QRect(b.left(),b.top(),4,b.height()),c.color.lighter(160));p.fillRect(QRect(b.right()-3,b.top(),4,b.height()),c.color.lighter(160));}}if(mode==1&&dragTargetTrack>=0&&dragTargetTrack<tracks.size()&&dragTargetTrack!=selectedTrack&&selectedTrack>=0&&selectedClip>=0&&selectedClip<tracks[selectedTrack].clips.size()){const Clip c=tracks[selectedTrack].clips[selectedClip];const int y=rulerH+dragTargetTrack*rowH;QRect ghost(qRound(timeToX(c.start)),y+5,qMax(8,qRound(c.duration/1000.0*scale)),rowH-10);p.save();p.setOpacity(0.62);p.setPen(QPen(tracks[dragTargetTrack].color.lighter(170),1,Qt::DashLine));p.setBrush(tracks[dragTargetTrack].color);p.drawRoundedRect(ghost,4,4);p.setPen(Qt::white);p.drawText(ghost.adjusted(6,0,-6,0),Qt::AlignVCenter|Qt::AlignLeft,c.name);p.restore();}int x=qRound(timeToX(playhead));p.setPen(QPen(QColor("#FF5D67"),2));p.drawLine(x,0,x,height());p.setBrush(QColor("#FF5D67"));p.drawPolygon(QPolygon({QPoint(x-6,0),QPoint(x+6,0),QPoint(x,9)}));}
void TimelineWidget::mousePressEvent(QMouseEvent*e){
 if(e->button()!=Qt::LeftButton)return;
 setFocus(Qt::MouseFocusReason);
 const qreal x=e->position().x();
 const qreal y=e->position().y();
 const int playheadX=qRound(timeToX(playhead));

 // The ruler is a direct seek/scrub surface: clicking seeks immediately,
 // and keeping the button held allows continuous Playhead dragging.
 if(y<rulerH){
  mode=4;
  playhead=xToTime(x);
  emit seekRequested(playhead);
  update();
  return;
 }

 // Give the visible Playhead line a forgiving hit area. This check comes
 // before clip selection so dragging the Playhead never moves/trims a clip.
 if(x>=labelW&&qAbs(x-playheadX)<=8){
  mode=4;
  playhead=xToTime(x);
  emit seekRequested(playhead);
  update();
  return;
 }

 selectedTrack=trackAt(qRound(y));
 selectedClip=clipAt(qRound(x),qRound(y));
 mode=0;
 if(selectedTrack>=0&&selectedClip>=0){
  auto c=tracks[selectedTrack].clips[selectedClip];
  qreal l=timeToX(c.start),r=timeToX(c.start+c.duration);
  mode=x-l<8?2:(r-x<8?3:1);
  origin=e->pos();
  oldStart=c.start;
  oldDuration=c.duration;
  dragTargetTrack=selectedTrack;
 }
 update();
}
void TimelineWidget::mouseMoveEvent(QMouseEvent*e){
 if(!(e->buttons()&Qt::LeftButton))return;

 // Mode 4 is reserved for Playhead scrubbing and is independent of
 // selectedTrack/selectedClip, so it works over empty space and clip rows.
 if(mode==4){
  playhead=xToTime(e->position().x());
  emit seekRequested(playhead);
  update();
  return;
 }
 if(selectedTrack<0||selectedClip<0||!mode)return;
 Clip&c=tracks[selectedTrack].clips[selectedClip];
 qint64 d=qRound64((e->position().x()-origin.x())*1000/scale);
 if(mode==1){c.start=qMax<qint64>(0,oldStart+d);dragTargetTrack=trackAt(qRound(e->position().y()));}
 else if(mode==2){qint64 v=qBound<qint64>(-oldStart,d,oldDuration-500);c.start=oldStart+v;c.duration=oldDuration-v;}
 else c.duration=qMax<qint64>(500,oldDuration+d);
 duration=qMax(duration,c.start+c.duration+10000);
 resizeContent();
 update();
}
void TimelineWidget::mouseReleaseEvent(QMouseEvent*){
 if(mode==1&&dragTargetTrack>=0&&dragTargetTrack<tracks.size()&&dragTargetTrack!=selectedTrack&&selectedTrack>=0&&selectedTrack<tracks.size()&&selectedClip>=0&&selectedClip<tracks[selectedTrack].clips.size()){
  Clip moved=tracks[selectedTrack].clips.takeAt(selectedClip);
  moved.color=tracks[dragTargetTrack].color;
  tracks[dragTargetTrack].clips.append(moved);
  selectedTrack=dragTargetTrack;
  selectedClip=tracks[selectedTrack].clips.size()-1;
 }
 mode=0;
 dragTargetTrack=-1;
 update();
}

void TimelineWidget::deleteSelectedClip(){
 if(selectedTrack<0||selectedTrack>=tracks.size()||selectedClip<0||selectedClip>=tracks[selectedTrack].clips.size())return;
 tracks[selectedTrack].clips.removeAt(selectedClip);
 selectedClip=-1;
 mode=0;
 dragTargetTrack=-1;
 update();
}
void TimelineWidget::keyPressEvent(QKeyEvent*e){
 if(e->key()==Qt::Key_Delete||e->key()==Qt::Key_Backspace){
  if(selectedTrack>=0&&selectedClip>=0){deleteSelectedClip();e->accept();return;}
 }
 QWidget::keyPressEvent(e);
}
void TimelineWidget::contextMenuEvent(QContextMenuEvent*e){
 const int track=trackAt(e->pos().y());
 const int clip=clipAt(e->pos().x(),e->pos().y());
 if(track<0||clip<0){QWidget::contextMenuEvent(e);return;}
 selectedTrack=track;
 selectedClip=clip;
 mode=0;
 dragTargetTrack=-1;
 update();
 QMenu menu(this);
 QAction* removeAction=menu.addAction(tr("Delete Clip"));
 removeAction->setShortcut(QKeySequence::Delete);
 if(menu.exec(e->globalPos())==removeAction)deleteSelectedClip();
 e->accept();
}
