// Checks that each display stays on its own projector (front and back) when
// projector links drop and come back, and that only the back screen shows
// the time. Drives the real main window on Qt's offscreen platform, adding
// and removing fake screens at runtime through the plugin's setConfiguration.
// run_tests.py builds this and copies a CGF database beside it first.

#include <QtWidgets>
#include <QtSql>
#include <functional>
#include <qpa/qplatformnativeinterface.h>

#define private public
#define protected public
#include "softprojector.hpp"
#include "settingsdialog.hpp"
#include "generalsettingwidget.hpp"
#undef private
#undef protected
#include "ui_softprojector.h"
#include "ui_generalsettingwidget.h"
#include "spfunctions.hpp"

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { ++checks; if(!(cond)) { ++failures; qWarning().noquote() << "FAIL:" << (msg); } else qInfo().noquote() << "ok  :" << (msg); } while(0)

static bool waitUntil(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t; t.start();
    while(!cond())
    {
        if(t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

static void pump(int ms) { waitUntil([] { return false; }, ms); }

static QJsonObject fakeScreen(const QString &name, int x)
{
    return {{"name", name}, {"x", x}, {"y", 0}, {"width", 1920}, {"height", 1080},
            {"logicalDpi", 96}, {"logicalBaseDpi", 96}, {"dpr", 1}};
}

// Plug and unplug: the offscreen plugin's runtime screen configuration. Then
// wait past the app's 800 ms settle time for loose links.
static void setScreens(const QList<QJsonObject> &list)
{
    QJsonArray arr;
    for(const QJsonObject &o : list)
        arr.append(o);
    QPlatformNativeInterface *ni = QGuiApplication::platformNativeInterface();
    using SetConfig = void (*)(const QJsonObject &, QPlatformNativeInterface *);
    auto fn = reinterpret_cast<SetConfig>(ni->nativeResourceForIntegration("setConfiguration"));
    if(!fn)
        qFatal("offscreen setConfiguration not available");
    fn(QJsonObject{{"screens", arr}}, ni);
    pump(1500);
}

static QString screenNames()
{
    QStringList n;
    for(QScreen *s : QGuiApplication::screens())
        n << s->name();
    return n.join(", ");
}

static QString onScreen(SoftProjector &w, int display)
{
    QScreen *s = w.displayScreenNow[display];
    return s ? s->name() : QStringLiteral("none");
}

// The screen a display window covers (its geometry), "none" when hidden.
static QString windowScreen(QWidget *pds)
{
    if(!pds->isVisible())
        return QStringLiteral("hidden");
    QScreen *s = QGuiApplication::screenAt(pds->geometry().center());
    return s ? s->name() : QStringLiteral("none");
}

static bool clockOn(ProjectorDisplayScreen *pds)
{
    return pds->dispView->rootObject()->property("clockVisible").toBool();
}

// Rewrite keys of the "general" settings row before the main window loads it.
static void setGeneral(const QMap<QString, QString> &values)
{
    QSqlQuery q("SELECT sets FROM Settings WHERE type = 'general'");
    QStringList order;
    QMap<QString, QString> kv;
    if(q.next())
    {
        for(const QString &line : q.value(0).toString().split('\n'))
        {
            const int eq = line.indexOf('=');
            if(eq < 0) continue;
            const QString k = line.left(eq).trimmed();
            order << k;
            kv[k] = line.mid(eq + 1).trimmed();
        }
    }
    for(auto it = values.cbegin(); it != values.cend(); ++it)
    {
        if(!kv.contains(it.key())) order << it.key();
        kv[it.key()] = it.value();
    }
    QStringList out;
    for(const QString &k : order)
        out << k + " = " + kv[k];
    QSqlQuery u;
    u.prepare("UPDATE Settings SET sets = ? WHERE type = 'general'");
    u.addBindValue(out.join('\n'));
    if(!u.exec())
        qFatal("cannot write settings");
}

static void expect(SoftProjector &w, const QString &d1, const QString &d2, const QString &when)
{
    CHECK(onScreen(w, 0) == d1 && onScreen(w, 1) == d2,
          QString("%1: display 1 on %2, display 2 on %3 (got %4, %5; screens %6)")
          .arg(when, d1, d2, onScreen(w, 0), onScreen(w, 1), screenNames()));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("CGF_BRIDGE_LOCAL", "1");
    qputenv("CGF_NO_COMPANION", "1");
    if(qEnvironmentVariableIsEmpty("CGF_BRIDGE_PORT")) qputenv("CGF_BRIDGE_PORT", "8781");
    QApplication a(argc, argv);
    a.setApplicationName("CGFScreensHarness");
    const QString dir = QCoreApplication::applicationDirPath();

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dir + "/spData.sqlite");
    if(!db.open()) { qCritical() << "no database"; return 2; }

    const QJsonObject laptop = fakeScreen("0 Laptop", 0), front = fakeScreen("1 Front", 1920),
            back = fakeScreen("2 Back", 3840);
    CHECK(screenNames() == "0 Laptop, 1 Front, 2 Back", "starts with the laptop, front and back screens: " + screenNames());

    // Settings as saved before 0.2.6: screen numbers only.
    setGeneral({{"displayScreen", "1"}, {"displayScreen2", "2"}, {"displayScreen3", "-1"},
                {"displayScreen4", "-1"}, {"displayScreenId", ""}, {"displayScreen2Id", ""},
                {"displayScreen3Id", ""}, {"displayScreen4Id", ""}, {"displayClock2", "true"},
                {"displayOnStartUp", "true"}});

    SoftProjector w;
    w.setAppDataDir(QDir(dir));
    pump(500);

    expect(w, "1 Front", "2 Back", "start (old settings, screen numbers)");
    CHECK(screenIdentityName(w.mySettings.general.displayScreenId) == "1 Front"
          && screenIdentityName(w.mySettings.general.displayScreen2Id) == "2 Back",
          "the screen numbers' projectors are remembered");
    CHECK(clockOn(w.pds2) && !clockOn(w.pds1), "the time shows on display 2 only");
    CHECK(windowScreen(w.pds1) == "1 Front" && windowScreen(w.pds2) == "2 Back",
          QString("windows cover their screens (%1, %2)").arg(windowScreen(w.pds1), windowScreen(w.pds2)));

    // A song live on both; pictures of the two screens beside the exe. A
    // public-domain hymn (John Newton, 1779) when the library has it.
    {
        QSqlQuery q("SELECT id FROM Songs ORDER BY title NOT LIKE 'Amazing grace How sweet%', id LIMIT 1");
        if(q.next())
        {
            Song song(q.value(0).toInt());
            song.readData();
            w.setSongList(song, 0);
            pump(1000);
            w.pds1->dispView->grabWindow().save(dir + "/front-screen.png");
            w.pds2->dispView->grabWindow().save(dir + "/back-screen.png");
            CHECK(clockOn(w.pds2) && !clockOn(w.pds1), "with a song live: the time on the back only");
        }
    }

    // The front link drops: the back keeps its window and the time.
    QPointer<QScreen> backScreen = w.displayScreenNow[1];
    setScreens({laptop, back});
    CHECK(screenNames() == "0 Laptop, 2 Back", "front unplugged: " + screenNames());
    expect(w, "none", "2 Back", "front unplugged");
    CHECK(w.displayScreenNow[1] == backScreen && backScreen, "display 2 stays on the same back screen");
    CHECK(!w.hasDisplayScreen1 && w.pds1->isHidden(), "display 1 waits, hidden, instead of covering the laptop");
    CHECK(clockOn(w.pds2) && windowScreen(w.pds2) == "2 Back", "the back still shows the time");

    // The front comes back. Qt lists it last now, so the old screen numbers
    // (1 = front, 2 = back) would put display 1 on the back.
    setScreens({laptop, back, front});
    CHECK(screenNames() == "0 Laptop, 2 Back, 1 Front", "front plugged in again, listed last: " + screenNames());
    expect(w, "1 Front", "2 Back", "front back again");
    CHECK(windowScreen(w.pds1) == "1 Front" && windowScreen(w.pds2) == "2 Back" && clockOn(w.pds2) && !clockOn(w.pds1),
          "front and back did not swap; the time stays on the back");

    // The output button off and on again: both windows back on their screens.
    w.ui->actionCloseDisplay->trigger();
    CHECK(w.pds1->isHidden() && w.pds2->isHidden(), "output off: both windows hidden");
    w.ui->actionCloseDisplay->trigger();
    CHECK(windowScreen(w.pds1) == "1 Front" && windowScreen(w.pds2) == "2 Back",
          QString("output on: windows on front and back (%1, %2)").arg(windowScreen(w.pds1), windowScreen(w.pds2)));

    // The back link drops and comes back.
    setScreens({laptop, back, front});
    setScreens({laptop, front});
    expect(w, "1 Front", "none", "back unplugged");
    CHECK(!clockOn(w.pds2) && w.pds2->isHidden(), "display 2 hidden while the back is gone");
    setScreens({laptop, front, back});
    expect(w, "1 Front", "2 Back", "back plugged in again");
    CHECK(clockOn(w.pds2), "the time is back on the back screen");

    // Both drop (the chain loses power); the back comes back first.
    setScreens({laptop});
    CHECK(w.displayScreenLost && w.pds1->isHidden() && w.pds2->isHidden(), "all projectors gone: all display windows hidden");
    setScreens({laptop, back});
    expect(w, "none", "2 Back", "back first after both dropped");
    setScreens({laptop, back, front});
    expect(w, "1 Front", "2 Back", "then the front");

    // The Settings form shows the screens as they are now and, left alone,
    // keeps both projectors.
    w.settingsDialog->loadSettings(w.mySettings.general, w.theme, w.mySettings.slideSets, w.mySettings.bibleSets,
                                   w.mySettings.bibleSets2, w.mySettings.bibleSets3, w.mySettings.bibleSets4);
    {
        // As on_actionSettings_triggered does before opening the form.
        GeneralSettings g = w.mySettings.general;
        g.displayScreen = QGuiApplication::screens().indexOf(w.displayScreenNow[0]);
        g.displayScreen2 = QGuiApplication::screens().indexOf(w.displayScreenNow[1]);
        GeneralSettingWidget *form = w.settingsDialog->generalSettingswidget;
        form->setSettings(g);
        CHECK(form->ui->comboBoxDisplayScreen->currentText().contains("1 Front")
              && form->ui->comboBoxDisplayScreen_2->currentText().contains("2 Back"),
              QString("the form shows %1 and %2").arg(form->ui->comboBoxDisplayScreen->currentText(),
                                                       form->ui->comboBoxDisplayScreen_2->currentText()));
        CHECK(form->ui->checkBoxClock2->isChecked() && form->ui->checkBoxClock2->isEnabled(), "the time box is ticked");
        form->resize(form->sizeHint());
        form->grab().save(dir + "/settings-general.png");
        const GeneralSettings kept = form->getSettings();
        CHECK(kept.displayScreenId == w.mySettings.general.displayScreenId
              && kept.displayScreen2Id == w.mySettings.general.displayScreen2Id && kept.displayClock2,
              "OK without changes keeps both projectors and the time");

        // Swap them in the form: display 1 on the back, display 2 on the front.
        form->ui->comboBoxDisplayScreen->setCurrentIndex(QGuiApplication::screens().indexOf(w.displayScreenNow[1]));
        form->updateSecondaryDisplayScreen();
        form->ui->comboBoxDisplayScreen_2->setCurrentIndex(form->ui->comboBoxDisplayScreen_2->findText(
            form->monitors.at(QGuiApplication::screens().indexOf(w.displayScreenNow[0]))));
        GeneralSettings swapped = form->getSettings();
        CHECK(screenIdentityName(swapped.displayScreenId) == "2 Back" && screenIdentityName(swapped.displayScreen2Id) == "1 Front",
              "choosing other screens in the form remembers those projectors");
        w.updateSetting(swapped, w.theme, w.mySettings.slideSets, w.mySettings.bibleSets,
                        w.mySettings.bibleSets2, w.mySettings.bibleSets3, w.mySettings.bibleSets4);
        w.displaySettingsChanged();
        pump(300);
        expect(w, "2 Back", "1 Front", "after choosing them the other way round");
        CHECK(clockOn(w.pds2) && windowScreen(w.pds2) == "1 Front", "the time follows display 2");
    }

    // The time can be turned off.
    {
        GeneralSettings g = w.mySettings.general;
        g.displayClock2 = false;
        w.updateSetting(g, w.theme, w.mySettings.slideSets, w.mySettings.bibleSets,
                        w.mySettings.bibleSets2, w.mySettings.bibleSets3, w.mySettings.bibleSets4);
        CHECK(!clockOn(w.pds2), "time turned off in Settings: gone from display 2");
    }

    // Saved and read back: the projectors and the time setting survive.
    {
        w.mySettings.general.displayClock2 = true;
        w.mySettings.saveSettings();
        Settings fresh;
        fresh.loadSettings();
        CHECK(fresh.general.displayScreenId == w.mySettings.general.displayScreenId
              && fresh.general.displayScreen2Id == w.mySettings.general.displayScreen2Id
              && fresh.general.displayClock2,
              "projectors and the time setting are saved and read back");
        CHECK(!fresh.general.displayScreenId.contains('=') && !fresh.general.displayScreenId.contains('\'')
              && !fresh.general.displayScreenId.contains('\n'), "the saved projector text is safe for the settings row");
    }

    // One projector only (display 2 off): a replaced projector still shows.
    {
        GeneralSettings g = w.mySettings.general;
        g.displayScreen2 = -1;
        g.displayScreen2Id.clear();
        w.updateSetting(g, w.theme, w.mySettings.slideSets, w.mySettings.bibleSets,
                        w.mySettings.bibleSets2, w.mySettings.bibleSets3, w.mySettings.bibleSets4);
        w.displaySettingsChanged();
        setScreens({laptop, fakeScreen("3 Other", 1920)});
        expect(w, "3 Other", "none", "one projector, replaced by another model");
        CHECK(windowScreen(w.pds1) == "3 Other", "display 1 shows on it");
    }

    qInfo().noquote() << QString("%1 checks, %2 failed").arg(checks).arg(failures);
    return failures ? 1 : 0;
}
