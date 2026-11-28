// Checks for pushed library releases (docs/song-push.md). Drives the real main
// window offscreen against a copy of a CGF snapshot. run_tests.py builds this,
// prepares the copies and the signed releases (releases.json, made with a
// throwaway key by tools/push_release.py), then runs:
//   push_tests --local      releases sent straight to the bridge, as the
//                           companion would; checks the database and the UI
//   push_tests --e2e <v>    waits until the companion, connected to a local
//                           push server, has brought the library to version v

#include <QtWidgets>
#include <QtSql>
#include <QWebSocket>
#include <QTcpSocket>
#include <functional>

#define private public
#define protected public
#include "softprojector.hpp"
#include "livebridge.hpp"
#include "bridgeserver.hpp"
#include "librarypush.hpp"
#include "songwidget.hpp"
#include "biblewidget.hpp"
#include "whatsnew.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_songwidget.h"
#include "ui_biblewidget.h"

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { ++checks; if(!(cond)) { ++failures; qWarning().noquote() << "FAIL:" << (msg); } else qInfo().noquote() << "ok  :" << (msg); } while(0)

static bool waitUntil(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t; t.start();
    while(!cond())
    {
        if(t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

struct Client
{
    QWebSocket ws;
    QList<QJsonObject> msgs;
    bool connected = false;
    Client()
    {
        QObject::connect(&ws, &QWebSocket::textMessageReceived, [this](const QString &t) {
            const QJsonObject o = QJsonDocument::fromJson(t.toUtf8()).object();
            if(o["type"].toString() != "tick") msgs.append(o);
        });
        QObject::connect(&ws, &QWebSocket::connected, [this] { connected = true; });
    }
    bool open(const QString &url) { ws.open(QUrl(url)); return waitUntil([this] { return connected; }); }
    QJsonObject waitFor(const QString &type, const std::function<bool(const QJsonObject &)> &pred = nullptr, int ms = 5000)
    {
        QJsonObject found;
        waitUntil([&] {
            for(int i = 0; i < msgs.size(); ++i)
                if(msgs[i]["type"].toString() == type && (!pred || pred(msgs[i]))) { found = msgs.takeAt(i); return true; }
            return false;
        }, ms);
        return found;
    }
    QJsonObject release(const QJsonValue &part)
    {
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(part.toObject()).toJson(QJsonDocument::Compact)));
        return waitFor("releaseResult");
    }
};

static QJsonObject readJson(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(f.readAll()).object();
}

static QVariant dbValue(const QString &sql)
{
    QSqlQuery q;
    return q.exec(sql) && q.next() ? q.value(0) : QVariant();
}

static int modelRow(SongWidget *sw, int id)
{
    for(int i = 0; i < sw->songs_model->song_list.size(); ++i)
        if(sw->songs_model->song_list.at(i).songID == id) return i;
    return -1;
}

static bool inAllSongs(SongWidget *sw, int id)
{
    for(const Song &s : sw->allSongs) if(s.songID == id) return true;
    return false;
}

static QStringList listTexts(QListWidget *list)
{
    QStringList out;
    for(int i = 0; i < list->count(); ++i) out << list->item(i)->text();
    return out;
}

// ---- What's new (whatsnew.hpp) ------------------------------------------------

static int countState(const QJsonObject &map, const QString &s)
{
    int n = 0;
    for(const QJsonValue &v : map) if(v.toObject()["s"].toString() == s) ++n;
    return n;
}

static QStringList titles(const QJsonObject &content, const QString &section)
{
    QStringList out;
    for(const QJsonValue &s : content["sections"].toArray())
        if(s.toObject()["title"].toString() == section)
            for(const QJsonValue &i : s.toObject()["items"].toArray())
                out << i.toObject()["kind"].toString() + ": " + i.toObject()["title"].toString();
    return out;
}

static QJsonArray linesOf(const QJsonObject &content, const QString &title)
{
    for(const QJsonValue &s : content["sections"].toArray())
        for(const QJsonValue &i : s.toObject()["items"].toArray())
            if(i.toObject()["title"].toString() == title) return i.toObject()["lines"].toArray();
    return QJsonArray();
}

static QJsonObject songEvents(int count, const char *state, int firstId)
{
    QJsonArray songs;
    for(int i = 0; i < count; ++i)
        songs << QJsonObject{{"id", firstId + i}, {"s", state}, {"n", i + 1}, {"t", QString("Test song %1").arg(i + 1)}};
    return QJsonObject{{"songs", songs}};
}

static void shot(const QJsonObject &content, const QString &file)
{
    WhatsNewDialog d(content);
    d.setAttribute(Qt::WA_DeleteOnClose, false);
    d.show();
    waitUntil([] { return false; }, 300);
    const bool saved = d.grab().save(file);
    d.close();
    CHECK(saved, "the window renders: " + QFileInfo(file).fileName());
}

static void checkWhatsNew(const QJsonObject &e, const QString &oldBibleName)
{
    const QString dir = QFileInfo(QSqlDatabase::database().databaseName()).absolutePath();
    QFile f(dir + "/cgf-whats-new.json");
    CHECK(f.open(QIODevice::ReadOnly), "the applied releases were recorded for What's new");
    const QJsonObject lib = QJsonDocument::fromJson(f.readAll()).object()["library"].toObject();
    f.close();
    const QJsonObject songs = lib["songs"].toObject();
    CHECK(countState(songs, "added") == 2 && countState(songs, "updated") == 2 && countState(songs, "removed") == 1,
          QString("songs recorded across v4 and v5: 2 added, 2 updated, 1 removed (%1, %2, %3)")
          .arg(countState(songs, "added")).arg(countState(songs, "updated")).arg(countState(songs, "removed")));
    CHECK(countState(lib["songbooks"].toObject(), "added") == 1, "the new songbook is recorded");
    const QJsonObject bibles = lib["bibles"].toObject();
    CHECK(bibles[e["newBible"].toString()].toObject()["s"].toString() == "updated"
          && bibles[e["oldBible"].toString()].toObject()["s"].toString() == "removed",
          "the replaced hand import counts as corrected, the old library Bible as removed");

    // The first start after updating from 0.2.6, with these library releases.
    const QJsonObject c = WhatsNew::pending(dir, "0.2.6", 6);
    const QStringList program = titles(c, "Program");
    CHECK(program == QStringList({"new: Bibles now update automatically", "new: New photos load the right way up",
                                  "new: You can now import slides without PowerPoint",
                                  "new: Imported slides now build up click by click",
                                  "changed: Program updates no longer ask you to restart",
                                  "removed: Local tools for managing Bibles and songbooks"}),
          "program changes since 0.2.6, new first: " + program.join(" | "));
    const QStringList library = titles(c, "Library");
    const QStringList expected = {"fixed: Bible text corrected: " + e["newBibleName"].toString(),
                                  "removed: Bible removed: " + oldBibleName,
                                  "new: New songbook: " + e["newBook"].toString(),
                                  "new: 2 new songs", "changed: 2 songs updated", "removed: 1 song removed"};
    CHECK(library == expected, "library changes, one line per kind: " + library.join(" | "));
    CHECK(linesOf(c, "2 new songs").size() == 2 && linesOf(c, "1 song removed").size() == 1,
          "a few songs are listed by number and title: " + linesOf(c, "2 new songs").at(0).toString());
    CHECK(c["app"].toString() == WhatsNew::appVersion() && c["library"].toInt() == 6, "the window names program and library versions");

    WhatsNew::markShown(dir, c);
    CHECK(WhatsNew::pending(dir, "0.2.6", 6).isEmpty(), "once shown, the next start shows nothing");
    CHECK(WhatsNew::lastShown(dir, 6) == c, "Help > What's new opens the same content again");

    QTemporaryDir batch;
    WhatsNew::recordLibrary(batch.path(), songEvents(7, "updated", 1));
    WhatsNew::recordLibrary(batch.path(), songEvents(3, "added", 100));
    QJsonObject b = WhatsNew::pending(batch.path(), "", 7);
    CHECK(titles(b, "Program").isEmpty(), "a new installation shows no program history");
    CHECK(titles(b, "Library").contains("changed: 7 songs updated") && linesOf(b, "7 songs updated").isEmpty(),
          "more than 5 songs: only the number");
    CHECK(linesOf(b, "3 new songs").size() == 3, "5 or fewer: listed");

    QTemporaryDir big;
    WhatsNew::recordLibrary(big.path(), songEvents(5000, "updated", 1));
    WhatsNew::recordLibrary(big.path(), songEvents(2, "updated", 1)); // the same songs again
    CHECK(titles(WhatsNew::pending(big.path(), "", 8), "Library") == QStringList({"changed: 5,000 songs updated"}),
          "a batch of 5,000 reads \"5,000 songs updated\", and songs updated twice count once");

    QTemporaryDir gone;
    WhatsNew::recordLibrary(gone.path(), songEvents(1, "added", 7));
    WhatsNew::recordLibrary(gone.path(), songEvents(1, "removed", 7));
    CHECK(WhatsNew::pending(gone.path(), "", 9).isEmpty(), "a song added and removed again before the next start is no news");

    // Screenshots: the church laptop's first start of this version after
    // Schlachter v3, and a big batch with a few new songs.
    const QString out = QFileInfo(dir).absolutePath();
    QTemporaryDir laptop;
    WhatsNew::recordLibrary(laptop.path(), QJsonObject{{"bibles", QJsonArray{
        QJsonObject{{"id", 13}, {"s", "removed"}, {"name", "German: Schlachter (2000)"}},
        QJsonObject{{"id", 14}, {"s", "updated"}, {"name", "German: Schlachter 2000 (v3)"}}}}});
    shot(WhatsNew::pending(laptop.path(), "0.2.6", 4), out + "/whats-new-laptop.png");
    QTemporaryDir week;
    WhatsNew::markShown(week.path(), QJsonObject{{"app", WhatsNew::appVersion()}}); // program changes already seen
    WhatsNew::recordLibrary(week.path(), songEvents(5000, "updated", 1));
    WhatsNew::recordLibrary(week.path(), QJsonObject{{"songs", QJsonArray{
        QJsonObject{{"id", 9001}, {"s", "added"}, {"n", 50}, {"t", QString::fromUtf8("Zünde an Dein Feuer Herr im Herzen mir")}},
        QJsonObject{{"id", 9002}, {"s", "added"}, {"n", 9}, {"t", QString::fromUtf8("Вижу Бога каждый день")}},
        QJsonObject{{"id", 9003}, {"s", "added"}, {"n", 12}, {"t", "Above all"}}}}});
    shot(WhatsNew::pending(week.path(), "", 7), out + "/whats-new-batch.png");
}

static int runLocal(SoftProjector &w, quint16 port, const QJsonObject &rel)
{
    const QJsonObject e = rel["expect"].toObject();
    const int edited0 = e["edited0"].toInt(), edited1 = e["edited1"].toInt();
    const int deleted = e["deleted"].toInt(), added = e["added"].toInt();
    SongWidget *sw = w.songWidget;

    Client c;
    CHECK(c.open(QString("ws://127.0.0.1:%1/ws").arg(port)), "a local program connects to the bridge");
    const QJsonObject hello = c.waitFor("hello");
    int helloVersion = hello["library"].toObject()["version"].toInt(-1);
    if(helloVersion < 0) helloVersion = c.waitFor("library")["version"].toInt(-1);
    CHECK(helloVersion == 3, QString("the bridge reports library v3 (got %1)").arg(helloVersion));

    // The operator's state that a release must respect: a song in the preview
    // on its second verse, another song live, the first song in the schedule.
    SongDatabase sdb;
    const Song s0 = sdb.getSongWithSongbook(edited0);
    sw->sendToPreview(s0);
    const int previewRow = qMin(1, sw->ui->listPreview->count() - 1);
    sw->ui->listPreview->setCurrentRow(previewRow);
    Song s1 = sdb.getSongWithSongbook(edited1);
    sw->sendToProjector(s1, 0);
    const QString liveBefore = w.current_song.songText;
    const QStringList showBefore = listTexts(w.ui->listShow);
    Song scheduled = s0;
    w.addToShcedule(scheduled);
    w.is_schedule_saved = true;

    QJsonObject r = c.release(rel["tampered"]);
    CHECK(r["status"].toString() == "rejected" && r["reason"].toString().contains("signature"), "a changed payload is refused (signature)");
    CHECK(w.libraryPush->version() == 3, "a refused release changes nothing");

    const QJsonArray r34 = rel["r34"].toArray();
    CHECK(r34.size() >= 2, QString("the first release came in %1 parts").arg(r34.size()));
    r = c.release(r34.last());
    CHECK(r["status"].toString() == "waiting", "the last part first: waits for the others");
    for(int i = 0; i < r34.size() - 1; ++i) r = c.release(r34[i]);
    CHECK(r["status"].toString() == "applied" && r["version"].toInt() == 4, QString("all parts in: v3 -> v4 applied (%1)").arg(r["status"].toString()));
    CHECK(c.waitFor("library", [](const QJsonObject &m) { return m["version"].toInt() == 4; }).contains("version"),
          "the bridge tells its clients the library is at v4");

    CHECK(dbValue("SELECT value FROM CgfSync WHERE key = 'content_version'").toInt() == 4, "database records v4");
    CHECK(dbValue(QString("SELECT song_text FROM Songs WHERE id = %1").arg(edited0)).toString() == e["newText0"].toString(),
          "an edited song has its new text in the database");
    CHECK(dbValue(QString("SELECT count FROM Songs WHERE id = %1").arg(edited0)).toInt() == 42, "the laptop's usage count is kept");
    CHECK(!dbValue(QString("SELECT id FROM Songs WHERE id = %1").arg(deleted)).isValid(), "a removed song is gone from the database");
    CHECK(dbValue(QString("SELECT title FROM Songs WHERE id = %1").arg(added)).toString() == e["addedTitle"].toString(),
          "a new song is in the database");
    CHECK(dbValue("SELECT title FROM Songs WHERE id = 500001").toString() == "Laptop quick-add", "the laptop's own quick-add is untouched");

    const int row0 = modelRow(sw, edited0);
    CHECK(row0 >= 0 && sw->songs_model->song_list.at(row0).title == e["newTitle0"].toString(), "the song list shows the new title");
    CHECK(modelRow(sw, added) >= 0 && inAllSongs(sw, added), "the song list and search include the new song");
    CHECK(modelRow(sw, deleted) < 0 && !inAllSongs(sw, deleted), "the song list and search drop the removed song");
    CHECK(sw->preview_song.songText == e["newText0"].toString(), "the preview shows the new text");
    CHECK(sw->ui->listPreview->currentRow() == previewRow, QString("the preview stays on verse %1").arg(previewRow + 1));
    CHECK(w.current_song.songText == liveBefore && listTexts(w.ui->listShow) == showBefore,
          "the song on the projector keeps its text");
    CHECK(w.schedule.at(0).song.songText == e["newText0"].toString(), "the schedule's copy of the song is updated");
    CHECK(w.ui->listWidgetSchedule->item(0)->text().endsWith(e["newTitle0"].toString()), "the schedule shows the new title");
    CHECK(w.is_schedule_saved, "the update does not mark the schedule unsaved");

    r = c.release(r34.first());
    CHECK(r["status"].toString() == "duplicate", "a part that arrives again is a duplicate");

    for(const QJsonValue &p : rel["r45"].toArray()) r = c.release(p);
    CHECK(r["status"].toString() == "applied" && w.libraryPush->version() == 5, "v4 -> v5 (a new songbook) applied");
    CHECK(sw->ui->songbook_menu->findText(e["newBook"].toString()) > 0, "the songbook menu has the new songbook");
    CHECK(modelRow(sw, e["addedInBook"].toInt()) >= 0, "the song in the new songbook is listed");

    r = c.release(rel["bad56"].toArray().first());
    CHECK(r["status"].toString() == "rejected" && w.libraryPush->version() == 5, "a release touching a laptop's own song id is refused");
    CHECK(!dbValue("SELECT id FROM Songs WHERE id = 500002").isValid(), "and leaves the database as it was");

    r = c.release(rel["gap78"].toArray().first());
    CHECK(r["status"].toString() == "gap" && r["version"].toInt() == 5, "a release that skips versions is reported as a gap");

    // ---- Bibles (docs/library-from-cloud.md) --------------------------------
    const QString oldBible = e["oldBible"].toString(), newBible = e["newBible"].toString();
    const QString oldBibleName = dbValue(QString("SELECT bible_name FROM BibleVersions WHERE id = %1").arg(oldBible)).toString().trimmed();
    CHECK(hello["library"].toObject()["features"].toArray().contains(QJsonValue("bibles")),
          "the bridge announces that this app takes Bible releases");
    CHECK(w.mySettings.bibleSets.secondaryBible == newBible && w.mySettings.bibleSets2.primaryBible == oldBible,
          "before: a hand-imported Bible is second on the screen, display 2 shows the old library Bible");

    BibleWidget *bw = w.bibleWidget;
    bw->ui->chapter_preview_list->setCurrentRow(0);
    bw->ui->chapter_preview_list->item(0)->setSelected(true);
    bw->sendToProjector(false);
    waitUntil([&] { return w.bibleOnScreen(); }, 2000);
    CHECK(w.bibleOnScreen(), "a Bible passage is on the projector");
    const QStringList bibleShow = listTexts(w.ui->listShow);

    const QJsonArray r56 = rel["r56"].toArray();
    CHECK(r56.size() >= 3, QString("the Bible release came in %1 parts").arg(r56.size()));
    for(int i = 0; i < r56.size(); ++i) r = c.release(r56[i]);
    CHECK(r["status"].toString() == "held" && w.libraryPush->version() == 5,
          QString("while the passage is on the projector the release waits (%1)").arg(r["status"].toString()));
    CHECK(dbValue(QString("SELECT bible_name FROM BibleVersions WHERE id = %1").arg(newBible)).toString() == "Hand import"
          && dbValue(QString("SELECT id FROM BibleVersions WHERE id = %1").arg(oldBible)).isValid(),
          "and the Bibles are untouched meanwhile");
    CHECK(listTexts(w.ui->listShow) == bibleShow, "the passage on the projector stays as it was");
    r = c.release(r56.first());
    CHECK(r["status"].toString() == "held", "a part sent again meanwhile is told to wait too");
    r = c.release(rel["future67"]);
    CHECK(r["status"].toString() == "rejected" && r["reason"].toString().contains("newer"),
          "a release needing something this app does not know is refused");

    QElapsedTimer took;
    took.start();
    Song s2 = sdb.getSongWithSongbook(edited1);
    sw->sendToProjector(s2, 0);
    const QJsonObject done = c.waitFor("releaseResult", nullptr, 15000);
    qInfo().noquote() << QString("Bible release applied %1 ms after the passage left the projector").arg(took.elapsed());
    CHECK(done["status"].toString() == "applied" && done["version"].toInt() == 6,
          QString("once a song replaces the passage, the held release applies (%1, v%2)")
          .arg(done["status"].toString()).arg(done["version"].toInt()));

    QStringList ids;
    QSqlQuery q("SELECT id FROM BibleVersions ORDER BY id");
    while(q.next()) ids << q.value(0).toString();
    QStringList library;
    for(const QJsonValue &v : e["library"].toArray()) library << v.toString();
    CHECK(ids == library, QString("the laptop has exactly the library's Bibles (%1)").arg(ids.join(',')));
    CHECK(dbValue(QString("SELECT bible_name FROM BibleVersions WHERE id = %1").arg(newBible)).toString()
          == e["newBibleName"].toString(), "the hand import is replaced by the library's Bible, with its version in the name");
    CHECK(dbValue(QString("SELECT COUNT(*) FROM BibleVerse WHERE CAST(bible_id AS INTEGER) = %1").arg(newBible)).toInt()
          == e["newBibleVerses"].toInt(), "every verse of the new Bible arrived");
    CHECK(dbValue(QString("SELECT verse_text FROM BibleVerse WHERE CAST(bible_id AS INTEGER) = %1 AND verse_id = 'B019C023V001'")
                  .arg(newBible)).toString() == "Push test psalm text", "its text is the library's");
    CHECK(!dbValue(QString("SELECT 1 FROM BibleVerse WHERE CAST(bible_id AS INTEGER) = %1").arg(oldBible)).isValid()
          && !dbValue(QString("SELECT 1 FROM BibleBooks WHERE bible_id = %1").arg(oldBible)).isValid(),
          "the old library Bible is gone, verses and books too");
    CHECK(dbValue(QString("SELECT new_id FROM CgfBibleReplaces WHERE old_id = '%1'").arg(oldBible)).toString() == newBible,
          "the replacement is recorded for later snapshot installs");
    CHECK(w.mySettings.bibleSets.secondaryBible == newBible, "the screen keeps its second Bible");
    CHECK(w.mySettings.bibleSets2.primaryBible == newBible, "display 2 moves from the removed Bible to its replacement");
    CHECK(dbValue("SELECT sets FROM Settings WHERE type = 'bible2'").toString().contains("primary = " + newBible),
          "and the settings are saved");
    CHECK(w.theme.bible2.versions.primaryBible == newBible, "the theme draws display 2 with the new Bible");
    CHECK(w.libraryPush->version() == 6 && !w.libraryPush->isHolding(), "the library is at v6, nothing held");

    Client c2;
    c2.open(QString("ws://127.0.0.1:%1/ws").arg(port));
    const QJsonObject hello2 = c2.waitFor("hello");
    QStringList names, helloIds;
    for(const QJsonValue &b : hello2["bibles"].toArray())
    {
        names << b.toObject()["name"].toString();
        helloIds << b.toObject()["id"].toString();
    }
    CHECK(names.contains(e["newBibleName"].toString()) && !helloIds.contains(oldBible),
          "the bridge lists the new Bible by name and no longer the old one");

    checkWhatsNew(e, oldBibleName);

    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}

static int runE2E(SoftProjector &w, int expected, const QJsonObject &rel)
{
    const QJsonObject e = rel["expect"].toObject();
    if(expected >= 6)
    {
        // A passage is on the projector while the Bible release (v5 -> v6) arrives.
        BibleWidget *bw = w.bibleWidget;
        bw->ui->chapter_preview_list->setCurrentRow(0);
        bw->ui->chapter_preview_list->item(0)->setSelected(true);
        bw->sendToProjector(false);
        waitUntil([&] { return w.bibleOnScreen(); }, 2000);
    }
    qInfo("READY");
    if(expected >= 6)
    {
        const bool held = waitUntil([&] { return w.libraryPush->version() == 5 && w.libraryPush->isHolding(); }, 120000);
        CHECK(held, QString("the Bible release waits while the passage is on the projector (at v%1)")
              .arg(w.libraryPush->version()));
        waitUntil([] { return false; }, 1500);
        CHECK(w.libraryPush->version() == 5, "and stays held");
        SongDatabase sdb;
        Song s = sdb.getSongWithSongbook(e["edited1"].toInt());
        w.songWidget->sendToProjector(s, 0);
    }
    const bool reached = waitUntil([&] { return w.libraryPush->version() == expected; }, 90000);
    CHECK(reached, QString("the pushed release reached the app (v%1)").arg(w.libraryPush->version()));
    CHECK(dbValue(QString("SELECT song_text FROM Songs WHERE id = %1").arg(e["edited0"].toInt())).toString() == e["newText0"].toString(),
          "the edited song arrived through server and companion");
    CHECK(modelRow(w.songWidget, e["added"].toInt()) >= 0, "the new song is in the song list");
    if(expected >= 6)
        CHECK(dbValue(QString("SELECT bible_name FROM BibleVersions WHERE id = %1").arg(e["newBible"].toString())).toString()
              == e["newBibleName"].toString()
              && !dbValue(QString("SELECT id FROM BibleVersions WHERE id = %1").arg(e["oldBible"].toString())).isValid(),
              "the new Bible arrived through server and companion, the old one is gone");
    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}

int main(int argc, char *argv[])
{
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QString("--local");
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if(qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts"); // offscreen draws no text without it
    qputenv("QT_QUICK_BACKEND", "software");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8778");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    QApplication a(argc, argv);
    a.setApplicationName("CGFPushHarness"); // its own settings, never the real app's
    a.setFont(QFont("Segoe UI", 9)); // what Qt 6 uses on Windows; offscreen would pick another
    const QString dir = QCoreApplication::applicationDirPath();

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dir + "/spData.sqlite");
    if(!db.open()) { qCritical() << "no database"; return 2; }

    SoftProjector w;
    w.setAppDataDir(QDir(dir));
    const quint16 port = quint16(qEnvironmentVariableIntValue("CGF_BRIDGE_PORT"));
    QTcpSocket probe;
    const bool up = waitUntil([&] { probe.abort(); probe.connectToHost(QHostAddress::LocalHost, port); return probe.waitForConnected(100); }, 5000);
    probe.abort();
    if(!up) { qCritical() << "bridge did not start on" << port; return 2; }
    waitUntil([] { return false; }, 300); // let the main window report the library version

    const QJsonObject rel = readJson(dir + "/releases.json");
    if(mode == "--e2e")
        return runE2E(w, argc > 2 ? QString::fromLatin1(argv[2]).toInt() : 4, rel);
    return runLocal(w, port, rel);
}
