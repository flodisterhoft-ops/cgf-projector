// Checks that photos come in the right way up (a camera's rotation tag is
// honoured) and that the rotate buttons in the Pictures tab and the slide
// show editor turn a picture where it has to stick: in the saved slide show,
// in the schedule's copy, and on the projector at the next Go Live.
// Drives the real main window on Qt's offscreen platform. run_tests.py builds
// this and copies a CGF database beside it first.

#include <QtWidgets>
#include <QtSql>
#include <functional>

#define private public
#define protected public
#include "softprojector.hpp"
#include "picturewidget.hpp"
#include "slideshoweditor.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_picturewidget.h"
#include "ui_slideshoweditor.h"
#include "spfunctions.hpp"

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

// A 400x200 photo as a camera stores it: red on the left, blue on the right.
// With orientation 6 ("turn 90 degrees clockwise to view") the picture is a
// 200x400 portrait with red on top. The tag is a minimal Exif block put in
// right after the JPEG's start marker.
static QString makePhoto(const QString &path, int orientation)
{
    QImage img(400, 200, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(0, 0, 200, 200, Qt::red);
    p.fillRect(200, 0, 200, 200, Qt::blue);
    p.end();
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPG", 95);
    if(orientation > 0)
    {
        QByteArray exif("Exif\0\0", 6);
        exif += QByteArray("II*\0\x08\0\0\0", 8);                       // little-endian TIFF, first IFD at 8
        exif += QByteArray("\x01\0", 2);                                 // one entry
        exif += QByteArray("\x12\x01\x03\0\x01\0\0\0", 8);               // tag 0x0112, SHORT, count 1
        exif += char(orientation); exif += QByteArray("\0\0\0", 3);
        exif += QByteArray("\0\0\0\0", 4);                               // no next IFD
        QByteArray seg("\xFF\xE1", 2);
        const int len = exif.size() + 2;
        seg += char(len >> 8); seg += char(len & 0xFF);
        jpeg.insert(2, seg + exif);
    }
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(jpeg);
    return path;
}

static bool isRed(const QColor &c) { return c.red() > 180 && c.blue() < 80; }
static bool isBlue(const QColor &c) { return c.blue() > 180 && c.red() < 80; }

// Where the red half is: "top", "bottom", "left", "right" or "?".
static QString redSide(const QPixmap &pix)
{
    const QImage i = pix.toImage();
    if(i.isNull()) return "none";
    const int w = i.width(), h = i.height();
    const QColor top = i.pixelColor(w / 2, h / 8), bottom = i.pixelColor(w / 2, h - 1 - h / 8),
            left = i.pixelColor(w / 8, h / 2), right = i.pixelColor(w - 1 - w / 8, h / 2);
    if(h > w && isRed(top) && isBlue(bottom)) return "top";
    if(h > w && isRed(bottom) && isBlue(top)) return "bottom";
    if(w > h && isRed(left) && isBlue(right)) return "left";
    if(w > h && isRed(right) && isBlue(left)) return "right";
    return "?";
}

static QString sides(const SlideShowItem &s)
{
    return redSide(s.image) + "/" + redSide(s.imagePreview) + "/" + redSide(s.imageSmall);
}

static SlideShow fromDb(int id)
{
    SlideShow s;
    s.loadSlideShow(id);
    return s;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    qputenv("CGF_NO_COMPANION", "1");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8782");
    QApplication a(argc, argv);
    a.setApplicationName("CGFPicturesHarness");
    const QString dir = QCoreApplication::applicationDirPath();

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dir + "/spData.sqlite");
    if(!db.open()) { qCritical() << "no database"; return 2; }

    const QString plain = makePhoto(dir + "/plain.jpg", 0);
    const QString tagged = makePhoto(dir + "/phone.jpg", 6);
    const QString upsideDown = makePhoto(dir + "/upside-down.jpg", 3);

    // ---- Loading: the camera's tag decides which way is up.
    CHECK(redSide(loadPictureFile(plain)) == "left", "a photo without a tag loads as stored: red " + redSide(loadPictureFile(plain)));
    CHECK(redSide(loadPictureFile(tagged)) == "top", "a photo tagged 'turn clockwise' loads upright: red " + redSide(loadPictureFile(tagged)));
    CHECK(redSide(loadPictureFile(upsideDown)) == "right", "a photo tagged 'upside down' loads turned over: red " + redSide(loadPictureFile(upsideDown)));
    CHECK(loadPictureFile(dir + "/missing.jpg").isNull(), "a missing file gives no picture");
    {
        SlideShowItem s;
        CHECK(s.loadFile(tagged, 0) && sides(s) == "top/top/top" && s.image.size() == QSize(200, 400)
              && s.imageSmall.size() == QSize(50, 100), "a slide from a tagged photo is upright in all three sizes: " + sides(s));
        SlideShowItem bound;
        CHECK(bound.loadFile(tagged, 100) && bound.image.size() == QSize(50, 100), "the size limit applies to the upright picture");
        SlideShowItem none;
        CHECK(!none.loadFile(dir + "/missing.jpg", 0), "an unreadable file makes no slide");
    }

    SoftProjector w;
    w.setAppDataDir(QDir(dir));
    pump(500);
    PictureWidget *pw = w.pictureWidget;
    Ui::PictureWidget *ui = pw->ui;
    w.ui->projectTab->setCurrentWidget(pw);

    // ---- Pictures tab, pictures that are not saved anywhere.
    CHECK(!ui->pushButtonRotateLeft->isEnabled() && !ui->pushButtonRotateRight->isEnabled(), "no picture: the rotate buttons are off");
    CHECK(!ui->pushButtonRotateLeft->toolTip().isEmpty() && !ui->pushButtonRotateRight->icon().isNull(), "the rotate buttons have an icon and a name");
    QSqlQuery q;
    q.exec("SELECT COUNT(*) FROM Slides"); q.first();
    const int slidesBefore = q.value(0).toInt();
    pw->addImageFiles({tagged, plain});
    CHECK(pw->slides.count() == 2 && ui->listWidgetSlides->count() == 2, "two pictures added");
    CHECK(ui->listWidgetSlides->currentRow() == 0 && ui->pushButtonRotateRight->isEnabled(), "the first new picture is selected and can be turned");
    CHECK(sides(pw->slides.at(0)) == "top/top/top", "the tagged photo is upright in the tab: " + sides(pw->slides.at(0)));

    pw->sendToProjector();
    pump(300);
    CHECK(w.pictureShowList.count() == 2 && redSide(w.pictureShowList.at(0).image) == "top", "live: the upright photo");

    ui->pushButtonRotateRight->click();
    CHECK(sides(pw->slides.at(0)) == "right/right/right", "Rotate right turns it clockwise: " + sides(pw->slides.at(0)));
    CHECK(redSide(ui->listWidgetSlides->item(0)->icon().pixmap(100, 100)) == "right", "the list picture follows");
    CHECK(redSide(ui->labelPreview->pixmap()) == "right", "the preview follows");
    CHECK(sides(pw->slides.at(1)) == "left/left/left", "the other picture is untouched");
    CHECK(redSide(w.pictureShowList.at(0).image) == "top", "the picture on the projector stays as it is");
    ui->pushButtonGoLive->click();
    pump(300);
    CHECK(redSide(w.pictureShowList.at(0).image) == "right", "Go Live sends the turned picture");

    // A picture of the tab and its buttons beside the exe, for a look.
    w.resize(1280, 800);
    w.show();
    pump(300);
    pw->grab().save(dir + "/pictures-tab.png");

    ui->pushButtonRotateLeft->click();
    ui->pushButtonRotateLeft->click();
    CHECK(sides(pw->slides.at(0)) == "left/left/left", "Rotate left twice turns it back and once more: " + sides(pw->slides.at(0)));
    for(int i = 0; i < 4; ++i) ui->pushButtonRotateRight->click();
    CHECK(sides(pw->slides.at(0)) == "left/left/left" && pw->slides.at(0).image.size() == QSize(400, 200), "four turns come back to the same picture");
    q.exec("SELECT COUNT(*) FROM Slides"); q.first();
    CHECK(q.value(0).toInt() == slidesBefore, "unsaved pictures write nothing to the database");

    ui->pushButtonClearImages->click();
    CHECK(!ui->pushButtonRotateLeft->isEnabled(), "Clear turns the rotate buttons off");

    // ---- A saved slide show keeps the turn.
    SlideShow show;
    show.name = "Rotate check";
    {
        SlideShowItem s1, s2;
        s1.loadFile(tagged, 0);
        s2.loadFile(plain, 0);
        show.slides << s1 << s2;
    }
    show.saveSideShow("Saving", nullptr, QList<int>());
    const int showId = show.slideShowId;
    CHECK(showId > 0 && sides(fromDb(showId).slides.at(0)) == "top/top/top", "saved slide show: upright photo in the database");

    pw->loadSlideShows();
    int listRow = -1;
    for(int i = 0; i < pw->slideShows.count(); ++i)
        if(pw->slideShows.at(i).slideSwId == showId) listRow = i;
    ui->listWidgetSlideShow->setCurrentRow(listRow);
    pump(200);
    CHECK(pw->slides.count() == 2 && pw->slides.at(0).slideId != -1 && !pw->isPreviewFromSchedule(), "the saved slide show is in the preview");

    // In the schedule twice: once now, once after the turn.
    SlideShow forSchedule = pw->getCurrentSlideshow();
    w.addToShcedule(forSchedule);
    const int schedRow = w.schedule.count() - 1;
    w.is_schedule_saved = true;

    ui->listWidgetSlides->setCurrentRow(0);
    ui->pushButtonRotateRight->click();
    CHECK(sides(pw->slides.at(0)) == "right/right/right", "turned in the tab: " + sides(pw->slides.at(0)));
    CHECK(sides(fromDb(showId).slides.at(0)) == "right/right/right", "turned in the database: " + sides(fromDb(showId).slides.at(0)));
    CHECK(sides(fromDb(showId).slides.at(1)) == "left/left/left", "the other saved picture is untouched");
    CHECK(sides(w.schedule.at(schedRow).slideshow.slides.at(0)) == "right/right/right", "the schedule's copy is turned too");
    CHECK(sides(w.schedule.at(schedRow).slideshow.slides.at(1)) == "left/left/left", "the schedule's other picture is untouched");
    CHECK(!w.is_schedule_saved, "the schedule counts as changed");

    // ---- Opened from the schedule: the schedule's copy turns, the database does not.
    w.is_schedule_saved = true;
    w.ui->listWidgetSchedule->setCurrentRow(schedRow);
    pump(200);
    CHECK(pw->isPreviewFromSchedule() && pw->slides.count() == 2, "the schedule item is in the preview");
    ui->listWidgetSlides->setCurrentRow(1);
    ui->pushButtonRotateLeft->click();
    CHECK(sides(pw->slides.at(1)) == "bottom/bottom/bottom", "turned in the tab: " + sides(pw->slides.at(1)));
    CHECK(sides(w.schedule.at(schedRow).slideshow.slides.at(1)) == "bottom/bottom/bottom", "turned in the schedule");
    CHECK(sides(fromDb(showId).slides.at(1)) == "left/left/left", "a schedule item never writes to the saved slide show");
    CHECK(!w.is_schedule_saved, "the schedule counts as changed");

    // ---- Slide show editor: Save keeps the turn, Cancel drops it.
    {
        SlideShowEditor e;
        e.setSlideShow(fromDb(showId));
        e.ui->listWidgetSlides->setCurrentRow(1);
        CHECK(e.ui->pushButtonRotateRight->isEnabled(), "editor: a selected picture can be turned");
        e.ui->pushButtonRotateRight->click();
        CHECK(sides(e.editSS.slides.at(1)) == "top/top/top" && redSide(e.ui->labelPreview->pixmap()) == "top", "editor: turned, preview follows");
        e.ui->pushButtonCancel->click();
        CHECK(sides(fromDb(showId).slides.at(1)) == "left/left/left", "editor: Cancel leaves the saved picture alone");
    }
    {
        SlideShowEditor e;
        e.setSlideShow(fromDb(showId));
        e.ui->listWidgetSlides->setCurrentRow(1);
        e.ui->pushButtonRotateRight->click();
        e.ui->pushButtonSave->click();
        const SlideShow saved = fromDb(showId);
        CHECK(saved.slides.count() == 2 && sides(saved.slides.at(1)) == "top/top/top", "editor: Save keeps the turn: " + sides(saved.slides.at(1)));
        CHECK(sides(saved.slides.at(0)) == "right/right/right", "editor: the other picture is untouched");
    }
    {
        SlideShowEditor e;
        CHECK(!e.ui->pushButtonRotateLeft->isEnabled(), "editor: no picture, rotate is off");
    }

    q.exec(QString("DELETE FROM Slides WHERE ss_id = %1").arg(showId));
    q.exec(QString("DELETE FROM SlideShows WHERE id = %1").arg(showId));

    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}
