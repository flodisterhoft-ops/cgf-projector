/***************************************************************************
//
//    CGF Projector - network bridge
//    Copyright (C) 2026  CGF Vernon
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation version 3 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
***************************************************************************/

#ifndef LIVEBRIDGE_HPP
#define LIVEBRIDGE_HPP

#include <QObject>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QPixmap>
#include <QThread>

class BridgeServer;

// Shares what the projector shows with browsers on the church network: the
// OBS stream overlay now, the back screen and phone remote later. This side
// runs on the projector's (GUI) thread and only hands data over; the network
// work happens in BridgeServer on its own thread, so a slow client can never
// make a projector transition stutter.
class LiveBridge : public QObject
{
    Q_OBJECT

public:
    explicit LiveBridge(QObject *parent = nullptr);
    ~LiveBridge();

    // What the projector shows now (see SoftProjector::publishLive). Repeats
    // of the same state are dropped here.
    void publish(const QJsonObject &live, const QPixmap &picture = QPixmap(),
                 const QString &videoPath = QString());

public slots:
    void videoStateChanged(QMediaPlayer::PlaybackState state);
    void videoPositionChanged(qint64 position);
    void videoDurationChanged(qint64 duration);
    // The projector's rendered layers, for the back-screen mirror. Only
    // converted and sent while a mirror page is connected.
    void frameShown(const QPixmap &text, const QPixmap &back, const QSize &screen, bool fade);
    // Library releases (docs/song-push.md)
    void setLibraryVersion(int version);
    void replyToClient(quintptr client, const QJsonObject &message);

signals:
    void bridgeStarted(quint16 port);
    void releaseReceived(const QJsonObject &envelope, quintptr client);

private slots:
    void setFramesWanted(bool wanted);

private:
    void sendVideo();
    void sendFrame(bool whole);

    QThread thread;
    BridgeServer *server;
    QJsonObject lastLive;
    qint64 pictureKey;
    QString lastVideoPath;

    QMediaPlayer::PlaybackState videoState;
    qint64 videoPosition;
    qint64 videoDuration;
    qint64 positionSent;
    QElapsedTimer sinceVideoSent;

    bool framesWanted = false;
    QPixmap frameText;
    QPixmap frameBack;
    QSize frameSize;
    bool frameFade = false;
    qint64 sentTextKey = 0;
    qint64 sentBackKey = 0;
};

#endif // LIVEBRIDGE_HPP
