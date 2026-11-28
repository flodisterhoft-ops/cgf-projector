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
#include "../headers/biblewidget.hpp"
#include "ui_softprojector.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>

void SoftProjector::setupLibraryPush()
{
    const QString dataDir = QFileInfo(QSqlDatabase::database().databaseName()).absolutePath();
    libraryPush = new LibraryPush(dataDir, this);
    connect(libraryPush, &LibraryPush::libraryChanged, this, &SoftProjector::applyLibraryChanges);
    connect(libraryPush, &LibraryPush::biblesChanged, this, &SoftProjector::applyBibleChanges);

    // Screen settings that name a Bible this laptop no longer has (a snapshot
    // install, or a hand import the library removed).
    QMap<QString, QString> replaces;
    QSqlQuery q;
    if(q.exec(QStringLiteral("SELECT old_id, new_id FROM CgfBibleReplaces")))
        while(q.next())
            replaces.insert(q.value(0).toString(), q.value(1).toString());
    fixBibleSettings(replaces);
    libraryPush->setBibleOnScreen(bibleOnScreen());

    if(!liveBridge)
        return;
    liveBridge->setLibraryVersion(libraryPush->version());
    connect(libraryPush, &LibraryPush::versionChanged, liveBridge, &LiveBridge::setLibraryVersion);
    connect(liveBridge, &LiveBridge::releaseReceived, this, [this](const QJsonObject &envelope, quintptr client) {
        const QJsonObject r = libraryPush->handleRelease(envelope);
        if(r.value(QStringLiteral("status")).toString() == QLatin1String("held"))
            heldReleaseClient = client;
        liveBridge->replyToClient(client, r);
    });
    connect(libraryPush, &LibraryPush::heldReleaseFinished, this, [this](const QJsonObject &r) {
        if(heldReleaseClient)
            liveBridge->replyToClient(heldReleaseClient, r);
        heldReleaseClient = 0;
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

// A Bible passage is the projector's item, shown or hidden: Show would draw
// it again from the database, so Bible releases wait until it is gone.
bool SoftProjector::bibleOnScreen() const
{
    return pType == BIBLE && ui->listShow->count() > 0;
}

void SoftProjector::applyBibleChanges(const QStringList &changed, const QStringList &removed,
                                      const QMap<QString, QString> &replaces)
{
    const QString operatorBefore = mySettings.bibleSets.operatorBible == QLatin1String("same")
            ? mySettings.bibleSets.primaryBible : mySettings.bibleSets.operatorBible;
    fixBibleSettings(replaces);
    // The Bible tab lists the operator's Bible; reload it only if that one
    // changed, keeping the book, chapter and selected verses.
    if(changed.contains(operatorBefore) || removed.contains(operatorBefore))
        bibleWidget->reloadBible();
    if(liveBridge)
        liveBridge->reloadBibles();
}

// Settings naming a Bible that is not in the database follow the library's
// replacement (old id -> new id); otherwise the first Bible stays primary and
// the other slots go empty, so the projector never shows a missing Bible.
void SoftProjector::fixBibleSettings(const QMap<QString, QString> &replaces)
{
    QStringList ids;
    QSqlQuery q;
    if(q.exec(QStringLiteral("SELECT id FROM BibleVersions ORDER BY id")))
        while(q.next())
            ids << q.value(0).toString();
    if(ids.isEmpty())
        return;
    bool changed = false;
    auto fix = [&](QString &value, const QString &fallback, const char *slot) {
        if(value.isEmpty() || value == QLatin1String("none") || value == QLatin1String("same") || ids.contains(value))
            return;
        QString target = value;
        for(int i = 0; i < 5 && !ids.contains(target) && replaces.contains(target); ++i)
            target = replaces.value(target);
        if(!ids.contains(target))
            target = fallback;
        qInfo("Bible settings: %s Bible %s is gone, now %s", slot, qPrintable(value), qPrintable(target));
        value = target;
        changed = true;
    };
    for(BibleVersionSettings *b : {&mySettings.bibleSets, &mySettings.bibleSets2,
                                   &mySettings.bibleSets3, &mySettings.bibleSets4})
    {
        fix(b->primaryBible, ids.first(), "primary");
        fix(b->secondaryBible, QStringLiteral("none"), "secondary");
        fix(b->trinaryBible, QStringLiteral("none"), "third");
        fix(b->operatorBible, QStringLiteral("same"), "operator");
    }
    if(changed)
        updateSetting(mySettings.general, theme, mySettings.slideSets, mySettings.bibleSets,
                      mySettings.bibleSets2, mySettings.bibleSets3, mySettings.bibleSets4);
}
