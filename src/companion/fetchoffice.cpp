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

#include "fetchoffice.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStorageInfo>
#include <QUrl>
#include <cstdio>

namespace
{
// LibreOffice 26.2.7 for 64-bit Windows, as published by The Document
// Foundation. To move to a newer version, change the three values together;
// the SHA-256 is on the download server beside the file (<file>.sha256).
// "stable" keeps only current versions; the archive keeps every version.
const char *const officeUrls[] = {
    "https://download.documentfoundation.org/libreoffice/stable/26.2.7/win/x86_64/LibreOffice_26.2.7_Win_x86-64.msi",
    "https://downloadarchive.documentfoundation.org/libreoffice/old/26.2.7.3/win/x86_64/LibreOffice_26.2.7.3_Win_x86-64.msi",
};
const char *const officeSha256 = "480fecf759fc6393bd614fbf43753eb95a8198fc1a065d41b727e3b664cd1e59";
// The download plus the unpacked files, with room to spare.
const qint64 neededBytes = 2500LL * 1000 * 1000;

void say(const QString &line)
{
    std::fprintf(stdout, "%s\n", qUtf8Printable(line));
    std::fflush(stdout);
}

void complain(const QString &line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    std::fflush(stderr);
}

// 0 done, 3 could not download, 4 not the file this program expects.
int download(const QUrl &url, const QString &target)
{
    QFile out(target);
    if(!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        complain(QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
        return 3;
    }
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000); // a stalled connection, not the whole download
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("cgf-companion"));
    QNetworkReply *reply = manager.get(request);

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QElapsedTimer sinceReport;
    sinceReport.start();
    qint64 received = 0;
    bool writeFailed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        const QByteArray chunk = reply->readAll();
        hash.addData(chunk);
        if(out.write(chunk) != chunk.size())
        {
            writeFailed = true;
            reply->abort();
            return;
        }
        received += chunk.size();
        const qint64 total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if(sinceReport.elapsed() >= 250 && total > 0)
        {
            say(QStringLiteral("progress %1 %2").arg(received).arg(total));
            sinceReport.restart();
        }
    });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    out.close();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError failure = reply->error();
    const QString reason = reply->errorString();
    reply->deleteLater();
    if(writeFailed)
    {
        complain(QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
        return 3;
    }
    if(failure != QNetworkReply::NoError || status != 200)
    {
        complain(QStringLiteral("%1: %2 (HTTP %3)").arg(url.host(), reason).arg(status));
        return 3;
    }
    if(hash.result().toHex() != officeSha256)
    {
        complain(QStringLiteral("%1: the file is not LibreOffice as published (SHA-256 %2)")
                 .arg(url.host(), QString::fromLatin1(hash.result().toHex())));
        return 4;
    }
    say(QStringLiteral("progress %1 %1").arg(received));
    return 0;
}
}

