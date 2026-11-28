/***************************************************************************
//
//    softProjector - an open source media projection software
//    Copyright (C) 2017  Vladislav Kobzar
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

#include "../headers/projectordisplayscreen.hpp"
#include "ui_projectordisplayscreen.h"


ProjectorDisplayScreen::ProjectorDisplayScreen(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ProjectorDisplayScreen)
{
    ui->setupUi(this);
    dispView = new QQuickView;
    imProvider = new SpImageProvider;
    dispView->engine()->addImageProvider(QLatin1String("improvider"),imProvider);
    QWidget *w = QWidget::createWindowContainer(dispView,this);
    dispView->setSource(QUrl("qrc:/qml/qml/DisplayArea.qml"));
    ui->verticalLayout->addWidget(w);

    // Create Display object for retriaving signals back from the display screen
    QObject *dispObj = dispView->rootObject();
    connect(dispObj,SIGNAL(exitClicked()),this,SLOT(exitSlideClicked()));
    connect(dispObj,SIGNAL(nextClicked()),this,SLOT(nextSlideClicked()));
    connect(dispObj,SIGNAL(prevClicked()),this,SLOT(prevSlideClicked()));

    // Connect Media Player objects
    connect(dispObj,SIGNAL(positionChanged(int)),this,SLOT(videoPositionChanged(int)));
    connect(dispObj,SIGNAL(durationChanged(int)),this,SLOT(videoDurationChanged(int)));
    connect(dispObj,SIGNAL(playbackStateChanged(int)),this,SLOT(videoPlaybackStateChanged(int)));
    connect(dispObj,SIGNAL(playbackStopped()),this,SLOT(playbackStopped()));

    backImSwitch1 = backImSwitch2 = textImSwitch1 = textImSwitch2 = false;
    back1to2 = text1to2 = isNewBack = true;
    m_color.setRgb(0,0,0,0);// = QColor(QColor::black());
}

ProjectorDisplayScreen::~ProjectorDisplayScreen()
{
    delete dispView;
    delete ui;
}

void ProjectorDisplayScreen::resetImGenSize()
{
    imGen.setScreenSize(this->size());
}

void ProjectorDisplayScreen::setBackPixmap(QPixmap p, int fillMode)
{
    // fill mode -->>  0 = Strech, 1 = keep aspect, 2 = keep aspect by expanding

    if(back.cacheKey() == p.cacheKey())
    {
        isNewBack = false;
        return;
    }
    back = p;
    isNewBack = true;

    switch(fillMode)
    {
    case 0:
        p = p.scaled(imGen.getScreenSize(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        break;
    case 1:
        p = p.scaled(imGen.getScreenSize(),Qt::KeepAspectRatio,Qt::SmoothTransformation);
        break;
    case 2:
        p = p.scaled(imGen.getScreenSize(),Qt::KeepAspectRatioByExpanding,Qt::SmoothTransformation);
        break;
    default:
        // Do No Scaling/resizing
        break;
    }

    imProvider->setPixMap(p);
    shownBack = p;
    back1to2 = (!back1to2);

    QObject *item1 = dispView->rootObject()->findChild<QObject*>("backImage1");
    QObject *item2 = dispView->rootObject()->findChild<QObject*>("backImage2");
    if(item1 && item2)
    {
        if(back1to2)
        {
            backImSwitch2 = (!backImSwitch2);
            if(backImSwitch2)
                item2->setProperty("source","image://improvider/imB2a");
            else
                item2->setProperty("source","image://improvider/imB2b");

            if(p.height()<imGen.height())
                item2->setProperty("y",(imGen.height()-p.height())/2);
            else
                item2->setProperty("y",0);
            if(p.width()<imGen.width())
                item2->setProperty("x",(imGen.width()-p.width())/2);
            else
                item2->setProperty("x",0);
        }
        else
        {
            backImSwitch1 = (!backImSwitch1);
            if(backImSwitch1)
                item1->setProperty("source","image://improvider/imB1a");
            else
                item1->setProperty("source","image://improvider/imB1b");
            if(p.height()<imGen.height())
                item1->setProperty("y",(imGen.height()-p.height())/2);
            else
                item1->setProperty("y",0);
            if(p.width()<imGen.width())
                item1->setProperty("x",(imGen.width()-p.width())/2);
            else
                item1->setProperty("x",0);
        }
    }
}

void ProjectorDisplayScreen::setBackPixmap(QPixmap p, QColor c)
{
    if(backType == 0)
        setBackPixmap(imGen.generateColorImage(c),0);
    else if(backType == 1)
        setBackPixmap(p,0);
    else if(backType ==2)
        setBackPixmap(imGen.generateEmptyImage(),0);
}

void ProjectorDisplayScreen::setTextPixmap(QPixmap p)
{
    imProvider->setPixMap(p);
    shownText = p;
    text1to2 = (!text1to2);

    QObject *item1 = dispView->rootObject()->findChild<QObject*>("textImage1");
    QObject *item2 = dispView->rootObject()->findChild<QObject*>("textImage2");
    if(item1 && item2)
    {
        if(text1to2)
        {
            textImSwitch2 = (!textImSwitch2);
            if(textImSwitch2)
                item2->setProperty("source","image://improvider/imT2a");
            else
                item2->setProperty("source","image://improvider/imT2b");
        }
        else
        {
            textImSwitch1 = (!textImSwitch1);
            if(textImSwitch1)
                item1->setProperty("source","image://improvider/imT1a");
            else
                item1->setProperty("source","image://improvider/imT1b");
        }
    }
}

void ProjectorDisplayScreen::setBackVideo(QString path)
{
    // Silent background loop. Qt 6 MediaPlayer.loops is a play count
    // (Infinite = -1); the Qt 5 playlist enum passed before (Loop = 3) meant
    // "play three times".
    QUrl source = path.contains("://") ? QUrl(path) : QUrl::fromLocalFile(path);
    loadVideo(source, QMediaPlayer::Infinite, 0.0, Qt::IgnoreAspectRatio);
}

void ProjectorDisplayScreen::loadVideo(const QUrl &source, int loops, double volume, Qt::AspectRatioMode fill)
{
    QMetaObject::invokeMethod(dispView->rootObject(), "loadVideo",
                              Q_ARG(QVariant, source), Q_ARG(QVariant, loops),
                              Q_ARG(QVariant, volume), Q_ARG(QVariant, int(fill)));
}

void ProjectorDisplayScreen::updateScreen()
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"stopTransitions");
    root->setProperty("tranTime", fadeTime);
    if(!keepSlideMedia)
        QMetaObject::invokeMethod(root,"stopSlideMedia");
    // A presentation step's own effect replaces the background's transition.
    if(isNewBack && !stepEffect.isEmpty())
    {
        QMetaObject::invokeMethod(root,"transitionBackStep",Q_ARG(QVariant,back1to2),Q_ARG(QVariant,stepEffect));
        isNewBack = false;
    }
    stepEffect.clear();
    fadeTime = 500;
    keepSlideMedia = false;
    //    QString tranType = "seq";

    // if background is a video, play video, else stop it.
    if(backType == B_VIDEO)
    {
        QMetaObject::invokeMethod(root,"playVideo");
    }
    else
    {
        QMetaObject::invokeMethod(root,"stopVideo");
    }

    if(text1to2 && back1to2)
    {
        if(isNewBack)
            QMetaObject::invokeMethod(root,"transitionBack1to2",Q_ARG(QVariant,tranType));
        QMetaObject::invokeMethod(root,"transitionText1to2",Q_ARG(QVariant,tranType));
    }
    else if(text1to2 && (!back1to2))
    {
        if(isNewBack)
            QMetaObject::invokeMethod(root,"transitionBack2to1",Q_ARG(QVariant,tranType));
        QMetaObject::invokeMethod(root,"transitionText1to2",Q_ARG(QVariant,tranType));
    }
    else if((!text1to2) && back1to2)
    {
        if(isNewBack)
            QMetaObject::invokeMethod(root,"transitionBack1to2",Q_ARG(QVariant,tranType));
        QMetaObject::invokeMethod(root,"transitionText2to1",Q_ARG(QVariant,tranType));
    }
    else
    {
        if(isNewBack)
            QMetaObject::invokeMethod(root,"transitionBack2to1",Q_ARG(QVariant,tranType));
        QMetaObject::invokeMethod(root,"transitionText2to1",Q_ARG(QVariant,tranType));
    }
    emit frameShown(shownText, shownBack, imGen.getScreenSize(), tranType != TR_NONE);
}

void ProjectorDisplayScreen::exitSlideClicked()
{
    emit exitSlide();
}

void ProjectorDisplayScreen::nextSlideClicked()
{
    emit nextSlide();
}

void ProjectorDisplayScreen::prevSlideClicked()
{
    emit prevSlide();
}

void ProjectorDisplayScreen::videoPositionChanged(int position)
{
    emit videoPositionChanged((qint64)position);
}

void ProjectorDisplayScreen::videoDurationChanged(int duration)
{
    emit videoDurationChanged((qint64)duration);
}

void ProjectorDisplayScreen::videoPlaybackStateChanged(int state)
{
    emit videoPlaybackStateChanged((QMediaPlayer::PlaybackState)state);
}

void ProjectorDisplayScreen::keyReleaseEvent(QKeyEvent *event)
{
    // Will get called when a key is released
    int key = event->key();
    if(key == Qt::Key_Left)
        emit prevSlide();
    else if(key == Qt::Key_Up)
        emit prevSlide();
    else if(key == Qt::Key_PageUp)
        emit prevSlide();
    else if(key == Qt::Key_Back)
        emit prevSlide();
    else if(key == Qt::Key_Right)
        emit nextSlide();
    else if(key == Qt::Key_Down)
        emit nextSlide();
    else if(key == Qt::Key_PageDown)
        emit nextSlide();
    else if(key == Qt::Key_Forward)
        emit nextSlide();
    else if(key == Qt::Key_Enter)
        emit nextSlide();
    else if(key == Qt::Key_Return)
        emit nextSlide();
    else if(key == Qt::Key_Escape)
        emit exitSlide();
    else
        QWidget::keyReleaseEvent(event);
}

void ProjectorDisplayScreen::renderNotText()
{
    setTextPixmap(imGen.generateEmptyImage());
    updateScreen();
}

void ProjectorDisplayScreen::renderPassiveText(QPixmap &back, bool useBack)
{
    setTextPixmap(imGen.generateEmptyImage());
    if(useBack)
    {
        backType = B_PICTURE;
        setBackPixmap(back,0);
    }
    else
    {
        backType = B_NONE;
        setBackPixmap(imGen.generateColorImage(m_color),0);
    }

    updateScreen();
}

void ProjectorDisplayScreen::renderBibleText(Verse bVerse, BibleSettings &bSets)
{
    // TODO: This is temporary until database is fixed
    if(bSets.useFading)
    {
        tranType = TR_FADE;
    }
    else
    {
        tranType = TR_NONE;
    }

    if(bSets.useBackground)
    {
        setBackPixmap(bSets.backgroundPix,0);
        backType = B_PICTURE;
    }
    else
    {
        setBackPixmap(imGen.generateColorImage(m_color),0);
        backType = B_NONE;
    }

    //tranType = bSets.transitionType;
    //backType = bSets.backgroundType;
    setTextPixmap(imGen.generateBibleImage(bVerse,bSets));
    //setBackPixmap(bSets.backgroundPix,bSets.backgroundColor);
    //if(backType ==2)
    //{
    //    setVideoSource(bSets.backgroundVideoPath);
    //}

    updateScreen();
}

void ProjectorDisplayScreen::renderSongText(Stanza stanza, SongSettings &sSets)
{
    // TODO: This is temporary until database is fixed
    if(sSets.useFading)
    {
        tranType = TR_FADE;
    }
    else
    {
        tranType = TR_NONE;
    }

    if(sSets.useBackground)
    {
        setBackPixmap(sSets.backgroundPix,0);
        backType = B_PICTURE;
    }
    else
    {
        setBackPixmap(imGen.generateColorImage(m_color),0);
        backType = B_NONE;
    }

    setTextPixmap(imGen.generateSongImage(stanza,sSets));
    //if(sSets.backgroundType == 1)
    //    setBackPixmap(sSets.backgroundPix,0);
    //else
    //    setBackPixmap(imGen.generateColorImage(m_color),0);
    //tranType = sSets.transitionType;
    updateScreen();
}

void ProjectorDisplayScreen::renderAnnounceText(AnnounceSlide announce, TextSettings &aSets)
{
    // TODO: This is temporary until database is fixed
    if(aSets.useFading)
    {
        tranType = TR_FADE;
    }
    else
    {
        tranType = TR_NONE;
    }

    if(aSets.useBackground)
    {
        setBackPixmap(aSets.backgroundPix,0);
        backType = B_PICTURE;
    }
    else
    {
        setBackPixmap(imGen.generateColorImage(m_color),0);
        backType = B_NONE;
    }

    setTextPixmap(imGen.generateAnnounceImage(announce,aSets));
    //if(aSets.transitionType == 1)
    //    setBackPixmap(aSets.backgroundPix,0);
    //else
    //    setBackPixmap(imGen.generateColorImage(m_color),0);
    //tranType = aSets.transitionType;
    updateScreen();
}

void ProjectorDisplayScreen::renderSlideShow(QPixmap slide, SlideShowSettings &ssSets)
{
    tranType = TR_FADE;

    bool expand;
    if(slide.width()<imGen.width() && slide.height()<imGen.height())
        expand = ssSets.expandSmall;
    else
        expand = true;

    setTextPixmap(imGen.generateEmptyImage());
    if(expand)
        setBackPixmap(slide,ssSets.fitType +1);
    else
        setBackPixmap(slide,3);
    updateScreen();

}

// Where two stills of the same size differ. With sprite, also the differing
// pixels of "from" as a picture of their own: the more a pixel differs, the
// more solid it is, which keeps soft edges soft.
static QRect changedRect(const QImage &before, const QImage &after, const QImage &from, QImage *sprite)
{
    const QImage a = before.convertToFormat(QImage::Format_RGB32);
    const QImage b = after.convertToFormat(QImage::Format_RGB32);
    const QImage f = from.convertToFormat(QImage::Format_RGB32);
    if(a.size() != b.size() || a.isNull())
        return QRect();
    auto difference = [](QRgb p, QRgb q) {
        return qMax(qAbs(qRed(p) - qRed(q)), qMax(qAbs(qGreen(p) - qGreen(q)), qAbs(qBlue(p) - qBlue(q))));
    };
    int left = a.width(), right = -1, top = a.height(), bottom = -1;
    for(int y = 0; y < a.height(); ++y)
    {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for(int x = 0; x < a.width(); ++x)
        {
            // Stills kept as JPEG differ a little around what changed.
            if(pa[x] == pb[x] || difference(pa[x], pb[x]) <= 20)
                continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = y;
        }
    }
    if(right < 0)
        return QRect();
    const QRect region = QRect(left, top, right - left + 1, bottom - top + 1).adjusted(-2, -2, 2, 2) & a.rect();
    if(sprite)
    {
        *sprite = QImage(region.size(), QImage::Format_ARGB32_Premultiplied);
        for(int y = 0; y < region.height(); ++y)
        {
            const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(region.y() + y)) + region.x();
            const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(region.y() + y)) + region.x();
            const QRgb *pf = reinterpret_cast<const QRgb *>(f.constScanLine(region.y() + y)) + region.x();
            QRgb *out = reinterpret_cast<QRgb *>(sprite->scanLine(y));
            for(int x = 0; x < region.width(); ++x)
            {
                const int alpha = qBound(0, (difference(pa[x], pb[x]) - 6) * 12, 255);
                out[x] = qPremultiply(qRgba(qRed(pf[x]), qGreen(pf[x]), qBlue(pf[x]), alpha));
            }
        }
    }
    return region;
}

void ProjectorDisplayScreen::setEffectImage(const char *item, const QPixmap &p)
{
    // The provider hands out the picture set last; a new name makes QML ask again.
    QPixmap copy = p;
    imProvider->setPixMap(copy);
    QObject *image = dispView->rootObject()->findChild<QObject*>(QLatin1String(item));
    if(image)
        image->setProperty("source", QStringLiteral("image://improvider/fx%1").arg(++effectImages));
}

void ProjectorDisplayScreen::renderSlideStep(QPixmap slide, SlideShowSettings &ssSets, const SlideStep &step,
                                             bool animate, bool sound)
{
    // The still on screen now, if this function put it there: the effect is
    // worked out from the difference between it and the new one.
    const QPixmap before = shownBack.cacheKey() == slideStillKey ? shownBack : QPixmap();

    tranType = TR_FADE;
    const bool expand = (slide.width() < imGen.width() && slide.height() < imGen.height()) ? ssSets.expandSmall : true;
    setTextPixmap(imGen.generateEmptyImage());
    setBackPixmap(slide, expand ? ssSets.fitType + 1 : 3);
    const QPixmap after = shownBack;
    slideStillKey = after.cacheKey();
    // Where setBackPixmap put the still: centred when smaller than the screen.
    const QPoint at(after.width() < imGen.width() ? (imGen.width() - after.width()) / 2 : 0,
                    after.height() < imGen.height() ? (imGen.height() - after.height()) / 2 : 0);

    if(animate && step.isValid() && isNewBack)
    {
        const QString kind = step.effect;
        fadeTime = qBound(100, step.durationMs, 2000);
        const bool whole = step.index == 0;     // a slide's transition works on the whole picture
        const bool sameSize = !before.isNull() && before.size() == after.size();
        if(kind == QLatin1String("cut"))
            tranType = TR_NONE;
        else if(kind == QLatin1String("fade") || kind.isEmpty() || !sameSize)
            tranType = TR_FADE;
        else
        {
            const bool moves = kind == QLatin1String("fly") || kind == QLatin1String("float") || kind == QLatin1String("zoom");
            const bool slides = kind == QLatin1String("push") || kind == QLatin1String("cover") || kind == QLatin1String("uncover");
            QVariantMap fx;
            fx.insert("dir", step.direction);
            fx.insert("duration", fadeTime);
            fx.insert("sx", at.x());
            fx.insert("sy", at.y());
            fx.insert("exit", step.exit || kind == QLatin1String("uncover"));
            fx.insert("push", kind == QLatin1String("push"));
            QRect region(QPoint(0, 0), after.size());
            if(whole && slides)
            {
                // The whole still moves: the new one in, the old one out, or both.
                fx.insert("kind", "fly");
                if(kind == QLatin1String("push"))
                    setEffectImage("fxSpriteOut", before);
                setEffectImage("fxSprite", kind == QLatin1String("uncover") ? before : after);
            }
            else if(whole)
            {
                fx.insert("kind", kind);        // wipe, split or box over the whole picture
                setEffectImage("fxStillA", after);
                setEffectImage("fxStillB", after);
            }
            else
            {
                QImage sprite;
                region = changedRect(before.toImage(), after.toImage(), (step.exit ? before : after).toImage(),
                                     moves ? &sprite : nullptr);
                fx.insert("kind", kind);
                if(moves)
                    setEffectImage("fxSprite", QPixmap::fromImage(sprite));
                else
                {
                    setEffectImage("fxStillA", after);
                    setEffectImage("fxStillB", after);
                }
            }
            fx.insert("x", at.x() + region.x());
            fx.insert("y", at.y() + region.y());
            fx.insert("w", region.width());
            fx.insert("h", region.height());
            // Nothing to see changed, or nearly everything did: a plain fade says it better.
            const bool huge = !whole && qint64(region.width()) * region.height() > qint64(after.width()) * after.height() * 85 / 100;
            if(region.isEmpty())
                tranType = TR_NONE;
            else if(!huge && (fx.value("kind").toString() == kind || slides))
                stepEffect = fx;
        }
    }

    // Videos, sounds and animated pictures of this step, where the still is on the screen.
    QVariantList media;
    const QDir folder(PresentationSteps::mediaDir());
    for(const SlideMedia &m : step.media)
    {
        if(!folder.exists(m.file))
            continue;
        QVariantMap v;
        v.insert("kind", m.kind);
        v.insert("source", QUrl::fromLocalFile(folder.filePath(m.file)).toString());
        v.insert("x", at.x() + m.rect.x() * after.width());
        v.insert("y", at.y() + m.rect.y() * after.height());
        v.insert("w", m.rect.width() * after.width());
        v.insert("h", m.rect.height() * after.height());
        v.insert("volume", m.volume);
        v.insert("loop", m.loop);
        // CGF_SLIDE_MEDIA_MUTED: the checks play real sounds, silently.
        v.insert("muted", !sound || qEnvironmentVariableIsSet("CGF_SLIDE_MEDIA_MUTED"));
        media << v;
    }
    keepSlideMedia = !media.isEmpty();
    updateScreen();
    if(!media.isEmpty())
        QMetaObject::invokeMethod(dispView->rootObject(), "playSlideMedia", Q_ARG(QVariant, QVariant(media)));
}

void ProjectorDisplayScreen::renderVideo(VideoInfo videoDetails)
{
    backType = B_VIDEO;
    setTextPixmap(imGen.generateEmptyImage());
    setBackPixmap(imGen.generateColorImage(m_color),0);

    // Play once. The Qt 5 enum used before (CurrentItemOnce = 0) is ignored by
    // Qt 6, so a video kept whatever loop count the previous one had set.
    loadVideo(videoDetails.filePath, QMediaPlayer::Once, 1.0, Qt::KeepAspectRatio);

    updateScreen();
}

void ProjectorDisplayScreen::playVideo()
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"playVideo");
}

void ProjectorDisplayScreen::pauseVideo()
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"pauseVideo");
}

void ProjectorDisplayScreen::stopVideo()
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"stopVideo");
}

void ProjectorDisplayScreen::playbackStopped()
{
    emit videoStopped();
}

qint64 ProjectorDisplayScreen::videoPosition()
{
    QObject *player = dispView->rootObject()->findChild<QObject*>("player");
    return player ? player->property("position").toLongLong() : 0;
}

bool ProjectorDisplayScreen::isVideoPaused()
{
    QObject *player = dispView->rootObject()->findChild<QObject*>("player");
    return player && player->property("playbackState").toInt() == QMediaPlayer::PausedState;
}

void ProjectorDisplayScreen::setVideoVolume(int level)
{
    float vol = 1.0*level/100;
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"setVideoVolume",Q_ARG(QVariant,vol));
}

void ProjectorDisplayScreen::setVideoMuted(bool muted)
{
    QObject *root = dispView->rootObject();
    // Was calling "setVideoVolume" with !muted, so un-muting jumped to full volume.
    QMetaObject::invokeMethod(root,"setVideoMuted",Q_ARG(QVariant,muted));
}

void ProjectorDisplayScreen::setVideoPosition(qint64 position)
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"setVideoPosition",Q_ARG(QVariant,position));
}

void ProjectorDisplayScreen::positionControls(DisplayControlsSettings &dSettings)
{
    //mySettings = dSettings;

    // set icon size
    int buttonSize(dSettings.buttonSize);
    if(buttonSize == 0)
        buttonSize = 16;
    else if(buttonSize == 1)
        buttonSize = 24;
    else if(buttonSize == 2)
        buttonSize = 32;
    else if(buttonSize == 3)
        buttonSize = 48;
    else if(buttonSize == 4)
        buttonSize = 64;
    else if(buttonSize == 5)
        buttonSize = 96;
    else
        buttonSize = 48;


    // calculate button position
    int y(this->height()), x(this->width()), margin(20);

    // calculate y position
    if(dSettings.alignmentV==0)//top
        y = margin;
    else if(dSettings.alignmentV==1)//middle
        y = (y-buttonSize)/2;
    else if(dSettings.alignmentV==2)//buttom
        y = y-buttonSize-margin;
    else
        y = y-buttonSize-margin;

    // calculate x position
    int xt((buttonSize*3)+20); //total width of the button group
    if(dSettings.alignmentH==0)
        x = margin;
    else if(dSettings.alignmentH==1)
        x = (x-xt)/2;
    else if (dSettings.alignmentH==2)
        x = x-xt-margin;
    else
        x = (x-xt)/2;

    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"positionControls",Q_ARG(QVariant,x),Q_ARG(QVariant,y),Q_ARG(QVariant,buttonSize),Q_ARG(QVariant,dSettings.opacity));

}

void ProjectorDisplayScreen::setControlsVisible(bool visible)
{
    QObject *root = dispView->rootObject();
    QMetaObject::invokeMethod(root,"setControlsVisible",Q_ARG(QVariant,visible));
}

void ProjectorDisplayScreen::setClockVisible(bool visible)
{
    dispView->rootObject()->setProperty("clockVisible", visible);
}
