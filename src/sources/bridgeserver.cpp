/***************************************************************************
//
//    CGF Projector - network bridge
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

#include "../headers/bridgeserver.hpp"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>
#include <algorithm>
#include <memory>

namespace {

const char settingsFileName[] = "cgf-network.json";
const int maxHeaderBytes = 16 * 1024;
const int maxMessageBytes = 64 * 1024;
// Programs on this computer may send library releases, which can be large.
const int maxLocalMessageBytes = 16 * 1024 * 1024;
const int tickMs = 10000;       // heartbeat to clients; also pings them
const qint64 clientTimeoutMs = 45000;

QByteArray statusText(int status)
{
    switch(status)
    {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    default:  return "Error";
    }
}

QByteArray mimeType(const QString &name)
{
    static const QHash<QString, QByteArray> types = {
        {"html", "text/html; charset=utf-8"}, {"js", "text/javascript; charset=utf-8"},
        {"css", "text/css; charset=utf-8"}, {"json", "application/json"},
        {"svg", "image/svg+xml"}, {"png", "image/png"}, {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"}, {"ico", "image/x-icon"}, {"woff2", "font/woff2"},
        {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mov", "video/quicktime"},
        {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"ogv", "video/ogg"},
        {"avi", "video/x-msvideo"}, {"wmv", "video/x-ms-wmv"}, {"mpg", "video/mpeg"},
        {"mpeg", "video/mpeg"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"},
        {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"flac", "audio/flac"},
        {"wma", "audio/x-ms-wma"}, {"aac", "audio/aac"}};
    return types.value(QFileInfo(name).suffix().toLower(), "application/octet-stream");
}

// RST stores book names in three languages ("Бытие-Genesis-1.Mose",
// "1-я Царств-1.Samuel") so the operator can search in any of them.
// References show only the first.
QString displayBookName(const QString &name)
{
    static const QRegularExpression extra(QStringLiteral("-(?=[A-Za-z]|\\d\\.)"));
    const QRegularExpressionMatch m = extra.match(name);
    return m.hasMatch() ? name.left(m.capturedStart()) : name;
}

QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QByteArray encodeImage(const QImage &image, const char *format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    if(qstrcmp(format, "jpg") == 0)
        writer.setQuality(90);
    if(!writer.write(image))
        qWarning("Network bridge cannot encode an image: %s", qPrintable(writer.errorString()));
    return bytes;
}

} // namespace

BridgeServer::BridgeServer(const QString &dataDir, const QString &dbPath)
    : dataDir(dataDir),
      dbPath(dbPath),
      dbConnection(QStringLiteral("bridge-%1").arg(quintptr(this))),
      webDir(qEnvironmentVariable("CGF_WEB_DIR")),
      port(defaultPort),
      enabled(true),
      tcp(nullptr),
      ws(nullptr),
      saveTimer(nullptr),
      tickTimer(nullptr),
      seq(0),
      videoAt(0),
      hidden(false),
      pictureSeq(0),
      videoSeq(0)
{
    clock.start();
}

BridgeServer::~BridgeServer()
{
    // Runs on the bridge thread, after its event loop has ended.
    if(saveTimer && saveTimer->isActive())
        saveSettings();
    {
        QSqlDatabase db = QSqlDatabase::database(dbConnection, false);
        if(db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(dbConnection);
}

void BridgeServer::start()
{
    loadSettings();
    if(qEnvironmentVariableIsSet("CGF_BRIDGE_PORT"))
        port = quint16(qEnvironmentVariableIntValue("CGF_BRIDGE_PORT"));
    if(!enabled)
    {
        qInfo("Network bridge is turned off in %s", settingsFileName);
        return;
    }

    openDatabase();
    loadBibles();

    saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);
    saveTimer->setInterval(500);
    connect(saveTimer, &QTimer::timeout, this, &BridgeServer::saveSettings);

    tickTimer = new QTimer(this);
    tickTimer->setInterval(tickMs);
    connect(tickTimer, &QTimer::timeout, this, &BridgeServer::tick);
    tickTimer->start();

    ws = new QWebSocketServer(QStringLiteral("CGF Projector"), QWebSocketServer::NonSecureMode, this);
    connect(ws, &QWebSocketServer::newConnection, this, &BridgeServer::onWsConnection);

    tcp = new QTcpServer(this);
    connect(tcp, &QTcpServer::newConnection, this, &BridgeServer::onTcpConnection);
    // CGF_BRIDGE_LOCAL: this computer only (tests; no Windows Firewall prompt).
    const QHostAddress address = qEnvironmentVariableIsSet("CGF_BRIDGE_LOCAL")
            ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::Any);
    if(!tcp->listen(address, port))
    {
        qWarning("Network bridge cannot use port %d: %s", port, qPrintable(tcp->errorString()));
        return;
    }
    qInfo("Network bridge: http://%s:%d/", qPrintable(QHostInfo::localHostName()), port);
    emit started(port);
}

// ---------------------------------------------------------------------------
// Settings: cgf-network.json beside the database. A library snapshot replaces
// the database, so nothing here is kept in it.

void BridgeServer::loadSettings()
{
    QFile f(QDir(dataDir).filePath(QLatin1String(settingsFileName)));
    if(!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    enabled = o.value("enabled").toBool(true);
    const int p = o.value("port").toInt(defaultPort);
    if(p > 0 && p < 65536)
        port = quint16(p);
    style = o.value("stream").toObject();
}

void BridgeServer::saveSettings()
{
    const QString path = QDir(dataDir).filePath(QLatin1String(settingsFileName));
    QJsonObject o;
    {
        QFile f(path);
        if(f.open(QIODevice::ReadOnly))
            o = QJsonDocument::fromJson(f.readAll()).object();
    }
    o["stream"] = style;
    QSaveFile out(path);
    if(!out.open(QIODevice::WriteOnly)
            || out.write(QJsonDocument(o).toJson()) < 0
            || !out.commit())
        qWarning("Network bridge cannot save %s: %s", qPrintable(path), qPrintable(out.errorString()));
}

// ---------------------------------------------------------------------------
// Bible text. The bridge has its own read-only connection so verse lookups
// (BibleVerse has no index) never run on the projector's thread.

void BridgeServer::openDatabase()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), dbConnection);
    db.setDatabaseName(dbPath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
    if(!db.open())
        qWarning("Network bridge cannot read the database: %s", qPrintable(db.lastError().text()));
}

void BridgeServer::loadBibles()
{
    QSqlDatabase db = QSqlDatabase::database(dbConnection, false);
    if(!db.isOpen())
        return;
    QSqlQuery q(db);
    q.exec("SELECT id, bible_name, abbreviation FROM BibleVersions ORDER BY id");
    while(q.next())
        bibles.append(QJsonObject{{"id", q.value(0).toString()},
                                  {"name", q.value(1).toString().trimmed()},
                                  {"abbr", q.value(2).toString().trimmed()}});
    q.exec("SELECT bible_id, id, book_name FROM BibleBooks");
    while(q.next())
        bookNames.insert(q.value(0).toString() + ':' + q.value(1).toString(),
                         displayBookName(q.value(2).toString().trimmed()));
    sample = bibleItem({QStringLiteral("B043C003V016")}, QJsonArray()); // John 3:16
}

QString BridgeServer::bookName(const QString &bibleId, int book) const
{
    return bookNames.value(bibleId + ':' + QString::number(book));
}

QJsonObject BridgeServer::bibleItem(const QStringList &verseIds, const QJsonArray &projected)
{
    QJsonObject item{{"verseIds", QJsonArray::fromStringList(verseIds)}, {"projected", projected}};
    QJsonArray versions;
    QSqlDatabase db = QSqlDatabase::database(dbConnection, false);
    if(verseIds.isEmpty() || !db.isOpen())
    {
        item["versions"] = versions;
        return item;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT bible_id, verse_id, book, chapter, verse, verse_text FROM BibleVerse "
                             "WHERE verse_id IN (%1)").arg(QStringList(verseIds.size(), QStringLiteral("?")).join(',')));
    for(const QString &id : verseIds)
        q.addBindValue(id);
    if(!q.exec())
        qWarning("Network bridge Bible lookup failed: %s", qPrintable(q.lastError().text()));

    struct Row { int order; int book; int chapter; int verse; QString text; };
    QHash<QString, QList<Row>> rows;
    while(q.next())
        rows[q.value(0).toString()].append({int(verseIds.indexOf(q.value(1).toString())),
                                            q.value(2).toInt(), q.value(3).toInt(),
                                            q.value(4).toInt(), q.value(5).toString().trimmed()});

    for(const QJsonValue &b : std::as_const(bibles))
    {
        const QJsonObject bible = b.toObject();
        const QString id = bible["id"].toString();
        QList<Row> list = rows.value(id);
        if(list.isEmpty())
            continue;
        std::stable_sort(list.begin(), list.end(), [](const Row &x, const Row &y) { return x.order < y.order; });

        // Some Bibles split one verse over two rows; keep them as one verse.
        QJsonArray verses;
        QStringList text;
        for(int i = 0; i < list.size(); ++i)
        {
            const Row &r = list.at(i);
            if(i > 0 && r.chapter == list.at(i - 1).chapter && r.verse == list.at(i - 1).verse)
            {
                QJsonObject v = verses.at(verses.size() - 1).toObject();
                v["text"] = v["text"].toString() + ' ' + r.text;
                verses.replace(verses.size() - 1, v);
            }
            else
                verses.append(QJsonObject{{"chapter", r.chapter}, {"verse", r.verse}, {"text", r.text}});
            text << r.text;
        }

        const Row &first = list.first();
        const Row &last = list.last();
        QString ref = QStringLiteral("%1:%2").arg(first.chapter).arg(first.verse);
        if(last.chapter != first.chapter)
            ref += QStringLiteral("-%1:%2").arg(last.chapter).arg(last.verse);
        else if(last.verse != first.verse)
            ref += QStringLiteral("-%1").arg(last.verse);
        const QString book = bookName(id, first.book);

        versions.append(QJsonObject{{"id", id}, {"name", bible["name"]}, {"abbr", bible["abbr"]},
                                    {"book", book},
                                    {"reference", book.isEmpty() ? ref : book + ' ' + ref},
                                    {"text", text.join(' ')}, {"verses", verses}});
    }
    item["versions"] = versions;
    return item;
}

// ---------------------------------------------------------------------------
// Live state, pushed from the projector's thread by LiveBridge.

void BridgeServer::setLive(const QJsonObject &incoming, const QImage &picture, const QString &path)
{
    QJsonObject msg = incoming;
    msg["type"] = "live";
    msg["seq"] = ++seq;
    const QString kind = incoming["kind"].toString();

    if(kind == "bible")
    {
        const QJsonObject b = incoming["bible"].toObject();
        QStringList ids;
        for(const QJsonValue &v : b["verseIds"].toArray())
            ids << v.toString();
        msg["bible"] = bibleItem(ids, b["projected"].toArray());
    }
    else if(kind == "picture")
    {
        // A null image means "the same picture as before" (LiveBridge only
        // converts a picture once), so its address stays the same.
        if(!picture.isNull())
        {
            const bool alpha = picture.hasAlphaChannel();
            pictureBytes = encodeImage(picture, alpha ? "png" : "jpg");
            pictureType = alpha ? "image/png" : "image/jpeg";
            pictureSeq = seq;
        }
        QJsonObject p = incoming["picture"].toObject();
        p.remove("key");
        p["url"] = QStringLiteral("/media/picture/%1").arg(pictureSeq);
        msg["picture"] = p;
    }
    else if(kind == "video")
    {
        if(path != videoPath)
        {
            videoPath = path;
            videoSeq = seq;
            video = QJsonObject();
        }
        QJsonObject v = incoming["video"].toObject();
        const QByteArray type = mimeType(videoPath);
        v["seq"] = videoSeq;
        v["url"] = QStringLiteral("/media/video/%1").arg(videoSeq);
        v["mime"] = QString::fromLatin1(type);
        v["audioOnly"] = type.startsWith("audio/");
        msg["video"] = v;
    }

    live = msg;
    broadcast(live);
}

void BridgeServer::setVideo(const QJsonObject &state)
{
    video = state;
    video["type"] = "video";
    video["seq"] = videoSeq;
    videoAt = clock.elapsed();
    broadcast(video);
}

// The projector's rendered layers for the back-screen mirror; a null image
// means that layer did not change.
void BridgeServer::setFrame(const QImage &text, const QImage &back, const QSize &screen, bool fade)
{
    // The projector makes a new pixmap for every change even when it looks
    // the same (a plain colour background), so compare pixels: an unchanged
    // layer keeps its address and the mirror does not reload it.
    const int seqBefore = frameSeq++;
    const QSize size = screen.isValid() ? screen : (text.isNull() ? back.size() : text.size());
    if(!text.isNull() && text != frameTextImage)
    {
        frameTextImage = text;
        frameText = encodeImage(text, "png");
        frameTextSeq = frameSeq;
    }
    if(!back.isNull())
    {
        // Laid out as the projector lays it out: centred when smaller than
        // the screen, from the top left when larger.
        QImage canvas(size, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        painter.drawImage(back.width() < size.width() ? (size.width() - back.width()) / 2 : 0,
                          back.height() < size.height() ? (size.height() - back.height()) / 2 : 0, back);
        painter.end();
        if(canvas != frameBackImage)
        {
            frameBackImage = canvas;
            frameBack = encodeImage(canvas, "jpg");
            frameBackSeq = frameSeq;
        }
    }
    if(frameTextSeq <= seqBefore && frameBackSeq <= seqBefore && !frame.isEmpty())
        return; // nothing visible changed
    frame = QJsonObject{{"type", "frame"}, {"seq", frameSeq},
                        {"text", frameText.isEmpty() ? QString() : QStringLiteral("/frame/text/%1").arg(frameTextSeq)},
                        {"back", frameBack.isEmpty() ? QString() : QStringLiteral("/frame/back/%1").arg(frameBackSeq)},
                        {"width", size.width()}, {"height", size.height()},
                        {"fade", fade}, {"ms", 500}}; // DisplayArea.qml tranTime
    const QString message = compact(frame);
    const QList<QWebSocket *> list = clients; // a failed send may drop a client
    for(QWebSocket *c : list)
        if(c->property("cgfFrames").toBool())
            c->sendTextMessage(message);
}

void BridgeServer::countMirrors()
{
    int n = 0;
    for(QWebSocket *c : std::as_const(clients))
        n += c->property("cgfFrames").toBool();
    if((n > 0) != (mirrors > 0))
        emit framesWanted(n > 0);
    mirrors = n;
}

QJsonObject BridgeServer::videoMessage() const
{
    // For a client joining mid-video: where playback is now, not at the last update.
    QJsonObject v = video;
    if(v["state"].toString() == QLatin1String("playing"))
        v["position"] = v["position"].toDouble() + double(clock.elapsed() - videoAt);
    return v;
}

// ---------------------------------------------------------------------------
// HTTP. Every response closes its connection; requests are GET/HEAD only.

void BridgeServer::onTcpConnection()
{
    while(QTcpSocket *s = tcp->nextPendingConnection())
    {
        connect(s, &QTcpSocket::readyRead, this, &BridgeServer::onHttpReadyRead);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        // Drop connections that never finish their request.
        QTimer::singleShot(15000, s, [s] {
            if(!s->property("cgfHandled").toBool())
                s->abort();
        });
    }
}

void BridgeServer::onHttpReadyRead()
{
    QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
    if(!s)
        return;
    const QByteArray head = s->peek(maxHeaderBytes);
    const int end = head.indexOf("\r\n\r\n");
    if(end < 0)
    {
        if(head.size() >= maxHeaderBytes)
            s->abort();
        return;
    }

    Request r;
    const QList<QByteArray> lines = head.left(end).split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    if(first.size() < 3)
    {
        s->abort();
        return;
    }
    r.method = first.at(0);
    r.path = first.at(1);
    for(int i = 1; i < lines.size(); ++i)
    {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if(colon > 0)
            r.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }
    disconnect(s, &QTcpSocket::readyRead, this, &BridgeServer::onHttpReadyRead);
    s->setProperty("cgfHandled", true);

    const int query = r.path.indexOf('?');
    const QByteArray path = query < 0 ? r.path : r.path.left(query);
    if(path == "/ws" && r.headers.value("upgrade").toLower() == "websocket")
    {
        // Browsers send Origin. Only pages served by this bridge may connect,
        // so a website open on a church computer can't reach the projector.
        const QByteArray origin = r.headers.value("origin");
        if(!origin.isEmpty()
                && QUrl(QString::fromLatin1(origin)).authority().compare(
                    QString::fromLatin1(r.headers.value("host")), Qt::CaseInsensitive) != 0)
        {
            s->read(end + 4);
            respond(s, 403, "text/plain", "Forbidden\n");
            return;
        }
        // The handshake is still unread; the WebSocket server reads it and
        // owns the socket from here on.
        disconnect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        ws->handleConnection(s);
        return;
    }
    s->read(end + 4);
    r.path = path;
    handleRequest(s, r);
}

void BridgeServer::handleRequest(QTcpSocket *s, const Request &r)
{
    const bool headOnly = r.method == "HEAD";
    if(r.method != "GET" && !headOnly)
    {
        respond(s, 405, "text/plain", "Method not allowed\n");
        return;
    }
    const QByteArray path = QByteArray::fromPercentEncoding(r.path);
    if(path == "/")
        serveStatic(s, "index.html", headOnly);
    else if(path == "/stream")
        serveStatic(s, "stream.html", headOnly);
    else if(path == "/stream/settings")
        serveStatic(s, "settings.html", headOnly);
    else if(path == "/screen")
        serveStatic(s, "screen.html", headOnly);
    else if(path.startsWith("/assets/"))
        serveStatic(s, QString::fromUtf8(path.mid(1)), headOnly);
    else if(path == "/api/state")
        respond(s, 200, "application/json", QJsonDocument(stateObject()).toJson(), headOnly);
    else if(path.startsWith("/media/picture/"))
    {
        if(pictureBytes.isEmpty() || path.mid(15).toInt() != pictureSeq)
            respond(s, 404, "text/plain", "Not found\n", headOnly);
        else
            respond(s, 200, pictureType, pictureBytes, headOnly);
    }
    else if(path.startsWith("/frame/text/") || path.startsWith("/frame/back/"))
    {
        const bool isText = path.startsWith("/frame/text/");
        const QByteArray &bytes = isText ? frameText : frameBack;
        if(bytes.isEmpty() || path.mid(12).toInt() != (isText ? frameTextSeq : frameBackSeq))
            respond(s, 404, "text/plain", "Not found\n", headOnly);
        else
            respond(s, 200, isText ? "image/png" : "image/jpeg", bytes, headOnly);
    }
    else if(path.startsWith("/media/video/"))
    {
        // Only the file the operator put live, and only under its own address,
        // so the browser's range requests never mix two files.
        if(videoPath.isEmpty() || path.mid(13).toInt() != videoSeq)
            respond(s, 404, "text/plain", "Not found\n", headOnly);
        else
            serveFile(s, videoPath, mimeType(videoPath), r.headers.value("range"), headOnly);
    }
    else
        respond(s, 404, "text/plain", "Not found\n", headOnly);
}

void BridgeServer::serveStatic(QTcpSocket *s, const QString &name, bool headOnly)
{
    if(name.contains(QLatin1String("..")) || name.contains('\\') || name.contains(':'))
    {
        respond(s, 404, "text/plain", "Not found\n", headOnly);
        return;
    }
    QFile f(webDir.isEmpty() ? QStringLiteral(":/web/") + name : QDir(webDir).filePath(name));
    if(!f.open(QIODevice::ReadOnly))
    {
        respond(s, 404, "text/plain", "Not found\n", headOnly);
        return;
    }
    respond(s, 200, mimeType(name), f.readAll(), headOnly);
}

void BridgeServer::serveFile(QTcpSocket *s, const QString &path, const QByteArray &type,
                             const QByteArray &range, bool headOnly)
{
    auto *file = new QFile(path, s);
    if(!file->open(QIODevice::ReadOnly))
    {
        respond(s, 404, "text/plain", "Not found\n", headOnly);
        return;
    }
    const qint64 size = file->size();
    qint64 first = 0;
    qint64 last = size - 1;
    int status = 200;
    if(range.startsWith("bytes=") && !range.contains(','))
    {
        const QByteArray spec = range.mid(6).trimmed();
        const int dash = spec.indexOf('-');
        bool ok = dash >= 0;
        if(ok && dash == 0)
            first = qMax<qint64>(0, size - spec.mid(1).toLongLong(&ok)); // last N bytes
        else if(ok)
        {
            first = spec.left(dash).toLongLong(&ok);
            if(ok && dash + 1 < spec.size())
                last = qMin(size - 1, spec.mid(dash + 1).toLongLong(&ok));
        }
        if(!ok || first > last || first >= size)
        {
            respond(s, 416, "text/plain", QByteArray(), headOnly,
                    "Content-Range: bytes */" + QByteArray::number(size) + "\r\n");
            return;
        }
        status = 206;
    }

    const qint64 length = qMax<qint64>(0, last - first + 1);
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n"
            "Content-Type: " + type + "\r\n"
            "Content-Length: " + QByteArray::number(length) + "\r\n"
            "Accept-Ranges: bytes\r\n";
    if(status == 206)
        h += "Content-Range: bytes " + QByteArray::number(first) + '-' + QByteArray::number(last)
                + '/' + QByteArray::number(size) + "\r\n";
    h += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    s->write(h);
    if(headOnly || length == 0)
    {
        s->disconnectFromHost();
        return;
    }

    // Stream in chunks as the socket drains; a video can be gigabytes.
    file->seek(first);
    auto remaining = std::make_shared<qint64>(length);
    auto pump = [s, file, remaining] {
        while(*remaining > 0 && s->bytesToWrite() < 512 * 1024)
        {
            const QByteArray chunk = file->read(qMin<qint64>(*remaining, 64 * 1024));
            if(chunk.isEmpty())
            {
                s->abort();
                return;
            }
            *remaining -= chunk.size();
            s->write(chunk);
        }
        if(*remaining == 0)
        {
            *remaining = -1;
            s->disconnectFromHost();
        }
    };
    connect(s, &QTcpSocket::bytesWritten, s, pump);
    pump();
}

