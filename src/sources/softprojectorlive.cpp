/***************************************************************************
//
//    CGF Projector - network bridge
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

// The main window's side of the network bridge: what the projector shows,
// handed to LiveBridge. Kept out of softprojector.cpp so the network work
// stays in its own files; softprojector.cpp only calls publishLive().

#include "../headers/softprojector.hpp"
#include "../headers/livebridge.hpp"
#include "../headers/librarypush.hpp"
#include "ui_softprojector.h"

#include <QJsonArray>

void SoftProjector::setupLiveBridge()
{
    liveBridge = new LiveBridge(this);
    connect(pds1, qOverload<QMediaPlayer::PlaybackState>(&ProjectorDisplayScreen::videoPlaybackStateChanged),
            liveBridge, &LiveBridge::videoStateChanged);
    connect(pds1, qOverload<qint64>(&ProjectorDisplayScreen::videoPositionChanged),
            liveBridge, &LiveBridge::videoPositionChanged);
    connect(pds1, qOverload<qint64>(&ProjectorDisplayScreen::videoDurationChanged),
            liveBridge, &LiveBridge::videoDurationChanged);
    connect(pds1, &ProjectorDisplayScreen::frameShown, liveBridge, &LiveBridge::frameShown);
    publishLive();
}

// textCleared: the operator pressed Clear (text gone, background stays). A
// redraw (updateScreen) ends it; turning the output off and on keeps it.
void SoftProjector::publishLive(bool textCleared)
{
    liveTextCleared = textCleared;
    refreshUpdateBar();
    if(libraryPush)
        libraryPush->setBibleOnScreen(bibleOnScreen());
    if(!liveBridge)
        return;

    // Visible follows the operator (Show/Hide and the output button), not the
    // cable: a dropped projector link must not blank the stream.
    const bool visible = showing && (isSingleScreen || ui->actionCloseDisplay->isChecked());
    // The projector's Bibles, in screen order; pages default to its first.
    QJsonArray projected;
    for(const QString &id : {mySettings.bibleSets.primaryBible, mySettings.bibleSets.secondaryBible,
                             mySettings.bibleSets.trinaryBible})
        if(!id.isEmpty() && id != QLatin1String("none") && !projected.contains(id))
            projected.append(id);
    QJsonObject live{{"visible", visible}, {"cleared", textCleared}, {"kind", "none"},
                     {"bibles", projected}};
    QPixmap picture;
    QString videoPath;
    const int row = ui->listShow->currentRow();

    switch(pType)
    {
    case BIBLE:
    {
        QJsonArray ids;
        const QStringList &idList = bibleWidget->bible.currentIdList;
        for(int i = 0; i < ui->listShow->count() && i < idList.count(); ++i)
            if(ui->listShow->item(i)->isSelected())
                ids.append(idList.at(i));
        if(!ids.isEmpty())
        {
            live["kind"] = "bible";
            live["bible"] = QJsonObject{{"verseIds", ids}, {"projected", projected}};
        }
        break;
    }
    case SONG:
    {
        const int count = current_song.getSongTextList().count();
        if(row >= 0 && row < count)
        {
            const Stanza stanza = current_song.getStanza(row);
            live["kind"] = "song";
            live["song"] = QJsonObject{{"id", current_song.songID}, {"number", current_song.number},
                                       {"title", current_song.title},
                                       {"songbook", current_song.songbook_name},
                                       {"stanzaTitle", stanza.stanzaTitle}, {"text", stanza.stanza},
                                       {"index", row}, {"count", count}};
        }
        break;
    }
    case ANNOUCEMENT:
    {
        const int count = currentAnnounce.getAnnounceList().count();
        if(row >= 0 && row < count)
        {
            live["kind"] = "announcement";
            live["announcement"] = QJsonObject{{"title", currentAnnounce.title},
                                               {"text", currentAnnounce.getAnnounceSlide(row).text},
                                               {"index", row}, {"count", count}};
        }
        break;
    }
    case PICTURE:
        if(row >= 0 && row < pictureShowList.count())
        {
            const SlideShowItem &item = pictureShowList.at(row);
            picture = item.image;
            live["kind"] = "picture";
            live["picture"] = QJsonObject{{"name", item.name}, {"index", row},
                                          {"count", int(pictureShowList.count())},
                                          {"key", QString::number(item.image.cacheKey())}};
        }
        break;
    case VIDEO:
        if(currentVideo.filePath.isLocalFile())
        {
            videoPath = currentVideo.filePath.toLocalFile();
            live["kind"] = "video";
            live["video"] = QJsonObject{{"name", currentVideo.fileName}};
        }
        break;
    }

    liveBridge->publish(live, picture, videoPath);
}
