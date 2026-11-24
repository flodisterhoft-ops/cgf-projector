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

#include "companion.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRandomGenerator>

namespace {

const int retrySeconds[] = {2, 5, 10, 30, 60};
const int heartbeatMs = 25000;
const qint64 serverSilentMs = 70000;   // no pong or message for this long: the link is dead
const qint64 bridgeGoneExitMs = 60000; // the app has closed
const qint64 appAnswerMs = 30000;
const qint64 maxLogBytes = 512 * 1024;
const char agent[] = "cgf-companion/1";

QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parse(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

// "v3 -> v4 (part 1/2): 2 changed, 1 added, 0 removed songs"
QString describe(const QString &envelope)
{
    const QJsonObject p = parse(parse(envelope).value(QStringLiteral("payload")).toString());
    const QJsonObject s = p.value(QStringLiteral("summary")).toObject();
    QString text = QStringLiteral("v%1 -> v%2").arg(p.value(QStringLiteral("from")).toInt())
            .arg(p.value(QStringLiteral("to")).toInt());
    const int parts = p.value(QStringLiteral("parts")).toInt(1);
    if(parts > 1)
        text += QStringLiteral(" (part %1/%2)").arg(p.value(QStringLiteral("part")).toInt()).arg(parts);
    return text + QStringLiteral(": %1 changed, %2 added, %3 removed songs")
            .arg(s.value(QStringLiteral("songs_changed")).toInt())
            .arg(s.value(QStringLiteral("songs_added")).toInt())
            .arg(s.value(QStringLiteral("songs_deleted")).toInt());
}

} // namespace

Companion::Companion(const QString &configPath, quint16 bridgePort, QObject *parent)
    : QObject(parent),
      configPath(configPath),
      bridgePort(bridgePort)
{
    connect(&bridge, &QWebSocket::connected, this, &Companion::onBridgeConnected);
    connect(&bridge, &QWebSocket::textMessageReceived, this, &Companion::onBridgeMessage);
    connect(&bridge, &QWebSocket::disconnected, this, &Companion::onBridgeDisconnected);
    connect(&bridge, &QWebSocket::errorOccurred, this, [this] {
        if(bridge.state() == QAbstractSocket::UnconnectedState)
            onBridgeDisconnected();
    });

    connect(&server, &QWebSocket::connected, this, &Companion::onServerConnected);
    connect(&server, &QWebSocket::textMessageReceived, this, &Companion::onServerMessage);
    connect(&server, &QWebSocket::disconnected, this, &Companion::onServerDisconnected);
    connect(&server, &QWebSocket::errorOccurred, this, [this] {
        if(retryStep < 5 || retryStep % 10 == 0)
            log(QStringLiteral("push server: %1").arg(server.errorString()));
        if(server.state() == QAbstractSocket::UnconnectedState)
            onServerDisconnected();
    });

    heartbeatTimer.setInterval(heartbeatMs);
    connect(&heartbeatTimer, &QTimer::timeout, this, &Companion::heartbeat);
    serverRetry.setSingleShot(true);
    connect(&serverRetry, &QTimer::timeout, this, &Companion::connectServer);
    bridgeRetry.setSingleShot(true);
    bridgeRetry.setInterval(2000);
    connect(&bridgeRetry, &QTimer::timeout, this, &Companion::connectBridge);
}

bool Companion::start()
{
    QFile f(configPath);
    if(!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    serverUrl = QUrl(o.value(QStringLiteral("server")).toString());
    token = o.value(QStringLiteral("token")).toString().toUtf8();
    device = o.value(QStringLiteral("device")).toString();

    const QString logName = QFileInfo(configPath).dir().filePath(QStringLiteral("cgf-companion.log"));
    if(QFileInfo(logName).size() > maxLogBytes)
    {
        QFile::remove(logName + QStringLiteral(".1"));
        QFile::rename(logName, logName + QStringLiteral(".1"));
    }
    logFile.setFileName(logName);

    const QString scheme = serverUrl.scheme();
    if(!serverUrl.isValid() || (scheme != QLatin1String("ws") && scheme != QLatin1String("wss")) || token.isEmpty())
    {
        log(QStringLiteral("cgf-cloud.json needs a ws:// or wss:// server and a token; stopping"));
        return false;
    }
    log(QStringLiteral("started for '%1' (server %2, app bridge port %3)")
        .arg(device, serverUrl.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery)).arg(bridgePort));
    bridgeGoneFor.start();
    sinceServerMessage.start();
    appBusySince.start();
    heartbeatTimer.start();
    connectBridge();
    return true;
}

void Companion::log(const QString &line)
{
    if(logFile.isOpen() || logFile.open(QIODevice::Append | QIODevice::Text))
    {
        logFile.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss  ")).toUtf8()
                      + line.toUtf8() + '\n');
        logFile.flush();
    }
}

// ---------------------------------------------------------------------------
// The projector app, over its bridge on this computer

void Companion::connectBridge()
{
    if(bridge.state() == QAbstractSocket::UnconnectedState)
        bridge.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(bridgePort)));
}

void Companion::onBridgeConnected()
{
    bridgeOpen = true;
    // Waits for the app's hello, which carries its library version.
}

void Companion::onBridgeDisconnected()
{
    if(bridgeOpen)
    {
        log(QStringLiteral("the projector app closed its connection"));
        bridgeGoneFor.restart();
    }
    bridgeOpen = false;
    appBusy = false;
    if(bridgeGoneFor.elapsed() > bridgeGoneExitMs)
    {
        stop(QStringLiteral("the projector app is gone"));
        return;
    }
    if(!bridgeRetry.isActive() && !stopping)
        bridgeRetry.start();
}

