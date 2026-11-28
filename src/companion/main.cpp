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
#include "fetchoffice.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkProxyFactory>

//   cgf-companion --config <cgf-cloud.json> [--data-dir <install folder>]
//                 [--bridge-port <port>] [--stay]
//   cgf-companion --fetch-libreoffice --data-dir <install folder>
// The app starts it with --config and --bridge-port; the launcher starts it at
// Windows sign-in with --data-dir and --stay (it then keeps running). With
// --fetch-libreoffice it downloads LibreOffice for the app's PowerPoint
// import and leaves (fetchoffice.hpp); it needs no cgf-cloud.json for that.
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("cgf-companion"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Relays CGF releases to CGF Projector"));
    const QCommandLineOption config(QStringLiteral("config"), QStringLiteral("cgf-cloud.json"), QStringLiteral("file"));
    const QCommandLineOption data(QStringLiteral("data-dir"), QStringLiteral("The install folder"), QStringLiteral("dir"));
    const QCommandLineOption port(QStringLiteral("bridge-port"), QStringLiteral("The app's bridge port"),
                                  QStringLiteral("port"));
    const QCommandLineOption stay(QStringLiteral("stay"), QStringLiteral("Keep running while the app is closed"));
    const QCommandLineOption office(QStringLiteral("fetch-libreoffice"),
                                    QStringLiteral("Download LibreOffice into the install folder, then leave"));
    parser.addOptions({config, data, port, stay, office});
    parser.addHelpOption();
    parser.process(app);
    if(parser.isSet(office))
    {
        if(!parser.isSet(data))
            return 2;
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        return fetchLibreOffice(QDir(parser.value(data)).absolutePath());
    }
    if(!parser.isSet(config))
        return 2;
    const QString dataDir = QDir(parser.isSet(data) ? parser.value(data)
                                                    : QFileInfo(parser.value(config)).absolutePath()).absolutePath();

    // The bridge port: as given, else as the app's cgf-network.json sets it.
    quint16 bridgePort = 8770;
    if(parser.isSet(port))
        bridgePort = parser.value(port).toUShort();
    else
    {
        QFile f(QDir(dataDir).filePath(QStringLiteral("cgf-network.json")));
        if(f.open(QIODevice::ReadOnly))
        {
            const int p = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("port")).toInt(8770);
            if(p > 0 && p < 65536)
                bridgePort = quint16(p);
        }
    }

    // One companion per install: a second one (the app starting it while the
    // sign-in one runs, or an old one about to restart) leaves at once.
    const QByteArray key = QCryptographicHash::hash(dataDir.toLower().toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    QLockFile lock(QDir::temp().filePath(QStringLiteral("cgf-companion-%1.lock").arg(QString::fromLatin1(key))));
    if(!lock.tryLock(0))
        return 0;

    // A church network may need a proxy for the internet.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    Companion companion(parser.value(config), dataDir, bridgePort, parser.isSet(stay));
    if(!companion.start())
        return 2;
    return app.exec();
}
