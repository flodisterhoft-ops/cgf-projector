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

#include "../headers/librarypush.hpp"
#include "../headers/cgfsignature.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <map>
#include <memory>

namespace {

const char configFileName[] = "cgf-cloud.json";
// Song ids above this belong to the laptop's own quick-adds; a release never
// touches them (the sync agent carries them across snapshots too).
const qint64 localSongBand = 500000;
// Usage counters stay the laptop's own.
const QStringList localSongColumns = {"count", "date"};
// Releases waiting for missing parts; more than this means something is wrong.
const int maxPending = 4;

QStringList columnNames(QSqlDatabase &db, const QString &table)
{
    QStringList cols;
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA table_info('%1')").arg(table));
    while(q.next())
        cols << q.value(1).toString();
    return cols;
}

QVariant sqlValue(const QJsonValue &v)
{
    switch(v.type())
    {
    case QJsonValue::Bool:
        return v.toBool() ? 1 : 0;
    case QJsonValue::Double:
    {
        const qint64 i = v.toInteger(0);
        if(double(i) == v.toDouble())
            return i;
        return v.toDouble();
    }
    case QJsonValue::String:
        return v.toString();
    case QJsonValue::Object:
        // BLOBs travel as {"$b64": "..."}
        return QByteArray::fromBase64(v.toObject().value(QStringLiteral("$b64")).toString().toLatin1());
    default:
        return QVariant(); // NULL
    }
}

// INSERT ... ON CONFLICT(id) DO UPDATE for the given columns. Column names come
// from the table itself (PRAGMA table_info), never from the release alone.
QString upsertSql(const QString &table, const QStringList &cols, const QStringList &keep)
{
    QStringList quoted, marks, updates;
    for(const QString &c : cols)
    {
        quoted << QLatin1Char('"') + c + QLatin1Char('"');
        marks << QStringLiteral("?");
        if(c != QLatin1String("id") && !keep.contains(c))
            updates << QStringLiteral("\"%1\" = excluded.\"%1\"").arg(c);
    }
    return QStringLiteral("INSERT INTO \"%1\" (%2) VALUES (%3) ON CONFLICT(id) DO %4")
            .arg(table, quoted.join(QLatin1Char(',')), marks.join(QLatin1Char(',')),
                 updates.isEmpty() ? QStringLiteral("NOTHING")
                                   : QStringLiteral("UPDATE SET ") + updates.join(QStringLiteral(", ")));
}

bool upsertRows(QSqlDatabase &db, const QString &table, const QJsonArray &rows, const QStringList &known,
                const QStringList &keep, qint64 maxId, QList<int> *ids, QString &error)
{
    std::map<QString, std::unique_ptr<QSqlQuery>> prepared;
    for(const QJsonValue &value : rows)
    {
        const QJsonObject row = value.toObject();
        const qint64 id = row.value(QStringLiteral("id")).toInteger(-1);
        if(id <= 0 || (maxId > 0 && id > maxId))
        {
            error = QStringLiteral("%1 row with id %2 is not allowed").arg(table).arg(id);
            return false;
        }
        QStringList cols;
        for(auto it = row.begin(); it != row.end(); ++it)
            if(known.contains(it.key()))
                cols << it.key();
        const QString sql = upsertSql(table, cols, keep);
        std::unique_ptr<QSqlQuery> &slot = prepared[sql];
        if(!slot)
        {
            slot = std::make_unique<QSqlQuery>(db);
            if(!slot->prepare(sql))
            {
                error = slot->lastError().text();
                return false;
            }
        }
        QSqlQuery &q = *slot;
        for(int i = 0; i < cols.size(); ++i)
            q.bindValue(i, sqlValue(row.value(cols.at(i))));
        if(!q.exec())
        {
            error = QStringLiteral("%1 %2: %3").arg(table).arg(id).arg(q.lastError().text());
            return false;
        }
        if(ids)
            ids->append(int(id));
    }
    return true;
}

} // namespace

LibraryPush::LibraryPush(const QString &dataDir, QObject *parent)
    : QObject(parent),
      dataDir(dataDir)
{
    loadConfig();
    currentVersion = readVersion();
    qInfo("Library version %d%s", currentVersion,
          publisherKeys.isEmpty() ? " (no publisher key: pushed releases are refused)" : "");
}

