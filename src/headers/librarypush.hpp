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

#ifndef LIBRARYPUSH_HPP
#define LIBRARYPUSH_HPP

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

// Applies signed library releases (changed songs, songbooks and Bibles) to
// the local database the moment they arrive, so they appear without a
// restart. The release comes from the companion program over the local
// bridge; this class trusts only the publisher's signature, never the path it
// took. The song on the projector screen is a separate copy and is never
// touched here; a release that changes Bibles waits while a Bible passage is
// the projector's item. Protocol: docs/song-push.md.
class LibraryPush : public QObject
{
    Q_OBJECT

public:
    explicit LibraryPush(const QString &dataDir, QObject *parent = nullptr);

    // The library version of the local database: the snapshot the sync agent
    // installed (version.txt) or the last release applied here, whichever is newer.
    int version() const { return currentVersion; }
    // cgf-cloud.json names a push server, so the companion should run.
    bool hasCloud() const { return cloud; }
    QString configPath() const;

    // Handles one release part; returns the "releaseResult" message for the sender.
    QJsonObject handleRelease(const QJsonObject &envelope);

    // What this version understands in a release's "needs" list. The bridge
    // announces it, so the companion holds back releases an older app would
    // misread.
    static QStringList features();

    // The main window reports whether a Bible passage is the projector's item
    // (shown or hidden). While it is, a release that changes Bibles is held;
    // it applies once the passage leaves.
    void setBibleOnScreen(bool onScreen);
    bool isHolding() const { return held.parts > 0; }

    // ECDSA P-256 / SHA-256; key is X||Y (64 bytes), signature r||s (64 bytes).
    static bool verifySignature(const QByteArray &publicKey, const QByteArray &message,
                                const QByteArray &signature);

signals:
    void versionChanged(int version);
    // After a release is in the database: songs added or changed, songs
    // removed, and whether songbooks changed.
    void libraryChanged(const QList<int> &songs, const QList<int> &deletedSongs, bool songbooksChanged);
    // After Bibles changed: those replaced or added, those removed, and which
    // Bible takes over a removed one in the screen settings (old id -> new id).
    void biblesChanged(const QStringList &changed, const QStringList &removed,
                       const QMap<QString, QString> &replaces);
    // What an applied release added, updated or removed, for the What's new
    // window: {"songs": [...], "songbooks": [...], "bibles": [...]} (whatsnew.hpp).
    void libraryRecorded(const QJsonObject &changes);
    // A held release was applied (or refused) after its Bible passage left
    // the projector: the "releaseResult" for whoever sent it.
    void heldReleaseFinished(const QJsonObject &result);

private:
    struct Pending
    {
        int from = 0;
        int parts = 0;
        QMap<int, QJsonObject> bodies; // part number -> payload
    };

    struct Changes
    {
        QList<int> songs, deletedSongs;
        bool songbooks = false;
        QStringList bibles, deletedBibles;
        QMap<QString, QString> replaces;
        QJsonArray songEvents, bookEvents, bibleEvents; // for libraryRecorded
    };

    void loadConfig();
    int readVersion() const;
    QJsonObject finish(int to, const Pending &release);
    QString apply(int to, const Pending &release, Changes &changes);
    QString applyBibles(const Pending &release, Changes &changes);
    void applyHeld();
    QJsonObject result(int to, const QString &status, const QString &reason = QString()) const;

    QString dataDir;
    QString church;
    bool cloud = false;
    QList<QByteArray> publisherKeys;
    int currentVersion = 0;
    QMap<int, Pending> pending; // releases still missing parts, by target version
    // A complete release that changes Bibles, waiting for the Bible passage to
    // leave the projector (heldTo is its version; held.parts 0 = none).
    Pending held;
    int heldTo = 0;
    bool bibleOnScreen = false;
};

#endif // LIBRARYPUSH_HPP
