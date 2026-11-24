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
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>

// Applies signed library releases (changed songs and songbooks) to the local
// database the moment they arrive, so new and edited songs appear without a
// restart. The release comes from the companion program over the local
// bridge; this class trusts only the publisher's signature, never the path it
// took. The song on the projector screen is a separate copy and is never
// touched here. Protocol: docs/song-push.md.
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

    // ECDSA P-256 / SHA-256; key is X||Y (64 bytes), signature r||s (64 bytes).
    static bool verifySignature(const QByteArray &publicKey, const QByteArray &message,
                                const QByteArray &signature);

signals:
    void versionChanged(int version);
    // After a release is in the database: songs added or changed, songs
    // removed, and whether songbooks changed.
    void libraryChanged(const QList<int> &songs, const QList<int> &deletedSongs, bool songbooksChanged);

private:
    struct Pending
    {
        int from = 0;
        int parts = 0;
        QMap<int, QJsonObject> bodies; // part number -> payload
    };

    void loadConfig();
    int readVersion() const;
    QString apply(int to, const Pending &release, QList<int> &songs, QList<int> &deleted,
                  bool &songbooks);
    QJsonObject result(int to, const QString &status, const QString &reason = QString()) const;

    QString dataDir;
    QString church;
    bool cloud = false;
    QList<QByteArray> publisherKeys;
    int currentVersion = 0;
    QMap<int, Pending> pending; // releases still missing parts, by target version
};

#endif // LIBRARYPUSH_HPP
