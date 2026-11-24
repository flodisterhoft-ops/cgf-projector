/***************************************************************************
//
//    CGF Projector - library releases pushed from the CGF publisher
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

// The main window's side of pushed library releases (docs/song-push.md). The
// app itself never talks to the internet: the companion program holds the
// connection to the push server and hands releases to the bridge on this
// computer. Kept out of softprojector.cpp, like the bridge.

#include "../headers/softprojector.hpp"
#include "../headers/librarypush.hpp"
#include "../headers/livebridge.hpp"
#include "ui_softprojector.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>

void SoftProjector::setupLibraryPush()
{
    const QString dataDir = QFileInfo(QSqlDatabase::database().databaseName()).absolutePath();
    libraryPush = new LibraryPush(dataDir, this);
    connect(libraryPush, &LibraryPush::libraryChanged, this, &SoftProjector::applyLibraryChanges);
    if(!liveBridge)
        return;
    liveBridge->setLibraryVersion(libraryPush->version());
    connect(libraryPush, &LibraryPush::versionChanged, liveBridge, &LiveBridge::setLibraryVersion);
    connect(liveBridge, &LiveBridge::releaseReceived, this, [this](const QJsonObject &envelope, quintptr client) {
        liveBridge->replyToClient(client, libraryPush->handleRelease(envelope));
    });
    connect(liveBridge, &LiveBridge::bridgeStarted, this, &SoftProjector::startCompanion);
}

void SoftProjector::startCompanion(quint16 bridgePort)
{
    if(!libraryPush->hasCloud() || qEnvironmentVariableIsSet("CGF_NO_COMPANION"))
        return;
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("cgf-companion.exe");
#else
    const QString name = QStringLiteral("cgf-companion");
#endif
    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if(!QFile::exists(exe))
    {
        qWarning("Library updates: %s is missing, so pushed songs arrive only with the next snapshot",
                 qPrintable(name));
        return;
    }
    // It stops by itself once this app has been gone for a minute, and a
    // second copy (after a restart) exits at once.
    if(!QProcess::startDetached(exe, {QStringLiteral("--config"), libraryPush->configPath(),
                                      QStringLiteral("--bridge-port"), QString::number(bridgePort)}))
        qWarning("Library updates: cannot start %s", qPrintable(exe));
}

void SoftProjector::applyLibraryChanges(const QList<int> &songs, const QList<int> &deletedSongs,
                                        bool songbooksChanged)
{
    songWidget->applyLibraryChanges(songs, deletedSongs, songbooksChanged);

    // Schedule entries hold their own copy of each song. Refresh those; the
    // projector screen shows yet another copy (current_song), which is never
    // changed here: an edited song reaches the screen the next time it goes live.
    SongDatabase db;
    for(int i = 0; i < schedule.size(); ++i)
    {
        Schedule &item = schedule[i];
        if(item.stype != QLatin1String("song") || !songs.contains(item.song.songID))
            continue;
        item.song = db.getSongWithSongbook(item.song.songID);
        item.name = QStringLiteral("%1 %2").arg(item.song.number).arg(item.song.title);
        if(QListWidgetItem *row = ui->listWidgetSchedule->item(i))
            row->setText(item.name);
    }
}
