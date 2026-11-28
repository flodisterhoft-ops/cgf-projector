/***************************************************************************
//
//    CGF Projector - app updates installed by the companion
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

// The main window's side of app updates (docs/song-push.md, "App updates").
// The companion installs a new version into its own app-<version> folder; the
// launcher starts it from the next start on. Here: mark this version as
// running fine (so the launcher keeps it), and at the first start after the
// program or the library changed, show the What's new window (whatsnew.hpp).
// Nobody is asked to restart: the new version simply opens next time.

#include "../headers/softprojector.hpp"
#include "../headers/librarypush.hpp"
#include "../headers/livebridge.hpp"
#include "../headers/whatsnew.hpp"
#include "ui_softprojector.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QTimer>

namespace {

QString dataDir()
{
    return QFileInfo(QSqlDatabase::database().databaseName()).absolutePath();
}

// The install package's layout: <install>\CGFProjector.exe (the launcher) and
// <install>\app-<version>\CGFProjector.exe (this program).
bool versionedLayout()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    QDir parent = appDir;
    return appDir.dirName().startsWith(QLatin1String("app-")) && parent.cdUp()
            && QFileInfo(parent.absolutePath()) == QFileInfo(dataDir())
            && QFile::exists(parent.filePath(QStringLiteral("CGFProjector.exe")));
}

// The version this laptop ran before (cgf-versions.txt, written by the
// launcher and the companion): "0.2.6" for "previous=app-0.2.6".
QString previousAppVersion()
{
    QFile f(QDir(dataDir()).filePath(QStringLiteral("cgf-versions.txt")));
    if(!f.open(QIODevice::ReadOnly))
        return QString();
    for(const QByteArray &raw : f.readAll().split('\n'))
    {
        const QString line = QString::fromUtf8(raw).trimmed();
        if(line.startsWith(QLatin1String("previous=app-")))
            return line.mid(13);
    }
    return QString();
}

} // namespace

void SoftProjector::setupAppUpdates()
{
    // A new version is on trial until it has run a full minute; the launcher
    // brings the previous one back if it never does.
    const int healthyMs = qEnvironmentVariableIsSet("CGF_HEALTHY_MS")
            ? qEnvironmentVariableIntValue("CGF_HEALTHY_MS") : 60000;
    QTimer::singleShot(healthyMs, this, &SoftProjector::markHealthy);

    QAction *whatsNew = new QAction(tr("What's new"), this);
    ui->menuHelp->insertAction(ui->actionAbout, whatsNew);
    connect(whatsNew, &QAction::triggered, this, [this] { showWhatsNew(false); });
    // On a laptop installed from the package, once the main window is up.
    if(versionedLayout() || qEnvironmentVariableIsSet("CGF_WHATS_NEW"))
        QTimer::singleShot(1500, this, [this] { showWhatsNew(true); });

    if(liveBridge)
        connect(liveBridge, &LiveBridge::appUpdateReady, this, &SoftProjector::showAppUpdate);
}

void SoftProjector::showWhatsNew(bool onlyIfNew)
{
    const int library = libraryPush ? libraryPush->version() : 0;
    QJsonObject content;
    if(onlyIfNew)
    {
        content = WhatsNew::pending(dataDir(), previousAppVersion(), library);
        if(content.isEmpty())
            return;
        WhatsNew::markShown(dataDir(), content);
        qInfo("What's new: shown for %s", qPrintable(content.value(QStringLiteral("app")).toString()));
    }
    else
        content = WhatsNew::lastShown(dataDir(), library);
    auto *dialog = new WhatsNewDialog(content, this);
    dialog->open(); // not exec(): nothing waits for it
}

void SoftProjector::markHealthy()
{
    if(!versionedLayout())
        return;
    QSaveFile out(QDir(dataDir()).filePath(QStringLiteral("cgf-healthy.txt")));
    if(out.open(QIODevice::WriteOnly))
    {
        out.write(QDir(QCoreApplication::applicationDirPath()).dirName().toUtf8() + '\n');
        out.commit();
    }
}

void SoftProjector::showAppUpdate(const QJsonObject &update)
{
    // The companion installed a newer version while this one runs. Nothing to
    // do here: the next start opens it and shows what changed.
    qInfo("App update %s is ready; it opens at the next start",
          qPrintable(update.value(QStringLiteral("version")).toString()));
}

// From closeEvent, once it is decided whether the window closes.
void SoftProjector::finishClose(bool accepted)
{
    if(accepted)
        markHealthy(); // a clean close counts as a good run
}
