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

#include "../headers/livebridge.hpp"
#include "../headers/bridgeserver.hpp"

#include <QFileInfo>
#include <QSqlDatabase>

LiveBridge::LiveBridge(QObject *parent)
    : QObject(parent),
      server(nullptr),
      pictureKey(0),
      videoState(QMediaPlayer::StoppedState),
      videoPosition(0),
      videoDuration(0),
      positionSent(0)
{
    // The settings file sits beside the database, which sits beside the exe.
    const QString dbPath = QSqlDatabase::database().databaseName();
    server = new BridgeServer(QFileInfo(dbPath).absolutePath(), dbPath);
    server->moveToThread(&thread);
    connect(&thread, &QThread::started, server, &BridgeServer::start);
    connect(&thread, &QThread::finished, server, &QObject::deleteLater);
    thread.setObjectName(QStringLiteral("Network bridge"));
    thread.start(QThread::LowPriority);
    sinceVideoSent.start();
}

LiveBridge::~LiveBridge()
{
    thread.quit();
    thread.wait(5000);
}

void LiveBridge::publish(const QJsonObject &live, const QPixmap &picture, const QString &videoPath)
{
    if(live == lastLive)
        return;
    lastLive = live;

    // Convert a picture once; after that the server keeps serving its copy.
    QImage image;
    if(!picture.isNull() && picture.cacheKey() != pictureKey)
    {
        pictureKey = picture.cacheKey();
        image = picture.toImage();
    }
    const bool newVideo = !videoPath.isEmpty() && videoPath != lastVideoPath;
    if(newVideo)
        lastVideoPath = videoPath;

    QMetaObject::invokeMethod(server, [s = server, live, image, videoPath] {
        s->setLive(live, image, videoPath);
    }, Qt::QueuedConnection);

    // The player reported "playing" while loading this file, before it was
    // published; repeat the state now that clients know which file it is.
    if(newVideo)
    {
        videoPosition = 0;
        videoDuration = 0;
        sendVideo();
    }
}

void LiveBridge::videoStateChanged(QMediaPlayer::PlaybackState state)
{
    videoState = state;
    sendVideo();
}

void LiveBridge::videoDurationChanged(qint64 duration)
{
    videoDuration = duration;
}

void LiveBridge::videoPositionChanged(qint64 position)
{
    // Positions arrive many times a second. Browsers keep playing on their
    // own, so send one only after a seek, and every few seconds to correct drift.
    videoPosition = position;
    const qint64 expected = positionSent
            + (videoState == QMediaPlayer::PlayingState ? sinceVideoSent.elapsed() : 0);
    if(qAbs(position - expected) > 750 || sinceVideoSent.elapsed() > 5000)
        sendVideo();
}

void LiveBridge::sendVideo()
{
    positionSent = videoPosition;
    sinceVideoSent.restart();
    const char *state = videoState == QMediaPlayer::PlayingState ? "playing"
                      : videoState == QMediaPlayer::PausedState ? "paused" : "stopped";
    const QJsonObject video{{"state", state}, {"position", videoPosition}, {"duration", videoDuration}};
    QMetaObject::invokeMethod(server, [s = server, video] { s->setVideo(video); }, Qt::QueuedConnection);
}
