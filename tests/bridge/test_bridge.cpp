// Checks for the network bridge (docs/network-bridge.md). Drives the real
// main window offscreen against a copy of the CGF church database (4 Bibles:
// ids 2, 3, 5, 13; song 214) and checks what the bridge sends over HTTP and
// WebSocket. Build and run with run_tests.py, which copies the database first.
//   bridge_tests            run the checks; exit code 0 when all pass
//   bridge_tests --serve    keep serving on CGF_BRIDGE_PORT for a browser;
//                           commands from cmd.txt beside the exe (see serve())

#include <QtWidgets>
#include <QtSql>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QStringListModel>
#include <functional>

#define private public
#define protected public
#include "softprojector.hpp"
#include "livebridge.hpp"
#include "bridgeserver.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"

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

static void pump(int ms) { waitUntil([] { return false; }, ms); }

struct Client
{
    QWebSocket ws;
    QList<QJsonObject> msgs;
    bool connected = false;
    explicit Client(const QString &origin = QString()) : ws(origin)
    {
        QObject::connect(&ws, &QWebSocket::textMessageReceived, [this](const QString &t) {
            const QJsonObject o = QJsonDocument::fromJson(t.toUtf8()).object();
            if(o["type"].toString() != "tick") msgs.append(o);
        });
        QObject::connect(&ws, &QWebSocket::connected, [this] { connected = true; });
    }
    bool open(const QString &url, int ms = 3000)
    {
        ws.open(QUrl(url));
        return waitUntil([this] { return connected; }, ms);
    }
    QJsonObject waitFor(const QString &type, const std::function<bool(const QJsonObject &)> &pred = nullptr, int ms = 3000)
    {
        QJsonObject found;
        waitUntil([&] {
            for(int i = 0; i < msgs.size(); ++i)
                if(msgs[i]["type"].toString() == type && (!pred || pred(msgs[i]))) { found = msgs.takeAt(i); return true; }
            return false;
        }, ms);
        return found;
    }
    int count(const QString &type) const
    {
        int n = 0;
        for(const QJsonObject &o : msgs) n += o["type"].toString() == type;
        return n;
    }
    void send(const QJsonObject &o) { ws.sendTextMessage(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))); }
};

struct Http { int status = 0; QByteArray body; QNetworkReply *raw = nullptr; QByteArray header(const char *h) const { return headers.value(QByteArray(h).toLower()); } QHash<QByteArray, QByteArray> headers; };

static Http request(QNetworkAccessManager &nam, const QString &url, const QByteArray &range = {}, const QByteArray &verb = "GET")
{
    QNetworkRequest req{QUrl(url)};
    if(!range.isEmpty()) req.setRawHeader("Range", range);
    QNetworkReply *r = nam.sendCustomRequest(req, verb);
    waitUntil([r] { return r->isFinished(); }, 5000);
    Http h;
    h.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    h.body = r->readAll();
    for(const auto &p : r->rawHeaderPairs()) h.headers.insert(p.first.toLower(), p.second);
    r->deleteLater();
    return h;
}

static QByteArray rawRequest(quint16 port, const QByteArray &req)
{
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    if(!s.waitForConnected(2000)) return {};
    s.write(req);
    QByteArray all;
    waitUntil([&] { all += s.readAll(); return s.state() == QAbstractSocket::UnconnectedState; }, 3000);
    all += s.readAll();
    return all;
}

static QJsonObject version(const QJsonObject &live, const QString &id)
{
    for(const QJsonValue &v : live["bible"].toObject()["versions"].toArray())
        if(v.toObject()["id"].toString() == id) return v.toObject();
    return {};
}

static void showVerses(SoftProjector &w, const QStringList &ids, int first, int last)
{
    w.bibleWidget->bible.currentIdList = ids;
    static QStringListModel model;
    QStringList rows;
    for(const QString &id : ids) rows << id;
    model.setStringList(rows);
    QItemSelection sel(model.index(first), model.index(last));
    w.setChapterList(rows, "test", sel);
}