void BridgeServer::respond(QTcpSocket *s, int status, const QByteArray &type,
                           const QByteArray &body, bool headOnly, const QByteArray &extraHeaders)
{
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    if(!type.isEmpty())
        h += "Content-Type: " + type + "\r\n";
    h += "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
         "Cache-Control: no-store\r\nConnection: close\r\n" + extraHeaders + "\r\n";
    s->write(h);
    if(!headOnly)
        s->write(body);
    s->disconnectFromHost();
}

// ---------------------------------------------------------------------------
// WebSocket clients

void BridgeServer::onWsConnection()
{
    while(QWebSocket *c = ws->nextPendingConnection())
    {
        c->setMaxAllowedIncomingMessageSize(c->peerAddress().isLoopback() ? maxLocalMessageBytes
                                                                          : maxMessageBytes);
        c->setProperty("cgfSeen", clock.elapsed());
        clients.append(c);
        connect(c, &QWebSocket::textMessageReceived, this, &BridgeServer::onWsMessage);
        connect(c, &QWebSocket::disconnected, this, &BridgeServer::onWsDisconnected);
        connect(c, &QWebSocket::pong, c, [this, c] { c->setProperty("cgfSeen", clock.elapsed()); });
        send(c, helloMessage());
        send(c, styleMessage());
        if(!live.isEmpty())
            send(c, live);
        if(!video.isEmpty())
            send(c, videoMessage());
    }
}