QString LibraryPush::configPath() const
{
    return QDir(dataDir).filePath(QLatin1String(configFileName));
}

void LibraryPush::loadConfig()
{
    QFile f(configPath());
    if(!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    church = o.value(QStringLiteral("church")).toString();
    cloud = !o.value(QStringLiteral("server")).toString().isEmpty()
            && !o.value(QStringLiteral("token")).toString().isEmpty();
    publisherKeys = cgfPublisherKeys(o.value(QStringLiteral("publisherKeys")).toArray());
}

int LibraryPush::readVersion() const
{
    // version.txt: written by the sync agent when it installs a snapshot.
    int v = 0;
    QFile f(QDir(dataDir).filePath(QStringLiteral("version.txt")));
    if(f.open(QIODevice::ReadOnly))
        v = QString::fromUtf8(f.readLine()).remove(QRegularExpression(QStringLiteral("[^0-9]"))).toInt();
    // CgfSync: the last release applied here. A new snapshot replaces the
    // whole database, and with it this table.
    QSqlQuery q;
    if(q.exec(QStringLiteral("SELECT value FROM CgfSync WHERE key = 'content_version'")) && q.next())
        v = qMax(v, q.value(0).toInt());
    return v;
}

QJsonObject LibraryPush::result(int to, const QString &status, const QString &reason) const
{
    QJsonObject r{{"type", "releaseResult"}, {"to", to}, {"status", status}, {"version", currentVersion}};
    if(!reason.isEmpty())
        r["reason"] = reason;
    return r;
}

QJsonObject LibraryPush::handleRelease(const QJsonObject &envelope)
{
    const QByteArray payload = envelope.value(QStringLiteral("payload")).toString().toUtf8();
    const QByteArray sig = QByteArray::fromBase64(envelope.value(QStringLiteral("sig")).toString().toLatin1());
    if(publisherKeys.isEmpty())
        return result(0, QStringLiteral("rejected"), QStringLiteral("no publisher key in cgf-cloud.json"));
    bool signedOk = false;
    for(const QByteArray &key : std::as_const(publisherKeys))
        if(verifySignature(key, payload, sig))
        {
            signedOk = true;
            break;
        }
    if(!signedOk)
        return result(0, QStringLiteral("rejected"), QStringLiteral("signature does not match the publisher key"));

    const QJsonObject body = QJsonDocument::fromJson(payload).object();
    const int from = body.value(QStringLiteral("from")).toInt(-1);
    const int to = body.value(QStringLiteral("to")).toInt(-1);
    const int part = body.value(QStringLiteral("part")).toInt(0);
    const int parts = body.value(QStringLiteral("parts")).toInt(0);
    if(body.value(QStringLiteral("kind")).toString() != QLatin1String("cgf-release")
            || body.value(QStringLiteral("v")).toInt() != 1
            || from < 0 || to <= from || parts < 1 || part < 1 || part > parts)
        return result(to, QStringLiteral("rejected"), QStringLiteral("not a release this version understands"));
    if(!church.isEmpty() && body.value(QStringLiteral("church")).toString() != church)
        return result(to, QStringLiteral("rejected"), QStringLiteral("release for another church"));
    // A release names what it needs beyond songs; refuse what this version
    // would only half apply.
    const QStringList known = features();
    for(const QJsonValue &need : body.value(QStringLiteral("needs")).toArray())
        if(!known.contains(need.toString()))
            return result(to, QStringLiteral("rejected"),
                          QStringLiteral("needs a newer CGF Projector (%1)").arg(need.toString()));

    if(to <= currentVersion)
        return result(to, QStringLiteral("duplicate"));
    // A Bible release waits for the screen; everything after it waits too.
    if(isHolding() && to >= heldTo)
        return result(to, QStringLiteral("held"));
    // Each release builds on the one before; a laptop that missed one asks
    // the server for it (or, after a long time offline, takes the snapshot).
    if(from != currentVersion)
        return result(to, QStringLiteral("gap"));

    Pending &p = pending[to];
    if(p.parts != parts || p.from != from)
    {
        p = Pending();
        p.from = from;
        p.parts = parts;
    }
    p.bodies.insert(part, body);
    if(p.bodies.size() < parts)
    {
        while(pending.size() > maxPending)
            pending.erase(pending.begin());
        return result(to, QStringLiteral("waiting"));
    }

    const Pending release = pending.take(to);
    bool bibles = false;
    for(const QJsonObject &b : release.bodies)
        bibles = bibles || b.contains(QStringLiteral("bibles")) || b.contains(QStringLiteral("bibles_library"));
    if(bibles && bibleOnScreen)
    {
        held = release;
        heldTo = to;
        qInfo("Library release v%d -> v%d changes Bibles; it applies once the Bible passage leaves the projector",
              from, to);
        return result(to, QStringLiteral("held"));
    }
    return finish(to, release);
}

QStringList LibraryPush::features()
{
    return {QStringLiteral("bibles")};
}

void LibraryPush::setBibleOnScreen(bool onScreen)
{
    bibleOnScreen = onScreen;
    // Not inside the click that took the passage off: let the new item go live first.
    if(!onScreen && isHolding())
        QTimer::singleShot(300, this, &LibraryPush::applyHeld);
}

void LibraryPush::applyHeld()
{
    if(bibleOnScreen || !isHolding())
        return;
    const Pending release = held;
    const int to = heldTo;
    held = Pending();
    heldTo = 0;
    const QJsonObject r = release.from == currentVersion
            ? finish(to, release)
            : result(to, QStringLiteral("gap"));
    emit heldReleaseFinished(r);
}

QJsonObject LibraryPush::finish(int to, const Pending &release)
{
    Changes changes;
    QElapsedTimer took;
    took.start();
    const QString error = apply(to, release, changes);
    if(!error.isEmpty())
    {
        qWarning("Library release v%d -> v%d refused: %s", release.from, to, qPrintable(error));
        return result(to, QStringLiteral("rejected"), error);
    }
    const int from = release.from;
    currentVersion = to;
    // Older half-received releases can never apply now.
    for(auto it = pending.begin(); it != pending.end(); )
        it = it.key() <= currentVersion ? pending.erase(it) : std::next(it);
    QString bibleNote;
    if(!changes.bibles.isEmpty() || !changes.deletedBibles.isEmpty())
        bibleNote = QStringLiteral(", Bibles replaced: %1, removed: %2")
                .arg(changes.bibles.join(QLatin1Char(',')), changes.deletedBibles.join(QLatin1Char(',')));
    qInfo("Library release v%d -> v%d applied in %lld ms: %lld songs changed or added, %lld removed%s%s",
          from, to, qlonglong(took.elapsed()), qlonglong(changes.songs.size()), qlonglong(changes.deletedSongs.size()),
          changes.songbooks ? ", songbooks changed" : "", qPrintable(bibleNote));
    emit versionChanged(currentVersion);
    emit libraryChanged(changes.songs, changes.deletedSongs, changes.songbooks);
    if(!changes.bibles.isEmpty() || !changes.deletedBibles.isEmpty())
        emit biblesChanged(changes.bibles, changes.deletedBibles, changes.replaces);
    if(!changes.songEvents.isEmpty() || !changes.bookEvents.isEmpty() || !changes.bibleEvents.isEmpty())
        emit libraryRecorded(QJsonObject{{"songs", changes.songEvents}, {"songbooks", changes.bookEvents},
                                         {"bibles", changes.bibleEvents}});
    return result(to, QStringLiteral("applied"));
}

QString LibraryPush::apply(int to, const Pending &release, Changes &changes)
{
    QList<int> &songs = changes.songs;
    QList<int> &deleted = changes.deletedSongs;
    bool &songbooks = changes.songbooks;
    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q(db);
    // The bridge thread may be reading verses; wait for it instead of failing.
    q.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    const QStringList songCols = columnNames(db, QStringLiteral("Songs"));
    const QStringList bookCols = columnNames(db, QStringLiteral("Songbooks"));
    if(!songCols.contains(QLatin1String("id")) || !bookCols.contains(QLatin1String("id")))
        return QStringLiteral("this database has no Songs or Songbooks table");
    if(!db.transaction())
        return db.lastError().text();

    // What each row does, for the What's new window: looked up before it is written.
    QSqlQuery songLook(db), bookLook(db);
    songLook.prepare(QStringLiteral("SELECT number, title FROM Songs WHERE id = ?"));
    bookLook.prepare(QStringLiteral("SELECT name FROM Songbooks WHERE id = ?"));
    auto look = [](QSqlQuery &l, qint64 id) {
        l.addBindValue(id);
        return l.exec() && l.next();
    };

    QString error;
    for(const QJsonObject &body : release.bodies) // in part order
    {
        const QJsonArray books = body.value(QStringLiteral("songbooks")).toArray();
        const QJsonArray booksGone = body.value(QStringLiteral("songbooks_deleted")).toArray();
        songbooks = songbooks || !books.isEmpty() || !booksGone.isEmpty();
        for(const QJsonValue &v : books)
        {
            const QJsonObject row = v.toObject();
            const qint64 id = row.value(QStringLiteral("id")).toInteger();
            changes.bookEvents << QJsonObject{{"id", id}, {"s", look(bookLook, id) ? "updated" : "added"},
                                              {"name", row.value(QStringLiteral("name")).toString()}};
        }
        if(!upsertRows(db, QStringLiteral("Songbooks"), books, bookCols, {}, 0, nullptr, error))
            break;
        for(const QJsonValue &id : booksGone)
        {
            if(look(bookLook, id.toInteger()))
                changes.bookEvents << QJsonObject{{"id", id.toInteger()}, {"s", "removed"},
                                                  {"name", bookLook.value(0).toString()}};
            q.prepare(QStringLiteral("DELETE FROM Songbooks WHERE id = ?"));
            q.addBindValue(id.toInteger());
            if(!q.exec())
            {
                error = q.lastError().text();
                break;
            }
        }
        if(!error.isEmpty())
            break;
        for(const QJsonValue &v : body.value(QStringLiteral("songs")).toArray())
        {
            const QJsonObject row = v.toObject();
            const qint64 id = row.value(QStringLiteral("id")).toInteger();
            changes.songEvents << QJsonObject{{"id", id}, {"s", look(songLook, id) ? "updated" : "added"},
                                              {"n", row.value(QStringLiteral("number")).toInt()},
                                              {"t", row.value(QStringLiteral("title")).toString()}};
        }
        if(!upsertRows(db, QStringLiteral("Songs"), body.value(QStringLiteral("songs")).toArray(), songCols,
                       localSongColumns, localSongBand, &songs, error))
            break;
        for(const QJsonValue &value : body.value(QStringLiteral("songs_deleted")).toArray())
        {
            const qint64 id = value.toInteger(-1);
            if(id <= 0 || id > localSongBand)
            {
                error = QStringLiteral("cannot remove song %1").arg(id);
                break;
            }
            if(look(songLook, id))
                changes.songEvents << QJsonObject{{"id", id}, {"s", "removed"}, {"n", songLook.value(0).toInt()},
                                                  {"t", songLook.value(1).toString()}};
            q.prepare(QStringLiteral("DELETE FROM Songs WHERE id = ?"));
            q.addBindValue(id);
            if(!q.exec())
            {
                error = q.lastError().text();
                break;
            }
            deleted << int(id);
        }
        if(!error.isEmpty())
            break;
    }
    if(error.isEmpty())
        error = applyBibles(release, changes);
    if(error.isEmpty())
    {
        q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS CgfSync (key TEXT PRIMARY KEY, value TEXT)"));
        q.prepare(QStringLiteral("INSERT INTO CgfSync (key, value) VALUES ('content_version', ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        q.addBindValue(QString::number(to));
        if(!q.exec())
            error = q.lastError().text();
    }
    if(!error.isEmpty())
    {
        db.rollback();
        changes = Changes();
        return error;
    }
    if(!db.commit())
    {
        const QString e = db.lastError().text();
        db.rollback();
        changes = Changes();
        return e;
    }
    return QString();
}

// Bibles in a release (docs/song-push.md): "bibles" carries each replaced or
// new Bible's BibleVersions row, its books and its verse count, in the part
// where it starts; "bible_verses" carries its verses, spread over the parts;
// "bibles_library" lists every Bible of the library, and any other Bible on
// this laptop is removed; "bible_replaces" says which Bible takes over a
// removed one in the screen settings. Runs inside apply()'s transaction.
QString LibraryPush::applyBibles(const Pending &release, Changes &changes)
{
    QList<QJsonObject> metas;
    QHash<qint64, QList<QJsonArray>> verses; // Bible id -> row batches in part order
    QJsonArray library;
    bool hasLibrary = false;
    QJsonObject replaces;
    for(const QJsonObject &body : release.bodies)
    {
        for(const QJsonValue &m : body.value(QStringLiteral("bibles")).toArray())
            metas << m.toObject();
        for(const QJsonValue &v : body.value(QStringLiteral("bible_verses")).toArray())
        {
            const QJsonObject batch = v.toObject();
            verses[batch.value(QStringLiteral("id")).toInteger(-1)] << batch.value(QStringLiteral("rows")).toArray();
        }
        if(body.contains(QStringLiteral("bibles_library")))
        {
            hasLibrary = true;
            library = body.value(QStringLiteral("bibles_library")).toArray();
        }
        if(body.contains(QStringLiteral("bible_replaces")))
            replaces = body.value(QStringLiteral("bible_replaces")).toObject();
    }
    if(metas.isEmpty() && !hasLibrary)
        return QString();

    QSqlDatabase db = QSqlDatabase::database();
    QSqlQuery q(db);
    QSet<qint64> keep, replaced;
    for(const QJsonValue &id : std::as_const(library))
        keep.insert(id.toInteger(-1));
    if(hasLibrary && keep.isEmpty())
        return QStringLiteral("the library list of Bibles is empty");
    for(const QJsonObject &m : std::as_const(metas))
    {
        const qint64 id = m.value(QStringLiteral("id")).toInteger(-1);
        if(id <= 0 || replaced.contains(id))
            return QStringLiteral("bad or repeated Bible id %1").arg(id);
        if(hasLibrary && !keep.contains(id))
            return QStringLiteral("Bible %1 is not in the library list").arg(id);
        replaced.insert(id);
    }
    for(auto it = verses.cbegin(); it != verses.cend(); ++it)
        if(!replaced.contains(it.key()))
            return QStringLiteral("verses for Bible %1 come without its details").arg(it.key());

    auto removeBible = [&q](qint64 id) -> QString {
        const QStringList sql = {QStringLiteral("DELETE FROM BibleVerse WHERE CAST(bible_id AS INTEGER) = ?"),
                                 QStringLiteral("DELETE FROM BibleBooks WHERE CAST(bible_id AS INTEGER) = ?"),
                                 QStringLiteral("DELETE FROM BibleVersions WHERE id = ?")};
        for(const QString &text : sql)
        {
            q.prepare(text);
            q.addBindValue(id);
            if(!q.exec())
                return q.lastError().text();
        }
        return QString();
    };

    // Bibles this laptop has that the library does not (a hand import).
    if(hasLibrary)
    {
        QList<QPair<qint64, QString>> local;
        if(!q.exec(QStringLiteral("SELECT id, bible_name FROM BibleVersions")))
            return q.lastError().text();
        while(q.next())
            local << qMakePair(q.value(0).toLongLong(), q.value(1).toString().trimmed());
        for(const auto &b : std::as_const(local))
            if(!keep.contains(b.first))
            {
                const QString e = removeBible(b.first);
                if(!e.isEmpty())
                    return e;
                changes.deletedBibles << QString::number(b.first);
                changes.bibleEvents << QJsonObject{{"id", b.first}, {"s", "removed"}, {"name", b.second}};
            }
    }

    QSqlQuery verseInsert(db);
    verseInsert.prepare(QStringLiteral("INSERT INTO BibleVerse (verse_id, bible_id, book, chapter, verse, verse_text) "
                                       "VALUES (?, ?, ?, ?, ?, ?)"));
    QSqlQuery bookInsert(db);
    bookInsert.prepare(QStringLiteral("INSERT INTO BibleBooks (bible_id, id, book_name, chapter_count) VALUES (?, ?, ?, ?)"));
    for(const QJsonObject &m : std::as_const(metas))
    {
        const qint64 id = m.value(QStringLiteral("id")).toInteger();
        const QString idText = QString::number(id);
        q.prepare(QStringLiteral("SELECT 1 FROM BibleVersions WHERE id = ?"));
        q.addBindValue(id);
        const bool existed = q.exec() && q.next();
        changes.bibleEvents << QJsonObject{{"id", id}, {"s", existed ? "updated" : "added"},
                                           {"name", m.value(QStringLiteral("bible_name")).toString().trimmed()}};
        const QString e = removeBible(id);
        if(!e.isEmpty())
            return e;
        q.prepare(QStringLiteral("INSERT INTO BibleVersions (id, bible_name, abbreviation, information, right_to_left) "
                                 "VALUES (?, ?, ?, ?, ?)"));
        q.addBindValue(id);
        q.addBindValue(m.value(QStringLiteral("bible_name")).toString());
        q.addBindValue(m.value(QStringLiteral("abbreviation")).toString());
        q.addBindValue(m.value(QStringLiteral("information")).toString());
        q.addBindValue(m.value(QStringLiteral("right_to_left")).toInt());
        if(!q.exec())
            return QStringLiteral("Bible %1: %2").arg(id).arg(q.lastError().text());
        for(const QJsonValue &b : m.value(QStringLiteral("books")).toArray())
        {
            const QJsonArray book = b.toArray(); // [id, name, chapter count]
            bookInsert.addBindValue(id);
            bookInsert.addBindValue(book.at(0).toInteger());
            bookInsert.addBindValue(book.at(1).toString());
            bookInsert.addBindValue(book.at(2).toInteger());
            if(!bookInsert.exec())
                return QStringLiteral("Bible %1 books: %2").arg(id).arg(bookInsert.lastError().text());
        }
        qint64 count = 0;
        for(const QJsonArray &rows : verses.value(id))
            for(const QJsonValue &r : rows)
            {
                const QJsonArray row = r.toArray(); // [verse_id, book, chapter, verse, text]
                if(row.size() != 5)
                    return QStringLiteral("Bible %1 has a malformed verse").arg(id);
                verseInsert.addBindValue(row.at(0).toString());
                verseInsert.addBindValue(idText);
                verseInsert.addBindValue(row.at(1).toVariant().toString());
                verseInsert.addBindValue(row.at(2).toInteger());
                verseInsert.addBindValue(row.at(3).toInteger());
                verseInsert.addBindValue(row.at(4).toString());
                if(!verseInsert.exec())
                    return QStringLiteral("Bible %1 verses: %2").arg(id).arg(verseInsert.lastError().text());
                ++count;
            }
        const qint64 expected = m.value(QStringLiteral("verses")).toInteger(-1);
        if(count != expected)
            return QStringLiteral("Bible %1 arrived with %2 of %3 verses").arg(id).arg(count).arg(expected);
        changes.bibles << idText;
    }

    // Kept beside the library so a later snapshot install can follow it too.
    if(!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS CgfBibleReplaces (old_id TEXT PRIMARY KEY, new_id TEXT NOT NULL)")))
        return q.lastError().text();
    for(auto it = replaces.constBegin(); it != replaces.constEnd(); ++it)
    {
        const QString target = it.value().toVariant().toString();
        changes.replaces.insert(it.key(), target);
        q.prepare(QStringLiteral("INSERT INTO CgfBibleReplaces (old_id, new_id) VALUES (?, ?) "
                                 "ON CONFLICT(old_id) DO UPDATE SET new_id = excluded.new_id"));
        q.addBindValue(it.key());
        q.addBindValue(target);
        if(!q.exec())
            return q.lastError().text();
    }
    return QString();
}

bool LibraryPush::verifySignature(const QByteArray &publicKey, const QByteArray &message,
                                  const QByteArray &signature)
{
    return cgfVerifySignature(publicKey, message, signature);
}
