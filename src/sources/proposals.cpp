/***************************************************************************
//
//    CGF Projector - operators' song changes on their way to the library
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

#include "../headers/proposals.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

// Table CgfProposals (docs/proposals-up.md):
//   uuid       the proposal's id everywhere
//   kind       "edit" (a library song) or "new" (a quick-add, id above 500000)
//   song_id    this laptop's song id
//   base       the text the operator started from (JSON), empty for "new"
//   proposed   the operator's text (JSON)
//   library    the library's latest text of the song (JSON), for an undo
//   replaces   this laptop's earlier proposal for the song, already sent
//   state      queued -> sent -> applied -> done, or undone, rejected, superseded
//
// An edit's override is on while its state is queued, sent or applied.

namespace {

const qint64 localSongBand = 500000;
Proposals *theInstance = nullptr;
int knownVersion = -1; // the library version, from the main window

// Written back by an override; "songbook" (the name) is for the message only.
const QStringList writeFields = {"title", "songbook_id", "number", "song_text", "category",
                                 "tune", "words", "music", "notes"};
// Compared to tell whether a release brings the operator's change. Notes
// never come from the master, so they do not count.
const QStringList compareFields = {"title", "songbook_id", "number", "song_text",
                                   "category", "tune", "words", "music"};

QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parse(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

QString nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// Line ends and trailing spaces do not count as a change.
QString normalized(const QJsonValue &v)
{
    QString t = v.isDouble() ? QString::number(v.toInteger()) : v.toString();
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    static const QRegularExpression trailing(QStringLiteral("[ \\t]+\\n"));
    t.replace(trailing, QStringLiteral("\n"));
    return t.trimmed();
}

bool sameText(const QJsonObject &a, const QJsonObject &b)
{
    for(const QString &f : compareFields)
        if(normalized(a.value(f)) != normalized(b.value(f)))
            return false;
    return true;
}

QString titleKey(const QJsonValue &v)
{
    return v.toString().simplified().toCaseFolded();
}

QJsonObject fieldsOf(QSqlDatabase &db, int songId)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT s.title, s.songbook_id, s.number, s.song_text, s.category, s.tune, "
                             "s.words, s.music, s.notes, b.name FROM Songs s "
                             "LEFT JOIN Songbooks b ON b.id = s.songbook_id WHERE s.id = ?"));
    q.addBindValue(songId);
    if(!q.exec() || !q.next())
        return {};
    QJsonObject o;
    o["title"] = q.value(0).toString();
    o["songbook_id"] = q.value(1).toLongLong();
    o["number"] = q.value(2).toLongLong();
    o["song_text"] = q.value(3).toString();
    o["category"] = q.value(4).toString();
    o["tune"] = q.value(5).toString();
    o["words"] = q.value(6).toString();
    o["music"] = q.value(7).toString();
    o["notes"] = q.value(8).toString();
    o["songbook"] = q.value(9).toString();
    return o;
}

bool writeFieldsTo(QSqlDatabase &db, int songId, const QJsonObject &f, QString &error)
{
    QStringList sets;
    QVariantList values;
    for(const QString &k : writeFields)
    {
        if(!f.contains(k))
            continue;
        sets << QStringLiteral("\"%1\" = ?").arg(k);
        const QJsonValue v = f.value(k);
        values << (v.isDouble() ? QVariant(v.toInteger()) : QVariant(v.toString()));
    }
    if(sets.isEmpty())
        return true;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE Songs SET %1 WHERE id = ?").arg(sets.join(QStringLiteral(", "))));
    for(const QVariant &v : std::as_const(values))
        q.addBindValue(v);
    q.addBindValue(songId);
    if(!q.exec())
    {
        error = q.lastError().text();
        return false;
    }
    return true;
}

struct Row
{
    QString uuid, kind, state, replaces;
    int songId = 0;
    QJsonObject base, proposed, library;
};

Row readRow(QSqlQuery &q)
{
    Row r;
    r.uuid = q.value(0).toString();
    r.kind = q.value(1).toString();
    r.songId = q.value(2).toInt();
    r.base = parse(q.value(3).toString());
    r.proposed = parse(q.value(4).toString());
    r.library = parse(q.value(5).toString());
    r.replaces = q.value(6).toString();
    r.state = q.value(7).toString();
    return r;
}

const char selectRow[] = "SELECT uuid, kind, song_id, base, proposed, library, replaces, state FROM CgfProposals ";

bool setState(QSqlDatabase &db, const QString &uuid, const QString &state)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE CgfProposals SET state = ?, updated_at = ? WHERE uuid = ?"));
    q.addBindValue(state);
    q.addBindValue(nowText());
    q.addBindValue(uuid);
    return q.exec();
}

int libraryVersion(QSqlDatabase &db)
{
    if(knownVersion >= 0)
        return knownVersion;
    QSqlQuery q(db);
    if(q.exec(QStringLiteral("SELECT value FROM CgfSync WHERE key = 'content_version'")) && q.next())
        return q.value(0).toInt();
    return -1;
}

QJsonObject message(const Row &r, const QString &createdAt, int version)
{
    QJsonObject m{{"type", "proposal"}, {"uuid", r.uuid}, {"kind", r.kind}, {"song_id", r.songId},
                  {"proposed", r.proposed}, {"created_at", createdAt},
                  {"user", qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"))}};
    if(r.kind == QLatin1String("edit"))
        m["base"] = r.base;
    if(!r.replaces.isEmpty())
        m["replaces"] = r.replaces;
    if(version >= 0)
        m["library_version"] = version;
    return m;
}

} // namespace

Proposals::Proposals(QObject *parent) : QObject(parent)
{
    theInstance = this;
    QSqlDatabase db = QSqlDatabase::database();
    ensureTable(db);
}

Proposals::~Proposals()
{
    if(theInstance == this)
        theInstance = nullptr;
}

Proposals *Proposals::instance()
{
    return theInstance;
}

bool Proposals::ensureTable(QSqlDatabase &db)
{
    QSqlQuery q(db);
    return q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS CgfProposals (uuid TEXT PRIMARY KEY, kind TEXT NOT NULL, "
            "song_id INTEGER NOT NULL, base TEXT, proposed TEXT NOT NULL, library TEXT, replaces TEXT, "
            "created_at TEXT NOT NULL, updated_at TEXT NOT NULL, state TEXT NOT NULL)"));
}

QJsonObject Proposals::songFields(int songId)
{
    QSqlDatabase db = QSqlDatabase::database();
    return fieldsOf(db, songId);
}

void Proposals::recordSave(int songId, const QJsonObject &before, bool isNew)
{
    QSqlDatabase db = QSqlDatabase::database();
    if(songId <= 0 || !ensureTable(db))
        return;
    const QJsonObject after = fieldsOf(db, songId);
    if(after.isEmpty())
        return;
    const bool local = songId > localSongBand;
    // A change to fonts or colours only is the laptop's own business.
    if(!isNew && !before.isEmpty() && sameText(before, after)
            && normalized(before.value("notes")) == normalized(after.value("notes")))
        return;

    // The song's latest proposal that is still in play.
    QSqlQuery q(db);
    q.prepare(QString::fromLatin1(selectRow) + QStringLiteral(
                  "WHERE song_id = ? AND state IN ('queued', 'sent', 'applied') ORDER BY created_at DESC LIMIT 1"));
    q.addBindValue(songId);
    Row last;
    const bool hasLast = q.exec() && q.next();
    if(hasLast)
        last = readRow(q);

    const QString t = nowText();
    if(hasLast && last.state == QLatin1String("queued"))
    {
        // Not sent yet: the newer text replaces it.
        q.prepare(QStringLiteral("UPDATE CgfProposals SET proposed = ?, updated_at = ? WHERE uuid = ?"));
        q.addBindValue(compact(after));
        q.addBindValue(t);
        q.addBindValue(last.uuid);
        q.exec();
        last.proposed = after;
        if(theInstance)
            emit theInstance->outgoing(message(last, t, libraryVersion(db)));
        return;
    }

    Row r;
    r.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.kind = local ? QStringLiteral("new") : QStringLiteral("edit");
    r.songId = songId;
    r.proposed = after;
    if(hasLast)
    {
        // Already sent: this one follows it, starting from its text.
        r.base = last.proposed;
        r.library = last.library;
        r.replaces = last.uuid;
        setState(db, last.uuid, QStringLiteral("superseded"));
    }
    else
    {
        r.base = before;
        r.library = before;
    }
    q.prepare(QStringLiteral("INSERT INTO CgfProposals (uuid, kind, song_id, base, proposed, library, replaces, "
                             "created_at, updated_at, state) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'queued')"));
    q.addBindValue(r.uuid);
    q.addBindValue(r.kind);
    q.addBindValue(songId);
    q.addBindValue(r.kind == QLatin1String("edit") ? compact(r.base) : QString());
    q.addBindValue(compact(r.proposed));
    q.addBindValue(r.kind == QLatin1String("edit") ? compact(r.library) : QString());
    q.addBindValue(r.replaces.isEmpty() ? QVariant() : QVariant(r.replaces));
    q.addBindValue(t);
    q.addBindValue(t);
    if(!q.exec())
    {
        qWarning("Proposals: cannot record song %d: %s", songId, qPrintable(q.lastError().text()));
        return;
    }
    qInfo("Proposals: %s song %d (\"%s\") queued for the library", qPrintable(r.kind), songId,
          qPrintable(after.value("title").toString()));
    if(theInstance)
        emit theInstance->outgoing(message(r, t, libraryVersion(db)));
}

QString Proposals::applyOverrides(QSqlDatabase &db, const QList<int> &songs, QList<int> &deleted)
{
    if(songs.isEmpty() || !ensureTable(db))
        return QString();
    QSqlQuery q(db);
    QString error;
    QSet<int> changed(songs.cbegin(), songs.cend());

    // Edits of library songs this release brings.
    q.prepare(QString::fromLatin1(selectRow) +
              QStringLiteral("WHERE kind = 'edit' AND state IN ('queued', 'sent', 'applied') ORDER BY created_at"));
    if(!q.exec())
        return q.lastError().text();
    QList<Row> edits;
    while(q.next())
    {
        Row r = readRow(q);
        if(changed.contains(r.songId))
            edits << r;
    }
    for(Row &r : edits)
    {
        const QJsonObject library = fieldsOf(db, r.songId);
        if(library.isEmpty())
        {
            setState(db, r.uuid, QStringLiteral("done")); // the library removed the song
            continue;
        }
        // The master has it (Florian's importer said so, or the text matches):
        // the library's song is the operator's song now.
        if(r.state == QLatin1String("applied") || sameText(library, r.proposed))
        {
            if(!setState(db, r.uuid, QStringLiteral("done")))
                return db.lastError().text();
            qInfo("Proposals: the library now has the change to song %d", r.songId);
            continue;
        }
        QSqlQuery u(db);
        u.prepare(QStringLiteral("UPDATE CgfProposals SET library = ? WHERE uuid = ?"));
        u.addBindValue(compact(library));
        u.addBindValue(r.uuid);
        if(!u.exec())
            return u.lastError().text();
        if(!writeFieldsTo(db, r.songId, r.proposed, error))
            return error;
        qInfo("Proposals: song %d keeps the operator's change until the library has it", r.songId);
    }

    // An accepted new song arrives as a library song in the same songbook
    // with the same title: its quick-add copy goes, with its usage count.
    q.prepare(QString::fromLatin1(selectRow) + QStringLiteral("WHERE kind = 'new' AND state = 'applied'"));
    if(!q.exec())
        return q.lastError().text();
    QList<Row> news;
    while(q.next())
        news << readRow(q);
    for(const Row &r : std::as_const(news))
    {
        const QJsonObject local = fieldsOf(db, r.songId);
        const QJsonObject want = local.isEmpty() ? r.proposed : local;
        for(int id : songs)
        {
            if(id > localSongBand)
                continue;
            const QJsonObject lib = fieldsOf(db, id);
            if(lib.value("songbook_id").toInteger() != want.value("songbook_id").toInteger()
                    || titleKey(lib.value("title")) != titleKey(want.value("title")))
                continue;
            if(!local.isEmpty())
            {
                QSqlQuery u(db);
                u.prepare(QStringLiteral("UPDATE Songs SET count = COALESCE(count, 0) + "
                                         "(SELECT COALESCE(count, 0) FROM Songs WHERE id = ?), "
                                         "date = CASE WHEN COALESCE(date, '') = '' "
                                         "THEN (SELECT date FROM Songs WHERE id = ?) ELSE date END WHERE id = ?"));
                u.addBindValue(r.songId);
                u.addBindValue(r.songId);
                u.addBindValue(id);
                if(!u.exec())
                    return u.lastError().text();
                u.prepare(QStringLiteral("DELETE FROM Songs WHERE id = ?"));
                u.addBindValue(r.songId);
                if(!u.exec())
                    return u.lastError().text();
                deleted << r.songId;
            }
            if(!setState(db, r.uuid, QStringLiteral("done")))
                return db.lastError().text();
            qInfo("Proposals: new song %d is library song %d now", r.songId, id);
            break;
        }
    }
    return QString();
}

void Proposals::setLibraryVersion(int version)
{
    knownVersion = version;
}

QList<QJsonObject> Proposals::queued() const
{
    QSqlDatabase db = QSqlDatabase::database();
    QList<QJsonObject> out;
    QSqlQuery q(db);
    if(!q.exec(QString::fromLatin1(selectRow).replace(QStringLiteral(" FROM"), QStringLiteral(", created_at FROM")) +
               QStringLiteral("WHERE state = 'queued' ORDER BY created_at")))
        return out;
    const int version = libraryVersion(db);
    while(q.next())
        out << message(readRow(q), q.value(8).toString(), version);
    return out;
}

QJsonObject Proposals::handleMessage(const QJsonObject &m)
{
    QSqlDatabase db = QSqlDatabase::database();
    const QString type = m.value("type").toString();
    const QString uuid = m.value("uuid").toString();
    if(type == QLatin1String("proposalsReady"))
    {
        for(const QJsonObject &p : queued())
            emit outgoing(p);
        return {};
    }
    QSqlQuery q(db);
    q.prepare(QString::fromLatin1(selectRow) + QStringLiteral("WHERE uuid = ?"));
    q.addBindValue(uuid);
    if(!q.exec() || !q.next())
        return {};
    const Row r = readRow(q);

    if(type == QLatin1String("proposalStored"))
    {
        const QString status = m.value("status").toString();
        if(r.state != QLatin1String("queued"))
            return {};
        if(status == QLatin1String("stored") || status == QLatin1String("duplicate"))
        {
            setState(db, uuid, QStringLiteral("sent"));
            qInfo("Proposals: song %d sent for the library", r.songId);
        }
        else if(status == QLatin1String("rejected"))
        {
            setState(db, uuid, QStringLiteral("rejected"));
            qWarning("Proposals: the server refused song %d: %s", r.songId,
                     qPrintable(m.value("reason").toString()));
        }
        // "later": stays queued for the next connection.
        return {};
    }

    if(type != QLatin1String("proposalState"))
        return {};
    const QString state = m.value("state").toString();
    const QJsonObject done{{"type", "proposalStateResult"}, {"uuid", uuid}, {"state", state}};
    // A later proposal for the song speaks for it now.
    if(r.state == QLatin1String("superseded") || r.state == QLatin1String("done") || r.state == QLatin1String("rejected"))
        return done;
    const bool edit = r.kind == QLatin1String("edit");
    if(state == QLatin1String("undone"))
    {
        if(r.state != QLatin1String("undone"))
        {
            QString error;
            if(edit && !r.library.isEmpty() && !writeFieldsTo(db, r.songId, r.library, error))
            {
                qWarning("Proposals: cannot undo song %d: %s", r.songId, qPrintable(error));
                return {};
            }
            setState(db, uuid, QStringLiteral("undone"));
            qInfo("Proposals: Florian undid the change to song %d", r.songId);
            if(edit)
                emit songsChanged({r.songId}, {});
        }
    }
    else if(state == QLatin1String("pending"))
    {
        if(r.state == QLatin1String("undone"))
        {
            // Kept after all: the operator's text again.
            QString error;
            if(edit && !writeFieldsTo(db, r.songId, r.proposed, error))
                return {};
            setState(db, uuid, QStringLiteral("sent"));
            if(edit)
                emit songsChanged({r.songId}, {});
        }
    }
    else if(state == QLatin1String("applied"))
    {
        if(r.state != QLatin1String("applied"))
        {
            if(r.state == QLatin1String("undone") && edit)
            {
                QString error;
                if(!writeFieldsTo(db, r.songId, r.proposed, error))
                    return {};
                emit songsChanged({r.songId}, {});
            }
            setState(db, uuid, QStringLiteral("applied"));
            qInfo("Proposals: the master library took the change to song %d", r.songId);
        }
    }
    else
        return {};
    return done;
}
