/***************************************************************************
//
//    CGF Projector - based on softProjector, an open source media projection software
//    Copyright (C) 2026  CGF Projector contributors
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

#include "../headers/applog.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMutex>
#include <QScreen>

namespace
{
QFile logFile;
QMutex logMutex;
QtMessageHandler previousHandler = nullptr;
qint64 sessionBytes = 0;
const qint64 rotateSize = 1024 * 1024;
// A warning repeated in a loop must not fill the disk.
const qint64 sessionLimit = 4 * 1024 * 1024;
thread_local bool inHandler = false;

void append(const QString &text)
{
    QMutexLocker lock(&logMutex);
    if(!logFile.isOpen() || sessionBytes > sessionLimit)
        return;
    const QByteArray line = (QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                             + "  " + text + "\n").toUtf8();
    sessionBytes += logFile.write(line);
    logFile.flush();
}

void handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Debug output is for developers; warnings and console.info() are kept.
    if(type != QtDebugMsg && !inHandler)
    {
        inHandler = true;
        const char *kind = type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warning" : "error";
        append(QString("%1: %2").arg(kind, msg));
        inHandler = false;
    }
    if(previousHandler)
        previousHandler(type, context, msg);
}
}

void AppLog::start(const QString &dir)
{
    const QString path = QDir(dir).filePath("CGFProjector.log");
    if(QFileInfo(path).size() > rotateSize)
    {
        const QString previous = QDir(dir).filePath("CGFProjector-previous.log");
        QFile::remove(previous);
        QFile::rename(path, previous);
    }
    logFile.setFileName(path);
    if(!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    previousHandler = qInstallMessageHandler(handleMessage);
    append(QString("---- CGF Projector %1 started (Qt %2)")
           .arg(QCoreApplication::applicationVersion(), qVersion()));
}

void AppLog::write(const QString &text)
{
    append(text);
}

QString AppLog::describe(const QScreen *screen)
{
    if(!screen)
        return "none";
    QString name = screen->name();
    const QString model = (screen->manufacturer() + " " + screen->model()).simplified();
    if(!model.isEmpty())
        name += " (" + model + ")";
    const QRect g = screen->geometry();
    return QString("%1 %2x%3 at %4,%5, %6 Hz, scale %7%8")
            .arg(name).arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y())
            .arg(qRound(screen->refreshRate())).arg(screen->devicePixelRatio())
            .arg(screen == QGuiApplication::primaryScreen() ? ", primary" : "");
}
