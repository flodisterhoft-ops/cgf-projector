/***************************************************************************
//
//    CGF Projector companion - relays releases from the push server
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

#ifndef COMPANION_HPP
#define COMPANION_HPP

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

// Holds the connection to the push server on the church laptop's behalf, so
// the projector app itself never talks to the internet (docs/song-push.md).
//  - Library releases go to the app over the local bridge, one at a time, in
//    order; the app checks their signature and answers with a result.
//  - App releases (a new app version) are installed here: the files land in
//    a new app-<version> folder, and the launcher starts that folder from
//    the next start on. The running app is never touched.
// Started by the launcher at Windows sign-in (--stay: keeps running) and by
// the app (stops a minute after the app has closed).
class Companion : public QObject
{
    Q_OBJECT

public:
    Companion(const QString &configPath, const QString &dataDir, quint16 bridgePort, bool stay,
              QObject *parent = nullptr);
    bool start();

private slots:
    void onBridgeConnected();
    void onBridgeMessage(const QString &text);
    void onBridgeDisconnected();
    void onServerConnected();
    void onServerMessage(const QString &text);
    void onServerDisconnected();
    void heartbeat();

private:
    void connectBridge();
    void connectServer();
    void sendHello();
    void sendNextToApp();
    void stop(const QString &why);
    void log(const QString &line);

    // App releases
    void handleAppRelease(const QString &envelope);
    void installNext();
    void installFailed(const QString &why);
    void installDone();
    void announceUpdate();
    QMap<QString, QString> readVersions() const;
    bool writeVersions(const QMap<QString, QString> &state) const;
    bool versionedInstall() const;

    QString configPath;
    QString dataDir;
    quint16 bridgePort;
    bool stay;
    QUrl serverUrl;
    QString filesUrl;          // https://.../v1/<church>/app/files/
    QByteArray token;
    QString device;
    QString church;
    QList<QByteArray> publisherKeys;
    QFile logFile;

    QWebSocket bridge;
    QWebSocket server;
    QNetworkAccessManager http;
    bool bridgeOpen = false;
    bool serverOpen = false;
    bool serverConnecting = false;
    bool stopping = false;
    int libraryVersion = -1;   // from the app; -1 while it is closed
    QString appFolder;         // the folder the open app runs from

    QQueue<QString> toApp;     // library releases waiting for the app, in order
    bool appBusy = false;
    QElapsedTimer appBusySince;

    // The app release being installed
    struct Install
    {
        int seq = 0;
        QString folder;
        QString version;
        QString notes;
        QJsonObject payload;
        QJsonArray files;
        int next = 0;
        int copied = 0;
        int downloaded = 0;
        QString partial;       // <dataDir>/<folder>.partial
    };
    Install job;
    bool installing = false;
    QString retryEnvelope;     // the last release, retried after a failure
    QTimer installRetry;

    QTimer heartbeatTimer;
    QTimer serverRetry;
    QTimer bridgeRetry;
    int retryStep = 0;
    QElapsedTimer sinceServerMessage;
    QElapsedTimer bridgeGoneFor;
};

#endif // COMPANION_HPP
