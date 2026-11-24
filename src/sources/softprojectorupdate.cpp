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
// running fine (so the launcher keeps it), offer a restart when a newer one is
// ready, and show the release notes.

#include "../headers/softprojector.hpp"
#include "../headers/livebridge.hpp"
#include "ui_softprojector.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QStatusBar>
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

QJsonObject releaseInfo(const QString &folder)
{
    QFile f(QDir(folder).filePath(QStringLiteral("cgf-release.json")));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

} // namespace

void SoftProjector::setupAppUpdates()
{
    // A new version is on trial until it has run a full minute; the launcher
    // brings the previous one back if it never does.
    const int healthyMs = qEnvironmentVariableIsSet("CGF_HEALTHY_MS")
            ? qEnvironmentVariableIntValue("CGF_HEALTHY_MS") : 60000;
    QTimer::singleShot(healthyMs, this, &SoftProjector::markHealthy);

    const QJsonObject installed = releaseInfo(QCoreApplication::applicationDirPath());
    if(!installed.isEmpty())
    {
        QAction *notes = new QAction(tr("What's new"), this);
        ui->menuHelp->insertAction(ui->actionAbout, notes);
        connect(notes, &QAction::triggered, this, [this, installed] {
            const QString text = installed.value(QStringLiteral("notes")).toString();
            QMessageBox::information(this, tr("What's new in %1").arg(installed.value(QStringLiteral("version")).toString()),
                                     text.isEmpty() ? tr("No notes for this version.") : text);
        });
    }
    if(liveBridge)
        connect(liveBridge, &LiveBridge::appUpdateReady, this, &SoftProjector::showAppUpdate);
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
    if(!versionedLayout())
        return;
    pendingUpdateVersion = update.value(QStringLiteral("version")).toString();
    pendingUpdateNotes = update.value(QStringLiteral("notes")).toString();
    qInfo("App update %s is ready (restart offered)", qPrintable(pendingUpdateVersion));
    if(!updateBar)
    {
        updateBar = new QWidget(this);
        auto *row = new QHBoxLayout(updateBar);
        row->setContentsMargins(6, 0, 6, 0);
        updateLabel = new QLabel(updateBar);
        auto *notes = new QPushButton(tr("What's new"), updateBar);
        updateRestart = new QPushButton(tr("Restart now"), updateBar);
        row->addWidget(updateLabel);
        row->addWidget(notes);
        row->addWidget(updateRestart);
        statusBar()->addPermanentWidget(updateBar);
        connect(notes, &QPushButton::clicked, this, [this] {
            QMessageBox::information(this, tr("What's new in %1").arg(pendingUpdateVersion),
                                     pendingUpdateNotes.isEmpty() ? tr("No notes for this version.") : pendingUpdateNotes);
        });
        connect(updateRestart, &QPushButton::clicked, this, &SoftProjector::restartForUpdate);
    }
    updateLabel->setText(tr("CGF Projector %1 is ready").arg(pendingUpdateVersion));
    updateBar->show();
    refreshUpdateBar();
}

void SoftProjector::refreshUpdateBar()
{
    if(!updateRestart)
        return;
    // Never while something is on the projector: a restart would blank it.
    const bool live = showing && (isSingleScreen || ui->actionCloseDisplay->isChecked());
    updateRestart->setEnabled(!live);
}

void SoftProjector::restartForUpdate()
{
    restartRequested = true;
    close(); // asks about an unsaved schedule, and may be cancelled
}

// From closeEvent, once it is decided whether the window closes.
void SoftProjector::finishClose(bool accepted)
{
    if(!accepted)
    {
        restartRequested = false;
        return;
    }
    markHealthy(); // a clean close counts as a good run
    if(restartRequested && versionedLayout())
        QProcess::startDetached(QDir(dataDir()).filePath(QStringLiteral("CGFProjector.exe")),
                                {QStringLiteral("--after"), QString::number(QCoreApplication::applicationPid())});
}
