/***************************************************************************
//
//    CGF Projector companion - relays library releases from the push server
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
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

// Holds the connection to the push server on the church laptop's behalf, so
// the projector app itself never talks to the internet (an architecture
// decision; docs/song-push.md). Releases go to the app over the local bridge,
// one at a time, in order; the app checks their signature and answers with a
// result, which is acknowledged to the server.
class Companion : public QObject
{
    Q_OBJECT

public:
    Companion(const QString &configPath, quint16 bridgePort, QObject *parent = nullptr);
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

    QString configPath;
    quint16 bridgePort;
    QUrl serverUrl;
    QByteArray token;
    QString device;
    QFile logFile;

    QWebSocket bridge;
    QWebSocket server;
    bool bridgeOpen = false;
    bool serverOpen = false;
    bool serverConnecting = false;
    bool stopping = false;
    int libraryVersion = -1;   // from the app; -1 until it reports one

    QQueue<QString> toApp;     // releases waiting for the app, in order
    bool appBusy = false;      // a release was sent; waiting for its result
    QElapsedTimer appBusySince;

    QTimer heartbeatTimer;
    QTimer serverRetry;
    QTimer bridgeRetry;
    int retryStep = 0;
    QElapsedTimer sinceServerMessage;
    QElapsedTimer bridgeGoneFor;
};

#endif // COMPANION_HPP
