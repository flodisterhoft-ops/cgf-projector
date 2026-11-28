// Checks presentation steps (BUILD.md, "Presentation steps"): a deck whose
// animations, transitions and media are known is imported, saved, reloaded
// and stepped through live, and the projector's effect layer, the automatic
// steps and the slide media are watched while it happens.
// Drives the real main window on Qt's offscreen platform. run_tests.py builds
// this, makes the deck with PowerPoint and copies a CGF database beside it.

#include <QtWidgets>
#include <QtSql>
#include <QQuickItem>
#include <QQuickView>
#include <QJSValue>

#define private public
#define protected public
#include "softprojector.hpp"
#include "picturewidget.hpp"
#include "projectordisplayscreen.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_picturewidget.h"
#include "presentationimport.hpp"
#include "presentationsteps.hpp"

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { ++checks; if(!(cond)) { ++failures; qWarning().noquote() << "FAIL:" << (msg); } else qInfo().noquote() << "ok  :" << (msg); } while(0)

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while(t.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

static bool waitUntil(const std::function<bool()> &cond, int ms)
{
    QElapsedTimer t; t.start();
    while(!cond())
    {
        if(t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

static QString describe(const SlideStep &s)
{
    return QString("%1.%2 %3%4%5%6 %7ms%8").arg(s.slide).arg(s.index).arg(s.effect, s.direction.isEmpty() ? "" : " " + s.direction,
                                                 s.exit ? " exit" : "", s.automatic ? QString(" auto+%1").arg(s.delayMs) : QString())
            .arg(s.durationMs).arg(s.media.isEmpty() ? QString() : QString(" media:%1").arg(s.media.count()));
}

static int darkPixels(const QPixmap &p)
{
    const QImage i = p.toImage().convertToFormat(QImage::Format_Grayscale8).scaled(320, 180);
    int n = 0;
    for(int y = 0; y < i.height(); ++y)
        for(int x = 0; x < i.width(); ++x)
            n += qGray(i.pixel(x, y)) < 128;
    return n;
}

// The projector's QML, to see what the effect layer is doing.
struct Screen
{
    QQuickItem *root;
    QVariantMap fx() const
    {
        QVariant v = root->property("fx");
        if(v.canConvert<QJSValue>())
            v = v.value<QJSValue>().toVariant();
        return v.toMap();
    }
    QString kind() const { return fx().value("kind").toString(); }
    QQuickItem *item(const char *name) const { return root->findChild<QQuickItem *>(QLatin1String(name)); }
    bool busy() const { return item("fxLayer")->isVisible(); }
    int gifs() const
    {
        QVariant v = root->property("slideGifs");
        if(v.canConvert<QJSValue>())
            v = v.value<QJSValue>().toVariant();
        return v.toList().count();
    }
    QObject *player(const char *name) const { return root->findChild<QObject *>(QLatin1String(name)); }
    // Exactly one of the two picture layers is fully shown.
    bool settled() const
    {
        const qreal a = item("backImage1")->opacity(), b = item("backImage2")->opacity();
        return !busy() && ((a > 0.99 && b < 0.01) || (b > 0.99 && a < 0.01));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    qputenv("CGF_NO_COMPANION", "1");
    qputenv("CGF_SLIDE_MEDIA_MUTED", "1");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8784");
    QApplication a(argc, argv);
    a.setApplicationName("CGFStepsHarness");
    const QString dir = QCoreApplication::applicationDirPath();
    const QDir data(dir);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dir + "/spData.sqlite");
    if(!db.open()) { qCritical() << "no database"; return 2; }

    const QString deck = dir + "/steps-deck.pptx";
    const QString old = dir + "/steps-deck.ppt";
    const bool video = qEnvironmentVariable("CGF_TEST_HAS_VIDEO") == "1";
    const bool office = !qEnvironmentVariableIsEmpty("CGF_LIBREOFFICE");
    if(!QFile::exists(deck)) { qCritical() << "run_tests.py makes the deck"; return 2; }
    QDir(data.filePath("slide-media")).removeRecursively();

    SoftProjector w;
    w.setAppDataDir(data);
    w.show();
    pump(500);
    PictureWidget *pw = w.pictureWidget;
    SlideShowSettings sets;
    sets.resize = true;
    sets.boundWidth = 1280;
    pw->setSettings(sets);
    w.mySettings.slideSets = sets;
    QString error;

    // ---------------------------------------------------------------- import
    qputenv("CGF_IMPORT_ENGINE", "powerpoint");
    QList<SlideShowItem> slides = PresentationImport::run(deck, sets, &w, error);
    const int rows = video ? 19 : 17;
    CHECK(slides.count() == rows && error.isEmpty(), QString("PowerPoint: %1 stills for 6 slides shown (%2) %3").arg(rows).arg(slides.count()).arg(error));
    QList<SlideStep> s;
    for(const SlideShowItem &i : std::as_const(slides))
        s << i.step();
    while(s.count() < 19)
        s << SlideStep();
    const int last = rows - 1;

    CHECK(s[0].slide == 1 && s[0].index == 0 && s[0].effect == "fade", "slide 1: one still, fade transition: " + describe(s[0]));
    CHECK(s[1].effect == "push" && s[1].direction == "right" && s[1].index == 0, "slide 2 comes in with a push from the right: " + describe(s[1]));
    bool lines = true;
    for(int i = 2; i <= 6; ++i)
        lines = lines && s[i].slide == 2 && s[i].index == i - 1 && s[i].effect == "fade" && !s[i].automatic
                && darkPixels(slides[i].image) > darkPixels(slides[i - 1].image);
    CHECK(lines, "slide 2: five more stills, each with one more line, each a fade on a click");
    CHECK(slides[1].name == "Slide 2" && slides[3].name == "Slide 2, step 2", "names: " + slides[1].name + " / " + slides[3].name);
    CHECK(s[7].effect == "wipe" && s[7].direction == "left" && s[7].index == 0, "slide 3 wipes in from the left: " + describe(s[7]));
    CHECK(s[8].effect == "fly" && s[8].direction == "left" && !s[8].exit && !s[8].automatic, "3.1 flies in from the left on a click: " + describe(s[8]));
    CHECK(s[9].effect == "wipe" && s[9].direction == "bottom" && s[9].automatic && s[9].delayMs == 500, "3.2 wipes from the bottom by itself, 0.5 s after 3.1 began: " + describe(s[9]));
    CHECK(s[10].effect == "zoom" && !s[10].automatic, "3.3 zooms in on a click: " + describe(s[10]));
    CHECK(s[11].effect == "fade" && s[11].exit, "3.4 fades out: " + describe(s[11]));
    CHECK(darkPixels(slides[11].image) < darkPixels(slides[10].image), "3.4: the shape is gone from the still");
    const SlideStep waited = slides.value(12).step();
    CHECK(waited.slide == 3 && waited.index == 5 && waited.effect == "fade" && !waited.automatic,
          "3.5: a click that only spins a shape shows nothing new, so what follows it is the click: " + describe(waited));
    CHECK(s[13].effect == "cover" && s[13].direction == "right" && s[13].media.count() == 1 && s[13].media[0].kind == "gif",
          "slide 4 covers from the right and has an animated picture: " + describe(s[13]));
    CHECK(s[14].automatic && s[14].effect == "cut" && s[14].index == 1, "4.1 appears by itself: " + describe(s[14]));
    bool sound = false;
    for(const SlideMedia &m : std::as_const(s[15].media))
        sound = sound || (m.kind == "audio" && m.file.endsWith(".wav") && data.exists("slide-media/" + m.file));
    CHECK(sound && !s[15].automatic && s[15].slide == 4 && s[15].index == 2, "4.2 plays a sound on a click, kept in the data folder: " + describe(s[15]));
    CHECK(rows == 17 || (s[16].slide == 5 && s[16].index == 0 && s[16].effect.isEmpty() && s[16].durationMs == 500),
          "the hidden slide is left out; slide 5, the video slide, has no transition and keeps the usual fade: " + describe(s[16]));
    if(video)
    {
        const SlideMedia m = s[17].media.value(0);
        CHECK(m.kind == "video" && m.file.endsWith(".mp4") && qAbs(m.rect.x() - 0.25) < 0.01 && qAbs(m.rect.width() - 0.5) < 0.01
              && data.exists("slide-media/" + m.file), "5.1 plays the video in its place: " + describe(s[17]));
    }
    CHECK(s[last].effect == "split" && s[last].direction == "vertical" && s[last].index == 0, "the last slide opens with a split: " + describe(s[last]));

    // ---------------------------------------------------------------- saved, reloaded, live
    pw->importPresentation(deck);
    pump(300);
    CHECK(pw->slides.count() == rows && pw->slides.value(8).step().effect == "fly", "the Pictures tab has the steps");
    SlideShow reloaded;
    reloaded.loadSlideShow(pw->currentSlideShowId());
    bool same = reloaded.slides.count() == rows;
    for(int i = 0; same && i < rows; ++i)
        same = reloaded.slides[i].step().toText() == slides[i].step().toText() && reloaded.slides[i].name == slides[i].name;
    CHECK(same, "saved and loaded again, every still keeps its step");

    pw->sendToProjector();
    pump(800);
    Screen screen{w.pds1->dispView->rootObject()};
    QListWidget *list = w.ui->listShow;
    auto go = [&](int row) { list->setCurrentRow(row); pump(40); };
    const QSize size = w.pds1->imGen.getScreenSize();
    const double sx = size.width() / 1280.0;
    CHECK(w.pictureShowList.count() == rows && list->currentRow() == 0 && !screen.busy(), "live at the first still, no effect running");

    // push: both stills move
    go(1);
    QVariantMap fx = screen.fx();
    CHECK(fx.value("kind") == "fly" && fx.value("push").toBool() && fx.value("dir") == "right" && screen.busy(),
          "next: the push starts (" + screen.kind() + ")");
    pump(500);
    const double mid = screen.item("fxSprite")->x();
    CHECK(mid > 0 && mid < size.width(), QString("half a second in, the new still is on its way in from the right (x %1)").arg(mid));
    w.pds1->dispView->grabWindow().save(dir + "/frame-push.png");
    CHECK(waitUntil([&] { return screen.settled(); }, 3000), "the push ends with the new still alone");

    // fade: no effect layer, the usual cross-fade
    go(2);
    CHECK(!screen.busy() && screen.root->property("tranTime").toInt() == 500, "a line fades in with the usual cross-fade");
    go(3); go(4); go(5); go(6);

    // wipe over the whole picture
    go(7);
    CHECK(screen.kind() == "wipe" && screen.fx().value("dir") == "left", "slide 3: the wipe starts");
    pump(600);
    const double window = screen.item("fxClipA")->width();
    CHECK(window > 0 && window < size.width(), QString("the window grows from the left (%1 wide)").arg(window));
    w.pds1->dispView->grabWindow().save(dir + "/frame-wipe.png");
    waitUntil([&] { return screen.settled(); }, 3000);

    // fly: only the shape moves; then the next step follows by itself
    go(8);
    fx = screen.fx();
    CHECK(fx.value("kind") == "fly" && !fx.value("push").toBool() && qAbs(fx.value("x").toDouble() - 80 * sx) < 6
          && qAbs(fx.value("w").toDouble() - 320 * sx) < 12,
          QString("3.1: the shape flies, and only the shape (x %1, w %2)").arg(fx.value("x").toDouble()).arg(fx.value("w").toDouble()));
    pump(120);
    const double flying = screen.item("fxSprite")->x();
    CHECK(flying < fx.value("x").toDouble(), QString("it comes from the left (x %1)").arg(flying));
    w.pds1->dispView->grabWindow().save(dir + "/frame-fly.png");
    CHECK(waitUntil([&] { return list->currentRow() == 9; }, 2000), "3.2 follows by itself");
    CHECK(screen.kind() == "wipe" && screen.fx().value("dir") == "bottom", "with its wipe from the bottom");
    pump(1500);
    CHECK(list->currentRow() == 9 && screen.settled(), "and there it waits for the next click");

    go(10);
    CHECK(screen.kind() == "zoom", "3.3: zoom");
    pump(150);
    const double scale = screen.item("fxSprite")->scale();
    CHECK(scale > 0.3 && scale < 1.0, QString("the shape grows (scale %1)").arg(scale));
    waitUntil([&] { return screen.settled(); }, 2000);
    go(11);
    CHECK(!screen.busy(), "3.4: the shape fades out with the cross-fade");
    pump(1500);
    CHECK(list->currentRow() == 11, "3.5 waits for its click");
    go(12);
    pump(600);

    // cover, an animated picture, a step by itself after the transition, a sound
    go(13);
    fx = screen.fx();
    CHECK(fx.value("kind") == "fly" && !fx.value("push").toBool() && !fx.value("exit").toBool() && screen.gifs() == 1,
          "slide 4: the new still slides over the old one; the animated picture is on");
    CHECK(waitUntil([&] { return list->currentRow() == 14; }, 4000), "4.1 follows by itself once the transition is over");
    CHECK(screen.gifs() == 1, "the animated picture keeps running");
    pump(300);
    go(15);
    QObject *soundPlayer = screen.player("slideSound");
    CHECK(soundPlayer->property("source").toUrl().toString().endsWith(".wav"), "4.2: the sound is given to the player: " + soundPlayer->property("source").toUrl().toString());
    CHECK(waitUntil([&] { return soundPlayer->property("playbackState").toInt() == 1; }, 3000), "and it plays");
    go(16);
    CHECK(screen.gifs() == 0 && soundPlayer->property("playbackState").toInt() == 0, "the next slide stops the sound and the animated picture");

    if(video)
    {
        go(17);
        QObject *player = screen.player("slidePlayer");
        QQuickItem *out = screen.item("slideVideo");
        CHECK(player->property("source").toUrl().toString().endsWith(".mp4"), "5.1: the video is given to the player");
        CHECK(qAbs(out->x() - 320 * sx) < 3 && qAbs(out->y() - 200 * sx) < 3 && qAbs(out->width() - 640 * sx) < 3
              && qAbs(out->height() - 360 * sx) < 3,
              QString("in its place on the slide (%1, %2, %3 x %4)").arg(out->x()).arg(out->y()).arg(out->width()).arg(out->height()));
        CHECK(waitUntil([&] { return player->property("playbackState").toInt() == 1; }, 4000), "and it plays");
        pump(700);
        w.pds1->dispView->grabWindow().save(dir + "/frame-video.png");
        go(18);
        CHECK(player->property("playbackState").toInt() == 0, "the next slide stops the video");
    }
    else
        go(last);
    CHECK(screen.kind() == "split" && screen.fx().value("dir") == "vertical", "the last slide: split");
    waitUntil([&] { return screen.settled(); }, 3000);

    // Any other way to a still: the usual fade, never a step effect.
    go(last - 1);
    CHECK(!screen.busy(), "a step back fades");
    go(3);
    CHECK(!screen.busy(), "a jump fades");
    pump(600);

    // A step about to follow by itself is called off when the operator moves or hides.
    go(7); go(8);
    go(3);
    pump(1500);
    CHECK(list->currentRow() == 3, "moving away calls off the step that was about to follow");
    go(7); go(8);
    w.ui->actionHide->trigger();
    pump(1500);
    CHECK(list->currentRow() == 8 && w.shownPictureRow == -1, "so does Hide");
    w.ui->actionShow->trigger();
    pump(100);
    CHECK(!screen.busy(), "Show brings the still back with a fade");
    pump(1200);

    // ---------------------------------------------------------------- other ways in
    slides = PresentationImport::run(old, sets, &w, error);
    int animated = 0;
    for(const SlideShowItem &i : std::as_const(slides))
        animated += i.step().index > 0;
    CHECK(error.isEmpty() && slides.count() >= 14 && animated >= 8,
          QString("the older .ppt format: saved as .pptx first, then the same steps (%1 stills, %2 of them steps) %3").arg(slides.count()).arg(animated).arg(error));

    qputenv("CGF_IMPORT_STEPS", "0");
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(slides.count() == (video ? 6 : 5) && !slides.value(1).step().isValid(), QString("steps switched off: one still per slide (%1)").arg(slides.count()));
    qputenv("CGF_IMPORT_STEPS", "broken");
    slides = PresentationImport::run(deck, sets, &w, error);
    CHECK(error.isEmpty() && slides.count() == (video ? 6 : 5) && !slides.value(1).step().isValid(),
          QString("a copy PowerPoint will not open: the file itself is drawn, one still per slide (%1) %2").arg(slides.count()).arg(error));
    qunsetenv("CGF_IMPORT_STEPS");

    if(office)
    {
        qputenv("CGF_IMPORT_ENGINE", "libreoffice");
        slides = PresentationImport::run(deck, sets, &w, error);
        bool grows = slides.count() == rows;
        for(int i = 2; grows && i <= 6; ++i)
            grows = darkPixels(slides[i].image) > darkPixels(slides[i - 1].image);
        CHECK(grows && error.isEmpty(), QString("LibreOffice draws the same %1 stills, a line more each step (%2) %3").arg(rows).arg(slides.count()).arg(error));
        if(slides.count() > 6)
        {
            slides[1].image.save(dir + "/libreoffice-2.0.png");
            slides[3].image.save(dir + "/libreoffice-2.2.png");
        }
        slides = PresentationImport::run(old, sets, &w, error);
        animated = 0;
        for(const SlideShowItem &i : std::as_const(slides))
            animated += i.step().index > 0;
        CHECK(error.isEmpty() && slides.count() >= 6, QString("LibreOffice and the older .ppt format: %1 stills, %2 of them steps %3").arg(slides.count()).arg(animated).arg(error));
    }

    // ---------------------------------------------------------------- deleting the slide show
    const QDir media(data.filePath("slide-media"));
    const int before = media.entryList(QDir::Files).count();
    w.ui->actionHide->trigger();
    for(int i = 0; i < pw->slideShows.count(); ++i)
        if(pw->slideShows.at(i).name.startsWith("steps-deck"))
            pw->ui->listWidgetSlideShow->setCurrentRow(i);
    pump(300);
    pw->deleteSlideShow();
    CHECK(before >= (video ? 3 : 2) && media.entryList(QDir::Files).isEmpty(),
          QString("deleting the slide show removes its sounds, video and animated picture (%1 files before, %2 after)")
          .arg(before).arg(media.entryList(QDir::Files).count()));
    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}