void BridgeServer::onWsDisconnected()
{
    QWebSocket *c = qobject_cast<QWebSocket *>(sender());
    clients.removeAll(c);
    if(c)
        c->deleteLater();
    countMirrors();
}

void BridgeServer::onWsMessage(const QString &text)
{
    QWebSocket *c = qobject_cast<QWebSocket *>(sender());
    if(c)
        c->setProperty("cgfSeen", clock.elapsed());
    const QJsonObject m = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = m.value("type").toString();
    if(type == QLatin1String("subscribe") && c)
    {
        // The back-screen mirror asks for rendered frames; others never pay for them.
        const bool frames = m.value("frames").toBool();
        c->setProperty("cgfFrames", frames);
        countMirrors();
        if(frames && !frame.isEmpty())
            send(c, frame);
    }
    else if(type == QLatin1String("setStyle") && m.value("style").isObject())
    {
        style = m.value("style").toObject();
        saveTimer->start();
        broadcast(styleMessage());
    }
    else if(type == QLatin1String("setHidden"))
    {
        hidden = m.value("hidden").toBool();
        broadcast(styleMessage());
    }
    else if(type == QLatin1String("release") && c)
    {
        // Only from this computer. The main window checks the publisher's
        // signature, so the sender itself need not be trusted.
        if(c->peerAddress().isLoopback())
            emit releaseReceived(m, quintptr(c));
        else
            send(c, QJsonObject{{"type", "releaseResult"}, {"status", "rejected"},
                                {"reason", "releases are accepted from this computer only"}});
    }
}

