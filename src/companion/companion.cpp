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

#include "companion.hpp"
#include "../headers/cgfsignature.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

const int retrySeconds[] = {2, 5, 10, 30, 60};
const int heartbeatMs = 25000;
const qint64 serverSilentMs = 70000;   // no pong or message for this long: the link is dead
const qint64 bridgeGoneExitMs = 60000; // the app has closed (when not --stay)
const qint64 appAnswerMs = 30000;
const int installRetryMs = 10 * 60 * 1000;
const qint64 maxLogBytes = 512 * 1024;
const char agent[] = "cgf-companion/2";
const char versionsFile[] = "cgf-versions.txt";

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

QByteArray fileSha256(const QString &path)
{
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

bool safeRelativePath(const QString &p)
{
    if(p.isEmpty() || p.size() > 260 || p.startsWith(QLatin1Char('/')) || p.contains(QLatin1Char('\\'))
            || p.contains(QLatin1Char(':')))
        return false;
    for(const QString &part : p.split(QLatin1Char('/')))
        if(part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    return true;
}

const QRegularExpression &appFolderPattern()
{
    static const QRegularExpression rx(QStringLiteral("^app-[A-Za-z0-9._-]{1,40}$"));
    return rx;
}

} // namespace

Companion::Companion(const QString &configPath, const QString &dataDir, quint16 bridgePort, bool stay,
                     QObject *parent)
    : QObject(parent),
      configPath(configPath),
      dataDir(dataDir),
      bridgePort(bridgePort),
      stay(stay)
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
    connect(&bridgeRetry, &QTimer::timeout, this, &Companion::connectBridge);
    installRetry.setSingleShot(true);
    connect(&installRetry, &QTimer::timeout, this, [this] {
        if(!retryEnvelope.isEmpty())
            handleAppRelease(retryEnvelope);
    });
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
    church = o.value(QStringLiteral("church")).toString();
    publisherKeys = cgfPublisherKeys(o.value(QStringLiteral("publisherKeys")).toArray());
    if(dataDir.isEmpty())
        dataDir = QFileInfo(configPath).absolutePath();

    const QString logName = QDir(dataDir).filePath(QStringLiteral("cgf-companion.log"));
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
    // https://<host>/v1/<church>/app/files/ beside wss://<host>/v1/<church>/ws
    QUrl files = serverUrl;
    files.setScheme(scheme == QLatin1String("wss") ? QStringLiteral("https") : QStringLiteral("http"));
    QString path = files.path();
    if(path.endsWith(QLatin1String("/ws")))
        path.chop(3);
    files.setPath(path + QStringLiteral("/app/files/"));
    filesUrl = files.toString();

    log(QStringLiteral("started for '%1' (%2, server %3, app bridge port %4)")
        .arg(device, QFileInfo(QCoreApplication::applicationDirPath()).fileName(),
             serverUrl.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery)).arg(bridgePort));
    bridgeGoneFor.start();
    sinceServerMessage.start();
    appBusySince.start();
    heartbeatTimer.start();
    connectBridge();
    connectServer();
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

void Companion::stop(const QString &why)
{
    if(stopping)
        return;
    stopping = true;
    log(why + QStringLiteral("; stopping"));
    QCoreApplication::quit();
}

// ---------------------------------------------------------------------------
// The projector app, over its bridge on this computer

void Companion::connectBridge()
{
    if(!stopping && bridge.state() == QAbstractSocket::UnconnectedState)
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
    appHolding = false;
    appFeatures.clear();
    toApp.clear();
    appFolder.clear();
    if(libraryVersion >= 0)
    {
        libraryVersion = -1;
        sendHello(); // the server stops sending songs until the app is back
    }
    if(!stay && bridgeGoneFor.elapsed() > bridgeGoneExitMs)
    {
        stop(QStringLiteral("the projector app is gone"));
        return;
    }
    if(!bridgeRetry.isActive() && !stopping)
        bridgeRetry.start(stay ? 5000 : 2000);
}

