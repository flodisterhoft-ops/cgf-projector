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
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_songwidget.h"

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

    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}

static int runE2E(SoftProjector &w, int expected, const QJsonObject &rel)
{
    const QJsonObject e = rel["expect"].toObject();
    qInfo("READY");
    const bool reached = waitUntil([&] { return w.libraryPush->version() == expected; }, 90000);
    CHECK(reached, QString("the pushed release reached the app (v%1)").arg(w.libraryPush->version()));
    CHECK(dbValue(QString("SELECT song_text FROM Songs WHERE id = %1").arg(e["edited0"].toInt())).toString() == e["newText0"].toString(),
          "the edited song arrived through server and companion");
    CHECK(modelRow(w.songWidget, e["added"].toInt()) >= 0, "the new song is in the song list");
    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}

int main(int argc, char *argv[])
{
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QString("--local");
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8778");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    QApplication a(argc, argv);
    a.setApplicationName("CGFPushHarness"); // its own settings, never the real app's
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