static int runChecks(SoftProjector &w, quint16 port)
{
    const QString base = QString("http://127.0.0.1:%1").arg(port);
    const QString wsUrl = QString("ws://127.0.0.1:%1/ws").arg(port);
    QNetworkAccessManager nam;

    // --- a mirror connecting right after start-up gets the passive screen
    {
        Client early;
        CHECK(early.open(wsUrl), "early mirror connects");
        early.send(QJsonObject{{"type", "subscribe"}, {"frames", true}});
        const QJsonObject f = early.waitFor("frame");
        CHECK(!f.isEmpty() && !f["back"].toString().isEmpty(), "start-up frame (passive screen) reaches a new mirror");
        early.ws.close();
        CHECK(waitUntil([&] { return !w.liveBridge->framesWanted; }, 3000), "frames off again after it leaves");
    }

    // --- connect
    Client c1;
    CHECK(c1.open(wsUrl), "WebSocket connects without Origin");
    const QJsonObject hello = c1.waitFor("hello");
    CHECK(hello["bibles"].toArray().size() == 4, QString("hello lists 4 Bibles (%1)").arg(hello["bibles"].toArray().size()));
    const QJsonObject sample = hello["sample"].toObject();
    CHECK(sample["versions"].toArray().size() == 4, "sample has John 3:16 in 4 Bibles");
    QJsonObject nkjvSample, rstSample;
    for(const QJsonValue &v : sample["versions"].toArray()) {
        if(v.toObject()["id"] == "2") nkjvSample = v.toObject();
        if(v.toObject()["id"] == "3") rstSample = v.toObject();
    }
    CHECK(nkjvSample["reference"].toString() == "John 3:16", "NKJV reference: " + nkjvSample["reference"].toString());
    CHECK(rstSample["reference"].toString() == QString::fromUtf8("От Иоанна 3:16"), "RST reference shows only the Russian book name: " + rstSample["reference"].toString());
    CHECK(!c1.waitFor("style").isEmpty(), "style sent on connect");
    QJsonObject live = c1.waitFor("live");
    CHECK(live["kind"].toString() == "none" && !live["visible"].toBool(), "initial live: nothing visible");
    CHECK(live["bibles"].toArray() == QJsonArray({"2", "5", "3"}), "projector Bible order from settings (2,5,3)");

    // --- song
    Song song(214);
    song.readData();
    const int stanzas = song.getSongTextList().count();
    w.setSongList(song, 0);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "song"; });
    const QJsonObject s0 = live["song"].toObject();
    CHECK(live["visible"].toBool(), "song is visible");
    CHECK(s0["text"].toString() == song.getStanza(0).stanza, "song stanza 1 text matches the projector");
    CHECK(s0["index"].toInt() == 0 && s0["count"].toInt() == stanzas, QString("song index 0 of %1").arg(stanzas));
    CHECK(s0["number"].toInt() == song.number, "song number");
    w.ui->listShow->setCurrentRow(1);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["song"].toObject()["index"].toInt() == 1; });
    CHECK(live["song"].toObject()["text"].toString() == song.getStanza(1).stanza, "next stanza published");
    pump(200);
    const int before = c1.count("live");
    w.updateScreen(); w.updateScreen(); w.updateScreen();
    pump(300);
    CHECK(c1.count("live") == before, "repeated updateScreen() without change sends nothing");

    // --- clear, hide, show
    w.on_actionClear_triggered();
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["cleared"].toBool(); });
    CHECK(!live.isEmpty() && live["visible"].toBool(), "Clear: cleared, still visible");
    w.on_actionHide_triggered();
    live = c1.waitFor("live", [](const QJsonObject &o) { return !o["visible"].toBool(); });
    CHECK(!live.isEmpty() && !live["cleared"].toBool(), "Hide: not visible");
    w.on_actionShow_triggered();
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["visible"].toBool(); });
    CHECK(!live.isEmpty() && live["kind"] == "song", "Show: visible again");

    // --- Bible
    const QStringList john3 = {"B043C003V016", "B043C003V017", "B043C003V018"};
    showVerses(w, john3, 0, 0);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "bible"; });
    const QJsonObject bible = live["bible"].toObject();
    CHECK(bible["verseIds"].toArray() == QJsonArray({"B043C003V016"}), "Bible verse id published");
    CHECK(bible["versions"].toArray().size() == 4, "verse looked up in all 4 Bibles");
    CHECK(bible["projected"].toArray() == QJsonArray({"2", "5", "3"}), "projected Bibles in screen order");
    CHECK(version(live, "2")["text"].toString().contains("For God so loved"), "NKJV text");
    showVerses(w, john3, 0, 1);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["bible"].toObject()["verseIds"].toArray().size() == 2; });
    CHECK(version(live, "2")["reference"].toString() == "John 3:16-17", "two verses: " + version(live, "2")["reference"].toString());
    CHECK(version(live, "2")["verses"].toArray().size() == 2, "two verse entries");
    showVerses(w, {"B019C023V001"}, 0, 0);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["bible"].toObject()["verseIds"].toArray() == QJsonArray({"B019C023V001"}); });
    const QString rstRef = version(live, "3")["reference"].toString();
    CHECK(rstRef.endsWith(" 22:1") && !rstRef.contains("Psalm"), "RST keeps its own numbering: " + rstRef);
    CHECK(version(live, "2")["reference"].toString().endsWith(" 23:1"), "NKJV Psalm 23:1: " + version(live, "2")["reference"].toString());

    // --- announcement
    Announcement an;
    an.title = "Harness";
    an.text = "Line one\nLine two";
    w.setAnnounceText(an, 0);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "announcement"; });
    CHECK(live["announcement"].toObject()["text"].toString().contains("Line one"), "announcement text");

    // --- picture
    QList<SlideShowItem> slides;
    for(int i = 0; i < 2; ++i) {
        SlideShowItem it;
        it.name = QString("Slide %1").arg(i + 1);
        QPixmap pm(1280, 720);
        pm.fill(i ? Qt::darkRed : Qt::darkBlue);
        it.image = pm;
        it.imageSmall = pm.scaled(100, 56);
        slides << it;
    }
    w.setPictureList(slides, 0, "Harness slides");
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "picture"; });
    const QString picUrl = live["picture"].toObject()["url"].toString();
    CHECK(picUrl.startsWith("/media/picture/"), "picture address: " + picUrl);
    Http pic = request(nam, base + picUrl);
    CHECK(pic.status == 200 && pic.header("content-type") == "image/jpeg" && pic.body.startsWith("\xFF\xD8"), QString("picture served as JPEG (%1 bytes)").arg(pic.body.size()));
    QImage decoded = QImage::fromData(pic.body);
    CHECK(decoded.size() == QSize(1280, 720), "picture decodes at 1280x720");
    const int picSeq = picUrl.section('/', -1).toInt();
    CHECK(request(nam, base + QString("/media/picture/%1").arg(picSeq - 1)).status == 404, "old picture address: 404");
    w.on_actionHide_triggered();
    w.on_actionShow_triggered();
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "picture" && o["visible"].toBool(); });
    CHECK(live["picture"].toObject()["url"].toString() == picUrl, "same picture keeps its address after Hide/Show");
    w.ui->listShow->setCurrentRow(1);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["picture"].toObject()["index"].toInt() == 1; });
    CHECK(live["picture"].toObject()["url"].toString() != picUrl, "next picture gets a new address");

    // --- video file serving (a fake file; playback is tested separately)
    const QString vpath = QCoreApplication::applicationDirPath() + "/harness-fake.mp4";
    {
        QFile f(vpath);
        f.open(QIODevice::WriteOnly);
        QByteArray data(1000000, 0);
        for(int i = 0; i < data.size(); ++i) data[i] = char(i % 251);
        f.write(data);
    }
    VideoInfo vi;
    vi.filePath = QUrl::fromLocalFile(vpath);
    vi.fileName = "harness-fake.mp4";
    w.mediaControls->setVolume(0);
    w.setVideo(vi);
    live = c1.waitFor("live", [](const QJsonObject &o) { return o["kind"] == "video"; });
    const QJsonObject vid = live["video"].toObject();
    const QString vurl = vid["url"].toString();
    CHECK(vurl.startsWith("/media/video/") && vid["mime"] == "video/mp4" && !vid["audioOnly"].toBool(), "video address and type: " + vurl);
    CHECK(!vid.contains("path"), "no local file path is exposed");
    Http r1 = request(nam, base + vurl, "bytes=0-99");
    bool pattern = r1.body.size() == 100;
    for(int i = 0; pattern && i < 100; ++i) pattern = r1.body[i] == char(i % 251);
    CHECK(r1.status == 206 && r1.header("content-range") == "bytes 0-99/1000000" && pattern, "range 0-99: 206 with the right bytes");
    Http r2 = request(nam, base + vurl, "bytes=999990-");
    CHECK(r2.status == 206 && r2.body.size() == 10 && r2.body[0] == char(999990 % 251), "open-ended range: last 10 bytes");
    Http r3 = request(nam, base + vurl, "bytes=-5");
    CHECK(r3.status == 206 && r3.body.size() == 5 && r3.header("content-range") == "bytes 999995-999999/1000000", "suffix range: last 5 bytes");
    CHECK(request(nam, base + vurl, "bytes=2000000-").status == 416, "range past the end: 416");
    Http r5 = request(nam, base + vurl);
    CHECK(r5.status == 200 && r5.body.size() == 1000000 && r5.header("accept-ranges") == "bytes", "whole file: 200, 1,000,000 bytes");
    Http r6 = request(nam, base + vurl, {}, "HEAD");
    CHECK(r6.status == 200 && r6.body.isEmpty() && r6.header("content-length") == "1000000", "HEAD: length, no body");
    const int vseq = vurl.section('/', -1).toInt();
    CHECK(request(nam, base + QString("/media/video/%1").arg(vseq + 1)).status == 404, "other video address: 404");
    const QJsonObject vstate = c1.waitFor("video", [vseq](const QJsonObject &o) { return o["seq"].toInt() == vseq; });
    CHECK(!vstate.isEmpty(), "video state follows the new video: " + vstate["state"].toString());

    // --- pages and errors
    Http index = request(nam, base + "/");
    CHECK(index.status == 200 && index.header("content-type").startsWith("text/html") && index.body.contains("/stream/settings"), "index page");
    CHECK(request(nam, base + "/stream").body.contains("/assets/stream.js"), "stream page");
    CHECK(request(nam, base + "/stream/settings").body.contains("/assets/settings.js"), "settings page");
    Http js = request(nam, base + "/assets/bridge.js");
    CHECK(js.status == 200 && js.header("content-type").startsWith("text/javascript"), "module served as JavaScript");
    CHECK(request(nam, base + "/nope").status == 404, "unknown address: 404");
    CHECK(rawRequest(port, "GET /assets/%2e%2e/stream.html HTTP/1.1\r\nHost: x\r\n\r\n").startsWith("HTTP/1.1 404"), "encoded ../ refused");
    CHECK(rawRequest(port, "GET /assets/..%5C..%5Cweb.qrc HTTP/1.1\r\nHost: x\r\n\r\n").startsWith("HTTP/1.1 404"), "encoded ..\\ refused");
    CHECK(rawRequest(port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n").startsWith("HTTP/1.1 405"), "POST: 405");
    Http state = request(nam, base + "/api/state");
    CHECK(state.status == 200 && QJsonDocument::fromJson(state.body).object()["live"].toObject()["kind"] == "video", "/api/state shows the live video");

    // --- origin check
    Client evil("http://evil.example");
    CHECK(!evil.open(wsUrl, 1500), "WebSocket from another site's page is refused");
    Client page(QString("http://127.0.0.1:%1").arg(port));
    CHECK(page.open(wsUrl), "WebSocket from a bridge page is accepted");

    // --- style and hide
    const QString settingsPath = QCoreApplication::applicationDirPath() + "/cgf-network.json";
    page.send(QJsonObject{{"type", "setStyle"}, {"style", QJsonObject{{"size", 60}, {"bibles", QJsonArray({"3"})}}}});
    const QJsonObject st = c1.waitFor("style", [](const QJsonObject &o) { return o["style"].toObject()["size"].toInt() == 60; });
    CHECK(!st.isEmpty(), "style change reaches the other client");
    CHECK(waitUntil([&] { QFile f(settingsPath); return f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).object()["stream"].toObject()["size"].toInt() == 60; }, 3000), "style saved to cgf-network.json");
    page.send(QJsonObject{{"type", "setHidden"}, {"hidden", true}});
    CHECK(!c1.waitFor("style", [](const QJsonObject &o) { return o["hidden"].toBool(); }).isEmpty(), "Hide from stream reaches every client");
    page.send(QJsonObject{{"type", "setHidden"}, {"hidden", false}});
    page.send(QJsonObject{{"type", "bogus"}});
    page.ws.sendTextMessage("not json");
    pump(300);
    CHECK(page.ws.state() == QAbstractSocket::ConnectedState, "junk messages are ignored");

    // --- back-screen mirror frames
    Client mirror;
    CHECK(mirror.open(wsUrl), "mirror connects");
    mirror.waitFor("hello");
    pump(200);
    CHECK(!w.liveBridge->framesWanted, "no frames converted while no mirror is connected");
    mirror.send(QJsonObject{{"type", "subscribe"}, {"frames", true}});
    CHECK(waitUntil([&] { return w.liveBridge->framesWanted; }, 2000), "a mirror subscribing turns frames on");
    const QJsonObject f0 = mirror.waitFor("frame");
    CHECK(f0["text"].toString().startsWith("/frame/text/") && f0["back"].toString().startsWith("/frame/back/"), "current frame sent on subscribe");
    const QSize screen(f0["width"].toInt(), f0["height"].toInt());
    Http ft = request(nam, base + f0["text"].toString());
    const QImage textImg = QImage::fromData(ft.body);
    CHECK(ft.status == 200 && ft.header("content-type") == "image/png" && textImg.hasAlphaChannel() && textImg.size() == screen,
          QString("text layer: transparent PNG %1x%2, %3 KB").arg(textImg.width()).arg(textImg.height()).arg(ft.body.size() / 1024));
    Http fb = request(nam, base + f0["back"].toString());
    CHECK(fb.status == 200 && fb.header("content-type") == "image/jpeg" && QImage::fromData(fb.body).size() == screen,
          QString("background: JPEG at screen size, %1 KB").arg(fb.body.size() / 1024));
    QElapsedTimer t;
    t.start();
    w.setSongList(song, 2);
    const QJsonObject f1 = mirror.waitFor("frame", [&](const QJsonObject &o) { return o["text"] != f0["text"]; });
    CHECK(!f1.isEmpty(), QString("new frame %1 ms after a song change").arg(t.elapsed()));
    w.ui->listShow->setCurrentRow(3);
    const QJsonObject f2 = mirror.waitFor("frame", [&](const QJsonObject &o) { return o["text"] != f1["text"]; });
    CHECK(!f2.isEmpty() && f2["back"] == f1["back"], "next stanza: new text, same background address");
    CHECK(f2["fade"].toBool() == w.theme.song.useFading, "fade follows the song theme");
    CHECK(request(nam, base + f1["text"].toString()).status == 404, "old text layer address: 404");
    CHECK(c1.count("frame") == 0, "clients that did not subscribe get no frames");
    CHECK(request(nam, base + "/screen").body.contains("/assets/screen.js"), "back-screen page");
    mirror.ws.close();
    CHECK(waitUntil([&] { return !w.liveBridge->framesWanted; }, 3000), "the last mirror leaving turns frames off");

    qInfo().noquote() << QString("\n%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}

// --serve: keep the bridge up and follow commands written to cmd.txt, one per
// line: song <id> <row> | bible <verseId>[,<verseId>...] | announce <text> |
// picture <path> | video <path> | play | pause | stop | clear | hide | show
static void serve(SoftProjector &w)
{
    const QString cmdPath = QCoreApplication::applicationDirPath() + "/cmd.txt";
    auto *timer = new QTimer(&w);
    QObject::connect(timer, &QTimer::timeout, [&w, cmdPath] {
        QFile f(cmdPath);
        if(!f.exists() || !f.open(QIODevice::ReadOnly)) return;
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        f.close();
        f.remove();
        for(QString line : lines) {
            line = line.trimmed();
            const QString cmd = line.section(' ', 0, 0);
            const QString arg = line.section(' ', 1);
            qInfo().noquote() << "cmd:" << line;
            if(cmd == "song") { Song s(arg.section(' ', 0, 0).toInt()); s.readData(); w.setSongList(s, arg.section(' ', 1, 1).toInt()); }
            else if(cmd == "row") w.ui->listShow->setCurrentRow(arg.toInt());
            else if(cmd == "bible") { const QStringList ids = arg.split(','); showVerses(w, ids, 0, ids.size() - 1); }
            else if(cmd == "announce") { Announcement a; a.title = "Announcement"; a.text = QString(arg).replace("\\n", "\n"); w.setAnnounceText(a, 0); }
            else if(cmd == "picture") {
                static QList<SlideShowItem> slides;
                slides.clear();
                SlideShowItem it; it.name = QFileInfo(arg).fileName(); it.image = QPixmap(arg); it.imageSmall = it.image.scaled(100, 100, Qt::KeepAspectRatio);
                slides << it;
                w.setPictureList(slides, 0, "Pictures");
            }
            else if(cmd == "video") { VideoInfo v; v.filePath = QUrl::fromLocalFile(arg); v.fileName = QFileInfo(arg).fileName(); w.mediaControls->setVolume(0); w.setVideo(v); }
            else if(cmd == "play") w.playVideo();
            else if(cmd == "pause") w.pauseVideo();
            else if(cmd == "stop") w.stopVideo();
            else if(cmd == "seek") w.setVideoPosition(arg.toLongLong());
            else if(cmd == "clear") w.on_actionClear_triggered();
            else if(cmd == "hide") w.on_actionHide_triggered();
            else if(cmd == "show") w.on_actionShow_triggered();
            else if(cmd == "quit") qApp->quit();
        }
    });
    timer->start(150);
}

int main(int argc, char *argv[])
{
    const bool serveMode = argc > 1 && QByteArray(argv[1]) == "--serve";
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8779");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    QApplication a(argc, argv);
    a.setApplicationName("CGFBridgeHarness");
    const QString dir = QCoreApplication::applicationDirPath();
    if(!serveMode) QFile::remove(dir + "/cgf-network.json");

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

    if(serveMode) {
        serve(w);
        return a.exec();
    }
    return runChecks(w, port);
}