void Companion::onBridgeMessage(const QString &text)
{
    const QJsonObject m = parse(text);
    const QString type = m.value(QStringLiteral("type")).toString();
    int version = -2;
    if(type == QLatin1String("hello"))
    {
        const QJsonObject library = m.value(QStringLiteral("library")).toObject();
        version = library.value(QStringLiteral("version")).toInt(-1);
        appFeatures.clear();
        for(const QJsonValue &f : library.value(QStringLiteral("features")).toArray())
            appFeatures << f.toString();
        appHolding = false;
        waitingFor.clear();
        appFolder = m.value(QStringLiteral("app")).toObject().value(QStringLiteral("folder")).toString();
        announceUpdate();
    }
    else if(type == QLatin1String("library"))
        version = m.value(QStringLiteral("version")).toInt(-1);
    else if(type == QLatin1String("releaseResult"))
    {
        appBusy = false;
        const QString status = m.value(QStringLiteral("status")).toString();
        if(status == QLatin1String("held"))
        {
            // Stays queued on the app's side; its answer follows later.
            if(!appHolding)
                log(QStringLiteral("v%1 changes Bibles; the projector applies it once the passage leaves the screen")
                    .arg(m.value(QStringLiteral("to")).toInt()));
            appHolding = true;
            return;
        }
        appHolding = false;
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
    const bool changed = version != libraryVersion;
    libraryVersion = version;
    if(changed && type == QLatin1String("hello"))
    {
        log(QStringLiteral("the projector's library is at v%1").arg(version));
        toApp.clear();
        appBusy = false;
        sendHello(); // the server sends what the library still lacks
    }
    sendNextToApp();
}

void Companion::sendNextToApp()
{
    if(appBusy || appHolding || !bridgeOpen || toApp.isEmpty())
        return;
    // A release needing something the open app does not understand waits
    // for a newer app (an update installs at its next start).
    const QJsonObject body = parse(parse(toApp.head()).value(QStringLiteral("payload")).toString());
    for(const QJsonValue &need : body.value(QStringLiteral("needs")).toArray())
        if(!appFeatures.contains(need.toString()))
        {
            if(waitingFor != need.toString())
                log(QStringLiteral("v%1 needs a newer projector app (%2); it waits until one runs")
                    .arg(body.value(QStringLiteral("to")).toInt()).arg(need.toString()));
            waitingFor = need.toString();
            return;
        }
    waitingFor.clear();
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
    if(serverOpen || serverConnecting || stopping)
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
    log(QStringLiteral("connected to the push server"));
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
    if(serverRetry.isActive() || stopping)
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
        if(libraryVersion < 0)
            return; // the app is closed; it asks again when it opens
        toApp.enqueue(text);
        sendNextToApp();
    }
    else if(type == QLatin1String("app"))
        handleAppRelease(text);
    else if(type == QLatin1String("resync"))
        log(QStringLiteral("the push server cannot bring this laptop from v%1 to v%2; "
                           "the sync agent's next snapshot install will")
            .arg(m.value(QStringLiteral("have")).toInt()).arg(m.value(QStringLiteral("latest")).toInt()));
}

void Companion::sendHello()
{
    if(!serverOpen)
        return;
    QJsonObject hello{{"type", "hello"}, {"agent", agent}};
    // Without the app open there is no library version: only app releases come.
    if(libraryVersion >= 0)
        hello["version"] = libraryVersion;
    const QString current = readVersions().value(QStringLiteral("current"));
    if(!current.isEmpty())
        hello["app"] = current;
    server.sendTextMessage(compact(hello));
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
    if(!stay && !bridgeOpen && bridgeGoneFor.elapsed() > bridgeGoneExitMs)
        stop(QStringLiteral("the projector app is gone"));
}

// ---------------------------------------------------------------------------
// App releases: a new version goes into its own app-<version> folder

