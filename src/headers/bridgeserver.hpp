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

#ifndef BRIDGESERVER_HPP
#define BRIDGESERVER_HPP

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QWebSocket;
class QWebSocketServer;

// The network side of the bridge. Lives on its own thread (see LiveBridge):
// serves the web pages from web.qrc, the live picture or video file, and one
// WebSocket endpoint (/ws) that broadcasts what the projector shows. One port
// carries all of it. See docs/network-bridge.md for the protocol.
class BridgeServer : public QObject
{
    Q_OBJECT

public:
    BridgeServer(const QString &dataDir, const QString &dbPath);
    ~BridgeServer();

    static constexpr quint16 defaultPort = 8770;

public slots:
    void start();
    void setLive(const QJsonObject &live, const QImage &picture, const QString &videoPath);
    void setVideo(const QJsonObject &video);
    void setFrame(const QImage &text, const QImage &back, const QSize &screen, bool fade);
    // Library releases (docs/song-push.md): the version goes into hello, and
    // each release's result back to the program that sent it.
    void setLibraryVersion(int version);
    void sendToClient(quintptr client, const QJsonObject &message);

signals:
    void started(quint16 port);
    void framesWanted(bool wanted); // a back-screen mirror connected, or the last one left
    // A signed library release from a program on this computer (the companion).
    void releaseReceived(const QJsonObject &envelope, quintptr client);
    // The companion installed a newer app version (folder, version, notes).
    void appUpdateReady(const QJsonObject &update);

private slots:
    void onTcpConnection();
    void onHttpReadyRead();
    void onWsConnection();
    void onWsMessage(const QString &message);
    void onWsDisconnected();
    void tick();
    void saveSettings();

private:
    struct Request
    {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers; // names in lower case
    };

    void loadSettings();
    void openDatabase();
    void loadBibles();
    QJsonObject bibleItem(const QStringList &verseIds, const QJsonArray &projected);
    QString bookName(const QString &bibleId, int book) const;

    void handleRequest(QTcpSocket *socket, const Request &request);
    void serveStatic(QTcpSocket *socket, const QString &name, bool headOnly);
    void serveFile(QTcpSocket *socket, const QString &path, const QByteArray &type,
                   const QByteArray &range, bool headOnly);
    void respond(QTcpSocket *socket, int status, const QByteArray &type,
                 const QByteArray &body, bool headOnly = false,
                 const QByteArray &extraHeaders = QByteArray());

    QJsonObject helloMessage() const;
    QJsonObject styleMessage() const;
    QJsonObject videoMessage() const;
    QJsonObject stateObject() const;
    void send(QWebSocket *client, const QJsonObject &message);
    void broadcast(const QJsonObject &message);
    void countMirrors();

    QString dataDir;
    QString dbPath;
    QString dbConnection;
    QString webDir;          // CGF_WEB_DIR: serve the pages from disk while editing them
    quint16 port;
    bool enabled;

    QTcpServer *tcp;
    QWebSocketServer *ws;
    QList<QWebSocket *> clients;
    QTimer *saveTimer;
    QTimer *tickTimer;
    QElapsedTimer clock;

    QJsonArray bibles;                   // installed Bibles: id, name, abbr
    QHash<QString, QString> bookNames;   // "bibleId:book" -> display name
    QJsonObject sample;                  // John 3:16 in every Bible, for previews

    int seq;
    int libraryVersion = -1;             // -1 until the main window reports it
    QJsonObject live;
    QJsonObject video;
    qint64 videoAt;                      // clock time of the last video update
    QJsonObject style;
    bool hidden;                         // stream operator's manual hide; not saved

    QByteArray pictureBytes;
    QByteArray pictureType;
    int pictureSeq;
    QString videoPath;
    int videoSeq;

    // Back-screen mirror: the projector's rendered layers
    int mirrors = 0;
    int frameSeq = 0;
    QJsonObject frame;
    QImage frameTextImage;
    QByteArray frameText;
    int frameTextSeq = 0;
    QImage frameBackImage;
    QByteArray frameBack;
    int frameBackSeq = 0;
};

#endif // BRIDGESERVER_HPP