void Companion::onBridgeMessage(const QString &text)
{
    const QJsonObject m = parse(text);
    const QString type = m.value(QStringLiteral("type")).toString();
    int version = -2;
    if(type == QLatin1String("hello"))
        version = m.value(QStringLiteral("library")).toObject().value(QStringLiteral("version")).toInt(-1);
    else if(type == QLatin1String("library"))
        version = m.value(QStringLiteral("version")).toInt(-1);
    else if(type == QLatin1String("releaseResult"))
    {
        appBusy = false;
        const QString status = m.value(QStringLiteral("status")).toString();
        const int to = m.value(QStringLiteral("to")).toInt();
        const int now = m.value(QStringLiteral("version")).toInt(-1);
        if(now >= 0)
            libraryVersion = now;
        if(status == QLatin1String("applied") || status == QLatin1String("duplicate"))
        {
            if(status == QLatin1String("applied"))
                log(QStringLiteral("v%1 is in the projector's library").arg(to));
            if(serverOpen)
                server.sendTextMessage(compact({{"type", "ack"}, {"version", libraryVersion}}));
        }
        else if(status == QLatin1String("gap"))
        {
            // It missed a release (or the app restarted on another version).
            toApp.clear();
            sendHello();
        }
        else if(status == QLatin1String("rejected"))
            log(QStringLiteral("the projector refused v%1: %2").arg(to)
                .arg(m.value(QStringLiteral("reason")).toString()));
        sendNextToApp();
        return;
    }
    else
        return; // live screen updates and ticks are for the browser pages

    if(version < 0)
        return;
    const bool first = libraryVersion < 0;
    const bool changed = version != libraryVersion;
    libraryVersion = version;
    if(first)
    {
        log(QStringLiteral("the projector's library is at v%1").arg(version));
        connectServer();
    }
    else if(type == QLatin1String("hello") && changed)
    {
        // The app restarted, perhaps on a new snapshot: start over from there.
        log(QStringLiteral("the projector's library is now at v%1").arg(version));
        toApp.clear();
        appBusy = false;
        sendHello();
    }
    sendNextToApp();
}

void Companion::sendNextToApp()
{
    if(appBusy || !bridgeOpen || toApp.isEmpty())
        return;
    const QString envelope = toApp.dequeue();
    log(QStringLiteral("passing %1 to the projector").arg(describe(envelope)));
    bridge.sendTextMessage(envelope);
    appBusy = true;
    appBusySince.restart();
}

// ---------------------------------------------------------------------------
// The push server

void Companion::connectServer()
{
    if(serverOpen || serverConnecting || libraryVersion < 0)
        return;
    QNetworkRequest request(serverUrl);
    request.setRawHeader("Authorization", "Bearer " + token);
    request.setRawHeader("User-Agent", agent);
    serverConnecting = true;
    server.open(request);
}

void Companion::onServerConnected()
{
    serverConnecting = false;
    serverOpen = true;
    if(retryStep > 0)
        log(QStringLiteral("connected to the push server"));
    else
        log(QStringLiteral("connected to the push server (library v%1)").arg(libraryVersion));
    retryStep = 0;
    sinceServerMessage.restart();
    sendHello();
}

void Companion::onServerDisconnected()
{
    if(serverOpen)
        log(QStringLiteral("push server connection closed"));
    serverOpen = false;
    serverConnecting = false;
    if(serverRetry.isActive())
        return;
    const int delay = retrySeconds[qMin(retryStep, 4)] * 1000 + QRandomGenerator::global()->bounded(1000);
    ++retryStep;
    serverRetry.start(delay);
}

void Companion::onServerMessage(const QString &text)
{
    sinceServerMessage.restart();
    if(text == QLatin1String("pong"))
        return;
    const QJsonObject m = parse(text);
    const QString type = m.value(QStringLiteral("type")).toString();
    if(type == QLatin1String("release"))
    {
        toApp.enqueue(text);
        sendNextToApp();
    }
    else if(type == QLatin1String("resync"))
        log(QStringLiteral("the push server cannot bring this laptop from v%1 to v%2; "
                           "the sync agent's next snapshot install will")
            .arg(m.value(QStringLiteral("have")).toInt()).arg(m.value(QStringLiteral("latest")).toInt()));
}

void Companion::sendHello()
{
    if(serverOpen && libraryVersion >= 0)
        server.sendTextMessage(compact({{"type", "hello"}, {"version", libraryVersion}, {"app", agent}}));
}

void Companion::heartbeat()
{
    if(stopping)
        return;
    if(serverOpen)
    {
        if(sinceServerMessage.elapsed() > serverSilentMs)
        {
            log(QStringLiteral("the push server stopped answering; reconnecting"));
            server.abort();
        }
        else
            server.sendTextMessage(QStringLiteral("ping"));
    }
    if(appBusy && appBusySince.elapsed() > appAnswerMs)
    {
        log(QStringLiteral("the projector did not answer; asking the server again"));
        appBusy = false;
        toApp.clear();
        sendHello();
    }
    if(!bridgeOpen && bridgeGoneFor.elapsed() > bridgeGoneExitMs)
        stop(QStringLiteral("the projector app is gone"));
}

void Companion::stop(const QString &why)
{
    if(stopping)
        return;
    stopping = true;
    log(why + QStringLiteral("; stopping"));
    QCoreApplication::quit();
}