QMap<QString, QString> Companion::readVersions() const
{
    QMap<QString, QString> state;
    QFile f(QDir(dataDir).filePath(QLatin1String(versionsFile)));
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return state;
    for(const QByteArray &raw : f.readAll().split('\n'))
    {
        const QString line = QString::fromUtf8(raw).trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if(eq > 0)
            state.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return state;
}

bool Companion::writeVersions(const QMap<QString, QString> &state) const
{
    QByteArray text;
    const QStringList order = {"current", "previous", "seq", "trial", "starts", "bad"};
    for(const QString &k : order)
        text += (k + QLatin1Char('=') + state.value(k) + QLatin1Char('\n')).toUtf8();
    for(auto it = state.begin(); it != state.end(); ++it)
        if(!order.contains(it.key()))
            text += (it.key() + QLatin1Char('=') + it.value() + QLatin1Char('\n')).toUtf8();
    QSaveFile out(QDir(dataDir).filePath(QLatin1String(versionsFile)));
    return out.open(QIODevice::WriteOnly) && out.write(text) == text.size() && out.commit();
}

bool Companion::versionedInstall() const
{
    return QFile::exists(QDir(dataDir).filePath(QLatin1String(versionsFile)));
}

void Companion::handleAppRelease(const QString &envelope)
{
    if(!versionedInstall())
        return; // a development folder; app updates need the install package's layout
    const QJsonObject e = parse(envelope);
    const QByteArray payload = e.value(QStringLiteral("payload")).toString().toUtf8();
    if(!cgfSignedByAny(publisherKeys, payload, e.value(QStringLiteral("sig")).toString().toLatin1()))
    {
        log(QStringLiteral("an app release with a wrong signature was refused"));
        return;
    }
    const QJsonObject p = QJsonDocument::fromJson(payload).object();
    const int seq = p.value(QStringLiteral("seq")).toInt();
    const QString folder = p.value(QStringLiteral("folder")).toString();
    const QJsonArray files = p.value(QStringLiteral("files")).toArray();
    if(p.value(QStringLiteral("kind")).toString() != QLatin1String("cgf-app") || p.value(QStringLiteral("v")).toInt() != 1
            || seq < 1 || !appFolderPattern().match(folder).hasMatch() || files.isEmpty()
            || (!church.isEmpty() && p.value(QStringLiteral("church")).toString() != church))
    {
        log(QStringLiteral("an app release this companion does not understand was ignored"));
        return;
    }

    QMap<QString, QString> state = readVersions();
    if(seq <= state.value(QStringLiteral("seq")).toInt())
        return; // already handled
    if(state.value(QStringLiteral("bad")).split(QLatin1Char(','), Qt::SkipEmptyParts).contains(folder))
        return; // it failed here before; only a newer release replaces it
    if(installing)
    {
        if(seq > job.seq)
            retryEnvelope = envelope; // after the current one
        return;
    }
    retryEnvelope = envelope;
    if(folder == state.value(QStringLiteral("current")))
    {
        // Installed from the package: only the release number was missing.
        state.insert(QStringLiteral("seq"), QString::number(seq));
        writeVersions(state);
        return;
    }

    job = Install();
    job.seq = seq;
    job.folder = folder;
    job.version = p.value(QStringLiteral("version")).toString();
    job.notes = p.value(QStringLiteral("notes")).toString();
    job.payload = p;
    job.files = files;
    const QJsonObject launcher = p.value(QStringLiteral("launcher")).toObject();
    if(!launcher.isEmpty())
        job.files.append(QJsonObject{{"launcher", true}, {"sha256", launcher.value(QStringLiteral("sha256"))},
                                     {"bytes", launcher.value(QStringLiteral("bytes"))}});
    job.partial = QDir(dataDir).filePath(folder + QStringLiteral(".partial"));
    QDir(job.partial).removeRecursively();
    QDir().mkpath(job.partial);
    installing = true;
    log(QStringLiteral("installing app %1 (release %2, %3 files)").arg(job.version).arg(seq).arg(files.size()));
    installNext();
}

void Companion::installNext()
{
    if(job.next >= job.files.size())
    {
        installDone();
        return;
    }
    const QJsonObject f = job.files.at(job.next).toObject();
    const QByteArray sha = f.value(QStringLiteral("sha256")).toString().toLatin1();
    const qint64 bytes = f.value(QStringLiteral("bytes")).toInteger(-1);
    const bool launcher = f.value(QStringLiteral("launcher")).toBool();
    const QString path = f.value(QStringLiteral("path")).toString();
    if(!launcher && !safeRelativePath(path))
    {
        installFailed(QStringLiteral("the release lists an unsafe path"));
        return;
    }
    const QString dest = launcher ? QDir(dataDir).filePath(QStringLiteral("CGFProjector.exe.new"))
                                  : QDir(job.partial).filePath(path);
    QDir().mkpath(QFileInfo(dest).absolutePath());

    // What this laptop already has is copied instead of downloaded.
    const QMap<QString, QString> state = readVersions();
    QStringList candidates;
    if(launcher)
        candidates << QDir(dataDir).filePath(QStringLiteral("CGFProjector.exe"));
    else
        for(const QString &key : {QStringLiteral("current"), QStringLiteral("previous")})
            if(!state.value(key).isEmpty())
                candidates << QDir(dataDir).filePath(state.value(key) + QLatin1Char('/') + path);
    for(const QString &src : candidates)
        if(QFileInfo(src).size() == bytes && fileSha256(src) == sha)
        {
            if(launcher || (QFile::remove(dest), QFile::copy(src, dest)))
            {
                if(!launcher)
                    ++job.copied;
                ++job.next;
                QTimer::singleShot(0, this, &Companion::installNext);
                return;
            }
        }

    QNetworkRequest request(QUrl(filesUrl + QString::fromLatin1(sha)));
    request.setRawHeader("Authorization", "Bearer " + token);
    request.setRawHeader("User-Agent", agent);
    request.setTransferTimeout(180000);
    QNetworkReply *reply = http.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, dest, sha, bytes, path, launcher] {
        reply->deleteLater();
        if(reply->error() != QNetworkReply::NoError)
        {
            installFailed(QStringLiteral("%1: %2").arg(launcher ? QStringLiteral("the launcher") : path,
                                                        reply->errorString()));
            return;
        }
        const QByteArray data = qUncompress(reply->readAll());
        if(data.size() != bytes || QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != sha)
        {
            installFailed(QStringLiteral("%1 did not match its hash").arg(path));
            return;
        }
        QSaveFile out(dest);
        if(!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
        {
            installFailed(QStringLiteral("cannot write %1").arg(dest));
            return;
        }
        ++job.downloaded;
        ++job.next;
        installNext();
    });
}

