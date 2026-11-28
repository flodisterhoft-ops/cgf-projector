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
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTimer>

namespace
{
// The data folder: where the database is. Updates replace the program's own
// folder, so LibreOffice and its settings live here.
QDir dataDir()
{
    return QFileInfo(QSqlDatabase::database().databaseName()).absoluteDir();
}

QString companionExe()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("cgf-companion.exe");
}

QList<SlideShowItem> convert(const QString &file, const SlideShowSettings &sets,
                             QWidget *parent, QString &error, bool &needsOffice);
}

std::function<bool(QWidget *)> PresentationImport::confirmDownload = [](QWidget *parent) {
    QMessageBox box(QMessageBox::Question, QObject::tr("Import PowerPoint or PDF"),
                    QObject::tr("This computer has no PowerPoint. CGF Projector can open PowerPoint files "
                                "with LibreOffice, a free office program.\n\n"
                                "Download LibreOffice now? It is about 375 MB and needs the internet this once."),
                    QMessageBox::NoButton, parent);
    QPushButton *download = box.addButton(QObject::tr("Download"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == download;
};

QString PresentationImport::findLibreOffice()
{
    // CGF_LIBREOFFICE names a soffice.exe anywhere else (checks, odd installs).
    QStringList candidates;
    if(!qEnvironmentVariableIsEmpty("CGF_LIBREOFFICE"))
        candidates << qEnvironmentVariable("CGF_LIBREOFFICE");
    candidates << dataDir().filePath("libreoffice/program/soffice.exe");
    for(const QSettings::Format view : {QSettings::Registry64Format, QSettings::Registry32Format})
    {
        const QString dir = QSettings("HKEY_LOCAL_MACHINE\\SOFTWARE\\LibreOffice\\UNO\\InstallPath", view)
                .value(".").toString();
        if(!dir.isEmpty())
            candidates << QDir(dir).filePath("soffice.exe");
    }
    for(const char *var : {"ProgramFiles", "ProgramFiles(x86)"})
    {
        if(!qEnvironmentVariableIsEmpty(var))
            candidates << QDir(qEnvironmentVariable(var)).filePath("LibreOffice/program/soffice.exe");
    }
    for(const QString &c : std::as_const(candidates))
    {
        if(QFileInfo(c).isFile())
            return QDir::toNativeSeparators(QFileInfo(c).absoluteFilePath());
    }
    return QString();
}

bool PresentationImport::fetchLibreOffice(QWidget *parent, QString &error)
{
    error.clear();
    QProcess ps;
    ps.setProgram(companionExe());
    ps.setArguments({"--fetch-libreoffice", "--data-dir", QDir::toNativeSeparators(dataDir().absolutePath())});

    QProgressDialog progress(QObject::tr("Downloading LibreOffice..."), QObject::tr("Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoReset(false);
    progress.setAutoClose(false);
    progress.setValue(0);

    // The companion prints "progress <received> <total>" while it downloads,
    // then "unpacking". Unpacking is Windows Installer's work and cannot be
    // stopped half way, so Cancel goes away for it.
    QByteArray pending;
    QObject::connect(&ps, &QProcess::readyReadStandardOutput, &progress, [&] {
        pending += ps.readAllStandardOutput();
        int end;
        while((end = pending.indexOf('\n')) >= 0)
        {
            const QStringList words = QString::fromUtf8(pending.left(end)).trimmed().split(' ');
            pending.remove(0, end + 1);
            if(words.value(0) == "progress" && words.value(2).toLongLong() > 0)
            {
                const qint64 got = words.value(1).toLongLong(), all = words.value(2).toLongLong();
                progress.setMaximum(1000);
                progress.setValue(int(got * 1000 / all));
                progress.setLabelText(QObject::tr("Downloading LibreOffice... %1 of %2 MB")
                                      .arg(got / 1000000).arg(all / 1000000));
            }
            else if(words.value(0) == "unpacking")
            {
                progress.setCancelButton(nullptr);
                progress.setLabelText(QObject::tr("Unpacking LibreOffice..."));
                progress.setMaximum(0);
                progress.setValue(0);
            }
        }
    });
    bool cancelled = false;
    QObject::connect(&progress, &QProgressDialog::canceled, &ps, [&] {
        cancelled = true;
        ps.kill();
    });

    QEventLoop loop;
    QObject::connect(&ps, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&ps, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError e) {
        if(e == QProcess::FailedToStart)
            loop.quit();
    });
    AppLog::write("LibreOffice: asking the companion to download it");
    ps.start();
    if(ps.state() != QProcess::NotRunning)
        loop.exec();
    // hide(), not close(): closing a progress dialog counts as Cancel.
    progress.hide();

    if(cancelled)
    {
        AppLog::write("LibreOffice: download cancelled");
        return false;
    }
    const QString detail = QString::fromUtf8(ps.readAllStandardError()).simplified();
    if(ps.error() == QProcess::FailedToStart || ps.exitStatus() != QProcess::NormalExit || ps.exitCode() != 0)
    {
        // The companion's exit codes: 3 no download, 4 damaged, 5 could not
        // unpack, 6 disk full.
        switch(ps.error() == QProcess::FailedToStart ? -1 : ps.exitCode())
        {
        case 3: error = QObject::tr("LibreOffice could not be downloaded. Check the internet connection and try again."); break;
        case 4: error = QObject::tr("The LibreOffice download arrived damaged. Please try again."); break;
        case 6: error = QObject::tr("LibreOffice needs about 2.5 GB of free disk space while it is set up."); break;
        default: error = QObject::tr("LibreOffice could not be set up on this computer."); break;
        }
        AppLog::write("LibreOffice: download failed: " + (detail.isEmpty() ? ps.errorString() : detail));
        return false;
    }
    AppLog::write("LibreOffice: ready in " + dataDir().filePath("libreoffice"));
    return true;
}

QString PresentationImport::fileFilter()
{
    return QObject::tr("PowerPoint or PDF (*.pptx *.ppt *.pptm *.ppsx *.pps *.odp *.pdf)");
}

QList<SlideShowItem> PresentationImport::run(const QString &file, const SlideShowSettings &sets,
                                             QWidget *parent, QString &error)
{
    bool needsOffice = false;
    QList<SlideShowItem> slides = convert(file, sets, parent, error, needsOffice);
    if(!needsOffice)
        return slides;

    // Neither PowerPoint nor LibreOffice. LibreOffice can be fetched once,
    // when the companion is there to do it and the operator agrees.
    if(!QFileInfo(companionExe()).isFile())
    {
        error = QObject::tr("Importing PowerPoint files needs PowerPoint or LibreOffice on this computer. "
                            "PDF files import without them.");
        return slides;
    }
    error.clear();
    if(!confirmDownload(parent) || !fetchLibreOffice(parent, error))
        return slides;
    slides = convert(file, sets, parent, error, needsOffice);
    if(needsOffice)
        error = QObject::tr("LibreOffice could not be set up on this computer.");
    return slides;
}

namespace
{
QList<SlideShowItem> convert(const QString &file, const SlideShowSettings &sets,
                             QWidget *parent, QString &error, bool &needsOffice)
{
    QList<SlideShowItem> slides;
    error.clear();
    needsOffice = false;

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
    QStringList arguments = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                             "-File", QDir::toNativeSeparators(script),
                             "-In", QDir::toNativeSeparators(file),
                             "-Out", QDir::toNativeSeparators(work.path()),
                             "-Size", QString::number(box)};
    // For a computer without PowerPoint. LibreOffice keeps its settings in
    // the data folder, apart from a LibreOffice the person uses themselves.
    const QString office = PresentationImport::findLibreOffice();
    if(!office.isEmpty())
        arguments << "-Soffice" << office
                  << "-Profile" << QDir::toNativeSeparators(dataDir().filePath("libreoffice-profile"));
    // CGF_IMPORT_ENGINE=libreoffice or powerpoint forces one (checks, and to
    // see what the other program makes of a file).
    const QString engine = qEnvironmentVariable("CGF_IMPORT_ENGINE");
    if(engine == "libreoffice" || engine == "powerpoint")
        arguments << "-Engine" << engine;
    ps.setArguments(arguments);

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
        {
            needsOffice = true;
            error = QObject::tr("Importing PowerPoint files needs PowerPoint or LibreOffice on this computer. "
                                "PDF files import without them.");
        }
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
} // namespace