void BridgeServer::setLibraryVersion(int version)
{
    if(version == libraryVersion)
        return;
    libraryVersion = version;
    broadcast(QJsonObject{{"type", "library"}, {"version", version}});
}

void BridgeServer::sendToClient(quintptr client, const QJsonObject &message)
{
    for(QWebSocket *c : std::as_const(clients))
        if(quintptr(c) == client)
        {
            send(c, message);
            return;
        }
}

void BridgeServer::tick()
{
    // Pages reconnect when the ticks stop; clients that stop answering pings
    // (a phone that went to sleep, a dropped Wi-Fi) are let go.
    const QString message = compact(QJsonObject{{"type", "tick"}});
    // A copy: abort() disconnects at once, which removes the client from the list.
    const QList<QWebSocket *> list = clients;
    for(QWebSocket *c : list)
    {
        if(clock.elapsed() - c->property("cgfSeen").toLongLong() > clientTimeoutMs)
        {
            c->abort();
            continue;
        }
        c->ping();
        c->sendTextMessage(message);
    }
}

QJsonObject BridgeServer::helloMessage() const
{
    QJsonArray addresses;
    for(const QHostAddress &a : QNetworkInterface::allAddresses())
        if(a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            addresses.append(a.toString());
    return QJsonObject{{"type", "hello"}, {"protocol", 1}, {"app", "CGF Projector"},
                       {"host", QHostInfo::localHostName()}, {"port", port},
                       {"addresses", addresses}, {"bibles", bibles}, {"sample", sample},
                       {"library", QJsonObject{{"version", libraryVersion}}}};
}

QJsonObject BridgeServer::styleMessage() const
{
    return QJsonObject{{"type", "style"}, {"style", style}, {"hidden", hidden}};
}

QJsonObject BridgeServer::stateObject() const
{
    return QJsonObject{{"live", live}, {"video", videoMessage()}, {"style", style},
                       {"hidden", hidden}, {"bibles", bibles}, {"clients", int(clients.size())}};
}

void BridgeServer::send(QWebSocket *client, const QJsonObject &message)
{
    client->sendTextMessage(compact(message));
}

void BridgeServer::broadcast(const QJsonObject &message)
{
    const QString text = compact(message);
    const QList<QWebSocket *> list = clients; // a failed send may drop a client
    for(QWebSocket *c : list)
        c->sendTextMessage(text);
}