void Companion::installFailed(const QString &why)
{
    log(QStringLiteral("app %1 was not installed (%2); trying again in 10 minutes").arg(job.version, why));
    QDir(job.partial).removeRecursively();
    QFile::remove(QDir(dataDir).filePath(QStringLiteral("CGFProjector.exe.new")));
    installing = false;
    installRetry.start(installRetryMs);
}

void Companion::installDone()
{
    // The release notes, for the app's "What's new" and its title bar.
    {
        QJsonObject release{{"version", job.version}, {"folder", job.folder}, {"seq", job.seq},
                            {"notes", job.notes}, {"published_at", job.payload.value(QStringLiteral("published_at"))}};
        QSaveFile out(QDir(job.partial).filePath(QStringLiteral("cgf-release.json")));
        if(!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(release).toJson()) < 0 || !out.commit())
        {
            installFailed(QStringLiteral("cannot write the release notes"));
            return;
        }
    }
    QMap<QString, QString> state = readVersions();
    const QString current = state.value(QStringLiteral("current"));
    const QString target = QDir(dataDir).filePath(job.folder);
    if(job.folder != current)
        QDir(target).removeRecursively(); // a leftover of an earlier attempt
    if(!QDir().rename(job.partial, target))
    {
        installFailed(QStringLiteral("cannot rename the new folder"));
        return;
    }

    // The launcher is not running between starts, so it can simply be replaced.
    const QString launcher = QDir(dataDir).filePath(QStringLiteral("CGFProjector.exe"));
    const QString fresh = launcher + QStringLiteral(".new");
    if(QFile::exists(fresh))
    {
        const QString old = launcher + QStringLiteral(".old");
        QFile::remove(old);
        const bool hadOne = QFile::exists(launcher);
        if((!hadOne || QFile::rename(launcher, old)) && QFile::rename(fresh, launcher))
            log(hadOne ? QStringLiteral("the launcher was updated") : QStringLiteral("the launcher was added"));
        else
        {
            if(hadOne && !QFile::exists(launcher))
                QFile::rename(old, launcher);
            log(QStringLiteral("the launcher could not be replaced; the old one stays"));
        }
    }

    if(!current.isEmpty() && current != job.folder)
        state.insert(QStringLiteral("previous"), current);
    state.insert(QStringLiteral("current"), job.folder);
    state.insert(QStringLiteral("seq"), QString::number(job.seq));
    state.insert(QStringLiteral("trial"), QStringLiteral("1"));
    state.insert(QStringLiteral("starts"), QStringLiteral("0"));
    if(!writeVersions(state))
    {
        installFailed(QStringLiteral("cannot write cgf-versions.txt"));
        return;
    }
    installing = false;
    log(QStringLiteral("app %1 is installed (%2 files downloaded, %3 copied); CGF Projector opens it from its next start")
        .arg(job.version).arg(job.downloaded).arg(job.copied));

    // Older folders go; one that is still running stays until next time.
    const QStringList keep = {state.value(QStringLiteral("current")), state.value(QStringLiteral("previous"))};
    for(const QString &name : QDir(dataDir).entryList({QStringLiteral("app-*")}, QDir::Dirs | QDir::NoDotAndDotDot))
        if(!keep.contains(name) && !QDir(QDir(dataDir).filePath(name)).removeRecursively())
            log(QStringLiteral("%1 is still in use; it is removed later").arg(name));

    if(serverOpen)
        server.sendTextMessage(compact({{"type", "status"}, {"app", job.folder}}));
    announceUpdate();

    // A newer release that arrived meanwhile.
    const QJsonObject later = parse(parse(retryEnvelope).value(QStringLiteral("payload")).toString());
    if(later.value(QStringLiteral("seq")).toInt() > job.seq)
    {
        handleAppRelease(retryEnvelope);
        return;
    }
    // Run the new companion too (when this one runs on its own, from sign-in).
    // If that cannot start, this one keeps running: updates must not stop.
    if(stay && QFileInfo(QCoreApplication::applicationDirPath()).fileName() != job.folder)
    {
        if(QFile::exists(launcher)
                && QProcess::startDetached(launcher, {QStringLiteral("--companion"), QStringLiteral("--after"),
                                                      QString::number(QCoreApplication::applicationPid())}))
            stop(QStringLiteral("restarting on the new version"));
        else
            log(QStringLiteral("cannot start the new companion; this one keeps running"));
    }
}

// Tell the open app (if it runs an older folder) that a new version is ready.
void Companion::announceUpdate()
{
    if(!bridgeOpen || appFolder.isEmpty())
        return;
    const QString current = readVersions().value(QStringLiteral("current"));
    if(current.isEmpty() || current == appFolder)
        return;
    QFile f(QDir(dataDir).filePath(current + QStringLiteral("/cgf-release.json")));
    QJsonObject release;
    if(f.open(QIODevice::ReadOnly))
        release = QJsonDocument::fromJson(f.readAll()).object();
    log(QStringLiteral("told the projector that %1 is ready").arg(current));
    bridge.sendTextMessage(compact({{"type", "appUpdate"}, {"folder", current},
                                    {"version", release.value(QStringLiteral("version")).toString(current.mid(4))},
                                    {"notes", release.value(QStringLiteral("notes")).toString()}}));
}
