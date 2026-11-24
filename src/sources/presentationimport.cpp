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

#include "../headers/presentationimport.hpp"
#include "../headers/applog.hpp"
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QTimer>

QString PresentationImport::fileFilter()
{
    return QObject::tr("PowerPoint or PDF (*.pptx *.ppt *.pptm *.ppsx *.pps *.odp *.pdf)");
}

QList<SlideShowItem> PresentationImport::run(const QString &file, const SlideShowSettings &sets,
                                             QWidget *parent, QString &error)
{
    QList<SlideShowItem> slides;
    error.clear();

    QTemporaryDir work;
    const QString script = work.filePath("import-presentation.ps1");
    if(!work.isValid() || !QFile::copy(":/scripts/scripts/import-presentation.ps1", script))
    {
        error = QObject::tr("Could not create a temporary folder for the import.");
        return slides;
    }
    // Copies of resources are read-only, which would keep the folder from
    // being deleted afterwards.
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner);

    // Draw slides at the size Pictures stores them (Settings > Pictures), so
    // they are not scaled down afterwards, which blurs text.
    const int box = sets.resize ? sets.boundWidth : 1920;

    QProcess ps;
    // Full path, so a powershell.exe elsewhere on PATH is never run instead.
    ps.setProgram(QDir(qEnvironmentVariable("SystemRoot"))
                  .filePath("System32/WindowsPowerShell/v1.0/powershell.exe"));
    ps.setArguments({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                     "-File", QDir::toNativeSeparators(script),
                     "-In", QDir::toNativeSeparators(file),
                     "-Out", QDir::toNativeSeparators(work.path()),
                     "-Size", QString::number(box)});

    QProgressDialog progress(QObject::tr("Importing %1...").arg(QFileInfo(file).fileName()),
                             QObject::tr("Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoReset(false);
    progress.setAutoClose(false);
    progress.setValue(0);

    // The script prints "count N" and then "slide I" per finished slide.
    QByteArray pending;
    QObject::connect(&ps, &QProcess::readyReadStandardOutput, &progress, [&] {
        pending += ps.readAllStandardOutput();
        int end;
        while((end = pending.indexOf('\n')) >= 0)
        {
            const QString line = QString::fromUtf8(pending.left(end)).remove(QChar(0xFEFF)).trimmed();
            pending.remove(0, end + 1);
            if(line.startsWith("count "))
                progress.setMaximum(line.mid(6).toInt());
            else if(line.startsWith("slide "))
                progress.setValue(line.mid(6).toInt());
        }
    });
    // Cancel asks the script to stop after the current slide, so it closes
    // PowerPoint's copy of the file; killing it would leave PowerPoint running
    // unseen. Only if it hangs is it stopped hard.
    QTimer stopTimer;
    stopTimer.setSingleShot(true);
    QObject::connect(&stopTimer, &QTimer::timeout, &ps, &QProcess::kill);
    QObject::connect(&progress, &QProgressDialog::canceled, &ps, [&] {
        QFile(QDir(work.path()).filePath("cancel")).open(QIODevice::WriteOnly);
        stopTimer.start(15000);
    });

    // Wait in an event loop, not with waitForFinished(): the projector keeps
    // drawing and a live video keeps playing during the import.
    QEventLoop loop;
    QObject::connect(&ps, &QProcess::finished, &loop, &QEventLoop::quit);
    ps.start();
    if(!ps.waitForStarted(10000))
    {
        error = QObject::tr("Could not start PowerShell: %1").arg(ps.errorString());
        AppLog::write("Import of " + file + " failed: " + error);
        return slides;
    }
    loop.exec();

    if(progress.wasCanceled())
    {
        AppLog::write("Import of " + file + " cancelled");
        return slides;
    }
    if(ps.exitStatus() != QProcess::NormalExit || ps.exitCode() != 0)
    {
        // The script's own message (often a bare COM error code) goes to the log.
        const QString detail = QString::fromUtf8(ps.readAllStandardError()).remove(QChar(0xFEFF)).simplified();
        if(ps.exitStatus() == QProcess::NormalExit && ps.exitCode() == 2)
            error = QObject::tr("Importing PowerPoint files needs PowerPoint on this computer. "
                                "PDF files import without it.");
        else
            error = QObject::tr("Could not import %1. The file may be damaged or protected by a password.")
                    .arg(QFileInfo(file).fileName());
        AppLog::write("Import of " + file + " failed: " + (detail.isEmpty() ? QString("no message") : detail));
        return slides;
    }

    const QDir out(work.path());
    for(const QString &name : out.entryList({"slide*.png"}, QDir::Files, QDir::Name))
    {
        QPixmap img;
        if(!img.load(out.filePath(name)))
            continue;
        // PDF pages come out larger on displays scaled above 100 %.
        if(img.width() > box || img.height() > box)
            img = img.scaled(box, box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        SlideShowItem slide;
        slide.image = img;
        slide.imagePreview = img.scaled(400, 400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        slide.imageSmall = img.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        slide.name = QObject::tr("Slide %1").arg(slides.count() + 1);
        slide.path = file;
        slides.append(slide);
    }
    if(slides.isEmpty())
        error = QObject::tr("%1 has no slides to show.").arg(QFileInfo(file).fileName());
    AppLog::write(QString("Imported %1: %2 slides").arg(file).arg(slides.count()));
    return slides;
}
