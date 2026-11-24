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

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QNetworkProxyFactory>

// Started by CGF Projector: cgf-companion --config <cgf-cloud.json> --bridge-port <port>
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("cgf-companion"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Relays CGF library releases to CGF Projector"));
    const QCommandLineOption config(QStringLiteral("config"), QStringLiteral("cgf-cloud.json"), QStringLiteral("file"));
    const QCommandLineOption port(QStringLiteral("bridge-port"), QStringLiteral("The app's bridge port"),
                                  QStringLiteral("port"), QStringLiteral("8770"));
    parser.addOptions({config, port});
    parser.addHelpOption();
    parser.process(app);
    if(!parser.isSet(config))
        return 2;
    const quint16 bridgePort = parser.value(port).toUShort();

    // One companion per app: after a restart of the app the old one is still
    // here for up to a minute and serves the new app instead.
    QLockFile lock(QDir::temp().filePath(QStringLiteral("cgf-companion-%1.lock").arg(bridgePort)));
    if(!lock.tryLock(0))
        return 0;

    // A church network may need a proxy for the internet.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    Companion companion(parser.value(config), bridgePort);
    if(!companion.start())
        return 2;
    return app.exec();
}
