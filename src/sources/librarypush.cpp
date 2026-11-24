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

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <map>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#endif

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
    for(const QJsonValue &k : o.value(QStringLiteral("publisherKeys")).toArray())
    {
        const QByteArray key = QByteArray::fromBase64(k.toString().toLatin1());
        if(key.size() == 64)
            publisherKeys << key;
        else
            qWarning("%s: a publisher key is not a P-256 public key", configFileName);
    }
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

    if(to <= currentVersion)
        return result(to, QStringLiteral("duplicate"));
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
    QList<int> songs, deleted;
    bool songbooks = false;
    const QString error = apply(to, release, songs, deleted, songbooks);
    if(!error.isEmpty())
    {
        qWarning("Library release v%d -> v%d refused: %s", from, to, qPrintable(error));
        return result(to, QStringLiteral("rejected"), error);
    }
    currentVersion = to;
    // Older half-received releases can never apply now.
    for(auto it = pending.begin(); it != pending.end(); )
        it = it.key() <= currentVersion ? pending.erase(it) : std::next(it);
    qInfo("Library release v%d -> v%d applied: %lld songs changed or added, %lld removed%s",
          from, to, qlonglong(songs.size()), qlonglong(deleted.size()), songbooks ? ", songbooks changed" : "");
    emit versionChanged(currentVersion);
    emit libraryChanged(songs, deleted, songbooks);
    return result(to, QStringLiteral("applied"));
}

QString LibraryPush::apply(int to, const Pending &release, QList<int> &songs, QList<int> &deleted,
                           bool &songbooks)
{
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

    QString error;
    for(const QJsonObject &body : release.bodies) // in part order
    {
        const QJsonArray books = body.value(QStringLiteral("songbooks")).toArray();
        const QJsonArray booksGone = body.value(QStringLiteral("songbooks_deleted")).toArray();
        songbooks = songbooks || !books.isEmpty() || !booksGone.isEmpty();
        if(!upsertRows(db, QStringLiteral("Songbooks"), books, bookCols, {}, 0, nullptr, error))
            break;
        for(const QJsonValue &id : booksGone)
        {
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
        songs.clear();
        deleted.clear();
        songbooks = false;
        return error;
    }
    if(!db.commit())
    {
        const QString e = db.lastError().text();
        db.rollback();
        return e;
    }
    return QString();
}

bool LibraryPush::verifySignature(const QByteArray &publicKey, const QByteArray &message,
                                  const QByteArray &signature)
{
    if(publicKey.size() != 64 || signature.size() != 64)
        return false;
#ifdef Q_OS_WIN
    BCRYPT_ALG_HANDLE alg = nullptr;
    if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    QByteArray blob(sizeof(BCRYPT_ECCKEY_BLOB), '\0');
    auto *header = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    blob.append(publicKey);
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if(BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                          reinterpret_cast<PUCHAR>(blob.data()), ULONG(blob.size()), 0)))
    {
        QByteArray hash = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
        QByteArray sig = signature;
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(hash.data()),
                                                  ULONG(hash.size()), reinterpret_cast<PUCHAR>(sig.data()),
                                                  ULONG(sig.size()), 0));
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    Q_UNUSED(message);
    qWarning("Pushed library releases need Windows to check their signature; refused");
    return false;
#endif
}
