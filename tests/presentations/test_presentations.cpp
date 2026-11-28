// Checks the PowerPoint import on a computer without PowerPoint: the offer to
// download LibreOffice, the download through the companion (served from this
// computer by run_tests.py, checked against the SHA-256 built into the
// companion), unpacking into the data folder, and imports through LibreOffice
// afterwards. Also that PowerPoint itself and PDF files still import.
// Drives the real main window on Qt's offscreen platform. run_tests.py builds
// this, makes the test deck with PowerPoint and copies a CGF database and the
// companion beside it first.

#include <QtWidgets>
#include <QtSql>

#define private public
#define protected public
#include "softprojector.hpp"
#include "picturewidget.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_picturewidget.h"
#include "presentationimport.hpp"

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { ++checks; if(!(cond)) { ++failures; qWarning().noquote() << "FAIL:" << (msg); } else qInfo().noquote() << "ok  :" << (msg); } while(0)

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while(t.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// Every slide of the 16:9 deck is drawn for a 1920 x 1080 projector,
// whatever size the Pictures setting gives photos.
static bool slidesFit(const QList<SlideShowItem> &slides)
{
    for(const SlideShowItem &s : slides)
    {
        const QSize z = s.image.size();
        if(qAbs(z.width() - 1920) > 2 || qAbs(z.height() - 1080) > 2)
            return false;
    }
    return !slides.isEmpty();
}

// The first slide has a dark blue background, the last a white one.
static bool looksRight(const QList<SlideShowItem> &slides)
{
    if(slides.isEmpty())
        return false;
    const QColor first = slides.first().image.toImage().pixelColor(20, 20);
    const QColor last = slides.last().image.toImage().pixelColor(20, 20);
    return first.blue() > first.red() + 30 && first.lightness() < 120 && last.lightness() > 240;
}

static bool officeRunning()
{
    QProcess p;
    p.start("tasklist", {"/FI", "IMAGENAME eq soffice.bin", "/NH"});
    p.waitForFinished(10000);
    return p.readAllStandardOutput().contains("soffice.bin");
}

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    qputenv("CGF_NO_COMPANION", "1");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8783");
    QApplication a(argc, argv);
    a.setApplicationName("CGFPresentationsHarness");
    const QString dir = QCoreApplication::applicationDirPath();
    const QDir data(dir);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dir + "/spData.sqlite");
    if(!db.open()) { qCritical() << "no database"; return 2; }

    const QString deck = dir + "/test-deck.pptx";
    const QString pdf = dir + "/test-deck.pdf";
    const QString served = qEnvironmentVariable("CGF_TEST_OFFICE_URL");   // the real file, from this computer
    const QString base = served.left(served.lastIndexOf('/') + 1);
    if(!QFile::exists(deck) || served.isEmpty()) { qCritical() << "run_tests.py prepares the deck and the download"; return 2; }

    SoftProjector w;
    w.setAppDataDir(data);
    pump(500);
    PictureWidget *pw = w.pictureWidget;
    SlideShowSettings sets;
    sets.resize = true;
    sets.boundWidth = 1280;
    pw->setSettings(sets);

    int asked = 0;
    bool answer = false;
    PresentationImport::confirmDownload = [&](QWidget *) { ++asked; return answer; };
    QString error;

    // ---- PowerPoint and PDF, as before.
    qputenv("CGF_IMPORT_ENGINE", "powerpoint");
    QList<SlideShowItem> slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(slides.count() == 6 && error.isEmpty(), QString("PowerPoint: 6 slides, the hidden one left out (%1) %2").arg(slides.count()).arg(error));
    CHECK(slidesFit(slides) && looksRight(slides), "PowerPoint: slides are 1920 x 1080 and look right");
    slides = PresentationImport::run(pdf, sets, &w, error);
    CHECK(slides.count() == 6 && error.isEmpty() && looksRight(slides), QString("PDF: 6 pages (%1) %2").arg(slides.count()).arg(error));

    // ---- A computer without PowerPoint and without LibreOffice.
    qputenv("CGF_IMPORT_ENGINE", "libreoffice");
    CHECK(PresentationImport::findLibreOffice().isEmpty(), "no LibreOffice yet: " + PresentationImport::findLibreOffice());
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(asked == 1 && slides.isEmpty() && error.isEmpty(), QString("it asks before downloading; Cancel imports nothing and shows no error (asked %1) %2").arg(asked).arg(error));
    CHECK(!data.exists("libreoffice") && !data.exists("libreoffice-download"), "Cancel downloads nothing");
    slides = PresentationImport::run(pdf, sets, &w, error);
    CHECK(asked == 1 && slides.count() == 6, "a PDF imports without asking");

    // ---- Downloads that fail leave nothing behind and say why.
    answer = true;
    qputenv("CGF_LIBREOFFICE_URL", (base + "missing.msi").toUtf8());
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(asked == 2 && slides.isEmpty() && error.contains("could not be downloaded"), "a failed download: " + error);
    qputenv("CGF_LIBREOFFICE_URL", (base + "not-libreoffice.msi").toUtf8());
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(asked == 3 && slides.isEmpty() && error.contains("damaged"), "a file that is not LibreOffice as published is refused: " + error);
    CHECK(!data.exists("libreoffice") && !data.exists("libreoffice-download") && !data.exists("libreoffice.new"),
          "failed downloads leave nothing behind");

    // ---- The real download: asks once, fetches, unpacks, imports.
    qputenv("CGF_LIBREOFFICE_URL", served.toUtf8());
    QElapsedTimer timer; timer.start();
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(asked == 4 && slides.count() == 6 && error.isEmpty(),
          QString("Download: LibreOffice is fetched and the deck imports, 6 slides (%1) in %2 s %3").arg(slides.count()).arg(timer.elapsed() / 1000).arg(error));
    CHECK(slidesFit(slides) && looksRight(slides), "LibreOffice: slides are 1920 x 1080 and look right");
    if(!slides.isEmpty())
        slides.first().image.save(dir + "/libreoffice-slide1.png");
    const QString office = PresentationImport::findLibreOffice();
    CHECK(QDir::fromNativeSeparators(office) == data.filePath("libreoffice/program/soffice.exe"), "LibreOffice is in the data folder: " + office);
    CHECK(!data.exists("libreoffice-download") && !data.exists("libreoffice.new") && !data.exists("libreoffice/help")
          && QDir(data.filePath("libreoffice")).entryList({"*.msi"}, QDir::Files).isEmpty(),
          "the download and the parts not needed are gone");
    qint64 bytes = 0;
    QDirIterator it(data.filePath("libreoffice"), QDir::Files, QDirIterator::Subdirectories);
    while(it.hasNext())
    {
        it.next();
        bytes += it.fileInfo().size();
    }
    CHECK(bytes > 400000000 && bytes < 900000000 && !data.exists("libreoffice/program/resource")
          && QDir(data.filePath("libreoffice/share/extensions")).entryList({"dict-*"}, QDir::Dirs).isEmpty(),
          QString("LibreOffice is trimmed to what the import needs: %1 MB").arg(bytes / 1000000));
    QFile settings(data.filePath("libreoffice-profile/user/registrymodifications.xcu"));
    CHECK(settings.open(QIODevice::ReadOnly) && settings.readAll().contains("Aptos"),
          "LibreOffice's own settings are in the data folder, with Aptos shown as Calibri");

    // ---- From now on: no question, any file name, the Pictures tab.
    const QString odd = dir + QString::fromUtf8("/Проповедь Gnade ü.pptx");
    QFile::remove(odd);
    QFile::copy(deck, odd);
    timer.restart();
    slides = PresentationImport::run(odd, sets, &w, error);
    CHECK(asked == 4 && slides.count() == 6 && error.isEmpty(),
          QString("second import: no question, Cyrillic file name, 6 slides (%1) in %2 s %3").arg(slides.count()).arg(timer.elapsed() / 1000).arg(error));

    const int showsBefore = pw->slideShows.count();
    pw->importPresentation(deck);
    pump(300);
    CHECK(pw->slideShows.count() == showsBefore + 1 && pw->slides.count() == 6
          && pw->ui->pushButtonGoLive->isEnabled(), "Pictures tab: the deck is a saved slide show, selected and ready");
    QSqlQuery q;
    q.exec("SELECT id FROM SlideShows WHERE name LIKE 'test-deck%'");
    while(q.next())
    {
        QSqlQuery d;
        d.exec("DELETE FROM Slides WHERE ss_id = " + q.value(0).toString());
        d.exec("DELETE FROM SlideShows WHERE id = " + q.value(0).toString());
    }

    // ---- Cancel while LibreOffice works: nothing imported, LibreOffice stopped.
    QTimer::singleShot(2500, &w, [] {
        for(QWidget *top : QApplication::topLevelWidgets())
            if(auto *dialog = qobject_cast<QProgressDialog *>(top))
                dialog->cancel();
    });
    slides = PresentationImport::run(deck, sets, &w, error);
    pump(2000);
    CHECK(slides.isEmpty() && error.isEmpty(), "Cancel during the conversion imports nothing and shows no error: " + error);
    CHECK(!officeRunning(), "Cancel stops LibreOffice");

    // ---- A broken file fails with a message, without a new question.
    const QString broken = dir + "/broken.pptx";
    { QFile f(broken); f.open(QIODevice::WriteOnly); f.write("this is not a presentation"); }
    slides = PresentationImport::run(broken, sets, &w, error);
    CHECK(asked == 4 && slides.isEmpty() && !error.isEmpty(), "a broken file: " + error);

    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}