int fetchLibreOffice(const QString &dataDir)
{
    const QDir data(dataDir);
    const QString target = data.filePath(QStringLiteral("libreoffice"));
    if(QFileInfo(target + QStringLiteral("/program/soffice.exe")).isFile())
    {
        say(QStringLiteral("done"));
        return 0;
    }
    if(!data.exists())
    {
        complain(QStringLiteral("no folder %1").arg(dataDir));
        return 5;
    }

    // One download per install folder at a time.
    const QByteArray key = QCryptographicHash::hash(data.absolutePath().toLower().toUtf8(), QCryptographicHash::Sha1)
            .toHex().left(12);
    QLockFile lock(QDir::temp().filePath(QStringLiteral("cgf-fetch-office-%1.lock").arg(QString::fromLatin1(key))));
    if(!lock.tryLock(0))
    {
        complain(QStringLiteral("another download is running"));
        return 1;
    }

    // Left-overs of a download that was cancelled or failed.
    const QString work = data.filePath(QStringLiteral("libreoffice-download"));
    const QString fresh = data.filePath(QStringLiteral("libreoffice.new"));
    QDir(work).removeRecursively();
    QDir(fresh).removeRecursively();
    QDir(target).removeRecursively();

    if(QStorageInfo(data.absolutePath()).bytesAvailable() < neededBytes)
    {
        complain(QStringLiteral("less than 2.5 GB free on %1").arg(QStorageInfo(data.absolutePath()).rootPath()));
        return 6;
    }
    if(!QDir().mkpath(work))
    {
        complain(QStringLiteral("cannot create %1").arg(work));
        return 5;
    }

    // CGF_LIBREOFFICE_URL: another place to get the same file from (the
    // checks serve it from this computer). The SHA-256 above still decides.
    QList<QUrl> urls;
    if(!qEnvironmentVariableIsEmpty("CGF_LIBREOFFICE_URL"))
        urls << QUrl(qEnvironmentVariable("CGF_LIBREOFFICE_URL"));
    else
        for(const char *u : officeUrls)
            urls << QUrl(QString::fromLatin1(u));

    const QString msi = QDir(work).filePath(QStringLiteral("LibreOffice.msi"));
    int result = 3;
    for(const QUrl &url : std::as_const(urls))
    {
        result = download(url, msi);
        if(result == 0)
            break;
    }
    if(result != 0)
    {
        QDir(work).removeRecursively();
        return result;
    }

    // Unpack only: msiexec /a writes the files to TARGETDIR and changes
    // nothing else on the computer. Its own quoting rules need the command
    // line written out (a property's value is quoted, not the whole word).
    say(QStringLiteral("unpacking"));
    QProcess installer;
    installer.setProgram(QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/msiexec.exe")));
    installer.setNativeArguments(QStringLiteral("/a \"%1\" /qn TARGETDIR=\"%2\"")
                                 .arg(QDir::toNativeSeparators(msi), QDir::toNativeSeparators(fresh)));
    installer.start();
    if(!installer.waitForFinished(-1) || installer.exitStatus() != QProcess::NormalExit || installer.exitCode() != 0
            || !QFileInfo(fresh + QStringLiteral("/program/soffice.exe")).isFile())
    {
        complain(QStringLiteral("Windows Installer could not unpack LibreOffice (exit code %1)").arg(installer.exitCode()));
        QDir(fresh).removeRecursively();
        QDir(work).removeRecursively();
        return 5;
    }

    // Not needed to convert files, and more than half of the 1.9 GB: the
    // installer's copy of the download, the help and readme files, the
    // spelling dictionaries, the menus' translations and all icon sets but
    // one. Slides come out pixel for pixel the same without them.
    const QDir unpacked(fresh);
    for(const QString &name : unpacked.entryList({QStringLiteral("*.msi")}, QDir::Files))
        QFile::remove(unpacked.filePath(name));
    QDir(unpacked.filePath(QStringLiteral("help"))).removeRecursively();
    QDir(unpacked.filePath(QStringLiteral("readmes"))).removeRecursively();
    QDir(unpacked.filePath(QStringLiteral("program/resource"))).removeRecursively();
    const QDir extensions(unpacked.filePath(QStringLiteral("share/extensions")));
    for(const QString &name : extensions.entryList({QStringLiteral("dict-*")}, QDir::Dirs | QDir::NoDotAndDotDot))
        QDir(extensions.filePath(name)).removeRecursively();
    const QDir icons(unpacked.filePath(QStringLiteral("share/config")));
    for(const QString &name : icons.entryList({QStringLiteral("images_*.zip")}, QDir::Files))
        if(name != QStringLiteral("images_colibre.zip"))
            QFile::remove(icons.filePath(name));

    if(!QDir().rename(fresh, target))
    {
        complain(QStringLiteral("cannot rename %1 to %2").arg(fresh, target));
        QDir(fresh).removeRecursively();
        QDir(work).removeRecursively();
        return 5;
    }
    QDir(work).removeRecursively();
    say(QStringLiteral("done"));
    return 0;
}
