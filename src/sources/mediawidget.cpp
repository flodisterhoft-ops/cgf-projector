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

#include <QMediaMetaData>
#include <QMediaDevices>
#include <QAudioDevice>

#include "../headers/mediawidget.hpp"
#include "ui_mediawidget.h"

MediaWidget::MediaWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MediaWidget)//,
{
    ui->setupUi(this);
    isReadyToPlay = false;
    goLivePending = false;
    player = new QMediaPlayer(this);
    // Qt 6 plays nothing audible unless the player has an audio output.
    audioOutput = new QAudioOutput(this);
    player->setAudioOutput(audioOutput);
    // It also stays on the device that was the default at start-up; follow
    // the Windows default (HDMI sound goes away with the projector link).
    connect(new QMediaDevices(this), &QMediaDevices::audioOutputsChanged, this, [this] {
        audioOutput->setDevice(QMediaDevices::defaultAudioOutput());
    });

    mediaControls = new MediaControl(this);
     ui->horizontalLayoutControls->addWidget(mediaControls);

    // Function-pointer connects: the old string connects named Qt 5 signals and
    // slots (videoAvailableChanged, stateChanged, error, setVolume, setMuted)
    // that no longer exist in Qt 6, so they silently did nothing.
    connect(player, &QMediaPlayer::metaDataChanged, this, &MediaWidget::updateInfo);
    connect(player, &QMediaPlayer::mediaStatusChanged, this, &MediaWidget::statusChanged);
    connect(player, &QMediaPlayer::hasVideoChanged, this, &MediaWidget::hasVideoChanged);
    connect(player, &QMediaPlayer::errorOccurred, this, &MediaWidget::displayErrorMessage);
    connect(player, &QMediaPlayer::playbackStateChanged, mediaControls, &MediaControl::updatePlayerState);
    connect(player, &QMediaPlayer::durationChanged, mediaControls, &MediaControl::setMaximumTime);
    connect(player, &QMediaPlayer::positionChanged, mediaControls, &MediaControl::updateTime);

    connect(mediaControls, &MediaControl::play, player, &QMediaPlayer::play);
    connect(mediaControls, &MediaControl::pause, player, &QMediaPlayer::pause);
    connect(mediaControls, &MediaControl::stop, player, &QMediaPlayer::stop);
    connect(mediaControls, &MediaControl::timeChanged, player, &QMediaPlayer::setPosition);
    connect(mediaControls, &MediaControl::muted, audioOutput, &QAudioOutput::setMuted);
    connect(mediaControls, &MediaControl::volumeChanged, this, [this](int level) {
        audioOutput->setVolume(level / 100.0f);
    });
  videoWidget = new VideoPlayerWidget(this);
    player->setVideoOutput(videoWidget);

    mediaControls->setVolume(100);


       ui->verticalLayoutMedia->addWidget(videoWidget);
        videoWidget->setVisible(false);




    /**********************************************/

    ui->pushButtonGoLive->setEnabled(false);

    playIcon = QIcon(":icons/icons/play.png");
    pauseIcon = QIcon(":icons/icons/pause.png");
    muteIcon = QIcon(":icons/icons/speakerMute.png");
    unmuteIcon = QIcon(":icons/icons/speaker.png");

    QPalette palette;
    palette.setBrush(QPalette::WindowText, Qt::white);

    ui->labelInfo->setStyleSheet("border-image:url(:icons/icons/playerScreen.png)");
    ui->labelInfo->setPalette(palette);
    ui->labelInfo->setText(tr("<center>No media</center>"));


//    connect(&mediaPlayer, SIGNAL(metaDataChanged()), this, SLOT(updateInfo()));
//    connect(videoWidget, SIGNAL(handleDrops(QDropEvent*)), this, SLOT(handleDrop(QDropEvent*)));

    setAcceptDrops(true);

    audioExt = "*.mp3 *.acc *.ogg *.oga *.wma *.wav *.asf *.mka";
    videoExt = "*.wmv *.avi *.mkv *.flv *.mp4 *.mpg *.mpeg *.mov *.ogv *.ts";
    loadMediaLibrary();
}

MediaWidget::~MediaWidget()
{
    delete player;
    delete videoWidget;
    delete mediaControls;
//    delete timeSlider;
//    delete volumeSlider;
    delete ui;
}

void MediaWidget::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void MediaWidget::loadMediaLibrary()
{
    QSqlQuery sq;
    sq.exec("SELECT long_path, short_path FROM Media");
    while(sq.next())
    {
        // Rows hold plain local paths. QUrl("C:/...") would parse "C:" as a URL
        // scheme, so convert explicitly (and still accept rows that are URLs).
        QString path = sq.value(0).toString();
        mediaDbPaths.append(path);
        mediaFilePaths.append(path.contains("://") ? QUrl(path) : QUrl::fromLocalFile(path));
        mediaFileNames.append(sq.value(1).toString());
    }
    ui->listWidgetMediaFiles->clear();
    ui->listWidgetMediaFiles->addItems(mediaFileNames);
}

void MediaWidget::statusChanged(QMediaPlayer::MediaStatus status)
{
    switch (status) {
    case QMediaPlayer::LoadingMedia:
        isReadyToPlay = false;
        ui->labelInfo->setText(QString("<center><strong><font color=#49fff9>%1</font>")
                               .arg(tr("Loading...")));
        break;
    case QMediaPlayer::BufferingMedia:
        ui->labelInfo->setText(QString("<center><strong><font color=#49fff9>%1</font>")
                               .arg(tr("Loading...")));
        break;
    case QMediaPlayer::StalledMedia:
        ui->labelInfo->setText(QString("<center><strong><font color=#49fff9>%1</font>")
                               .arg(tr("Media Stalled")));
        break;
    case QMediaPlayer::InvalidMedia:
        goLivePending = false;
        displayErrorMessage();
        break;
    case QMediaPlayer::LoadedMedia:
    case QMediaPlayer::BufferedMedia:
        isReadyToPlay = true;
        updateInfo(); // replace the "Loading..." text with the file details
        if(goLivePending)
        {
            // Defer until this load notification is finished: Qt applies the
            // pause() that setMediaFromSchedule() requested during loading
            // right after reporting LoadedMedia, which would undo a play()
            // issued from here. Picking another file meanwhile cancels it.
            QTimer::singleShot(0, this, [this]() {
                if(goLivePending)
                    goLiveFromSchedule();
            });
        }
        break;
    default:
        break;
    }
}

void MediaWidget::displayErrorMessage()
{
    QString errMsg1 = tr("Possible Fail reasons:");
    QString errMsg2 = tr(" - Unsupported media format");
    QString errMsg3 = tr(" - Media file no longer exists or invalid path to file");

    if(!player->errorString().isEmpty())
    {
        errMsg1 = player->errorString();
        errMsg2 = errMsg3 = "";
    }
    ui->labelInfo->setText(QString("<center><strong><font color=#ff5555>%1:</font></strong><br>"
                                   "<font color=#49fff9>%2</font><br>%3<br>%4<br>%5</center>")
                           .arg(tr("ERROR Playing media file"))
                           .arg(currentMediaUrl.fileName())
                           .arg(errMsg1)
                           .arg(errMsg2)
                           .arg(errMsg3));
}

void MediaWidget::handleDrop(QDropEvent *e)
{
    QList<QUrl> urls = e->mimeData()->urls();
    QStringList fileList;
    QStringList fileNameList;

    // Make sure that only supported files get to media player
    QString fext = audioExt + " " + videoExt;
    fext.remove("*");
    fext.replace(" ","$|");
    fext.append("$");
    QRegularExpression rx(fext);
    rx.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    foreach(const QUrl &url, urls)
    {
        QString fp = url.toLocalFile();
        if(fp.contains(rx))
        {
            QFileInfo fn(fp);
            fileList.append(fp);
            fileNameList.append(fn.fileName());
        }
    }

    // Add files to library
    if(fileList.count()>0)
    {
        int mcount = ui->listWidgetMediaFiles->count(); // get total media count
        insertFiles(fileList);
        ui->listWidgetMediaFiles->setCurrentRow(mcount); // select first in list of just added and play it
    }
}

void MediaWidget::dropEvent(QDropEvent *e)
{
    if (e->mimeData()->hasUrls() && e->proposedAction() != Qt::LinkAction) {
        e->acceptProposedAction();
        handleDrop(e);
    } else {
        e->ignore();
    }
}

void MediaWidget::dragEnterEvent(QDragEnterEvent *e)
{
    dragMoveEvent(e);
}

void MediaWidget::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        if (e->proposedAction() == Qt::CopyAction || e->proposedAction() == Qt::MoveAction){
            e->acceptProposedAction();
        }
    }
}


void MediaWidget::playFile(QUrl filePath)
{
    goLivePending = false;
    // stop() re-reports LoadedMedia for the previous file, so clear the ready
    // flag only afterwards; otherwise the old file's state (e.g. "has video")
    // was used to decide how to go live with the new one.
    player->stop();
    isReadyToPlay = false;
    currentMediaUrl = filePath;
    QUrl m(filePath);
    player->setSource(m);
}

void MediaWidget::updateInfo()
{
    int maxLength = 50;
    QString font = "<font color=#49fff9>";

    QString fName = player->source().fileName();
    QMediaMetaData md = player->metaData();
    QString tAlbum = md.stringValue(QMediaMetaData::AlbumTitle);
    QString tTitle = md.stringValue(QMediaMetaData::Title);
    QString tArtist = md.stringValue(QMediaMetaData::AlbumArtist);
    int tBitrate = md.stringValue(QMediaMetaData::AudioBitRate).toInt();

    if (fName.length() > maxLength)
        fName = fName.left(maxLength) + "...";

    if (tAlbum.length() > maxLength)
        tAlbum = tAlbum.left(maxLength) + "...";

    if (tTitle.length() > maxLength)
        tTitle = tTitle.left(maxLength) + "...";

    if (tArtist.length() > maxLength)
        tArtist = tArtist.left(maxLength) + "...";

    QString file;
    if(!fName.isEmpty())
        file = "File: " + font + fName + "<br></font>";

    QString album;
    if(!tAlbum.isEmpty())
        album = "Album: " + font + tAlbum + "<br></font>";

    QString title;
    if(!tTitle.isEmpty())
    {
        title = "Title: " + font + tTitle + "<br></font>";
        file.clear();
    }

    QString artist;
    if (!tArtist.isEmpty())
        artist = "Artist: " + font + tArtist + "<br></font>";

    QString bitrate;
    if (tBitrate != 0)
        bitrate = "Bitrate: " + font + QString::number(tBitrate/1000) + "kbit</font>";

    ui->labelInfo->setText(file + album + title + artist + bitrate);

}

void MediaWidget::insertFiles(QStringList &files)
{
    // Insert filenames and path into appropriate lists and database
    QSqlQuery sq;
    foreach(const QString &file, files)
    {
        if(mediaDbPaths.contains(file))
            continue; // already in the library

        QFileInfo f(file);
        // Bound values: a file name with an apostrophe broke the old string-built SQL.
        sq.prepare("INSERT INTO Media (long_path, short_path) VALUES (?, ?)");
        sq.addBindValue(file);
        sq.addBindValue(f.fileName());
        if(!sq.exec())
        {
            qWarning() << "Could not add media file" << file << sq.lastError().text();
            continue;
        }
        mediaDbPaths.append(file);
        mediaFileNames.append(f.fileName());
        mediaFilePaths.append(QUrl::fromLocalFile(file));
        ui->listWidgetMediaFiles->addItem(f.fileName());
    }
}

void MediaWidget::hasVideoChanged(bool bHasVideo)
{
    qDebug()<<"hasVideoChanged"<<bHasVideo;
    if(!bHasVideo && videoWidget->isFullScreen())
        videoWidget->setFullScreen(false);
    ui->labelInfo->setVisible(!bHasVideo);
    ui->pushButtonGoLive->setEnabled(bHasVideo);
    videoWidget->setVisible(bHasVideo);
//    isReadyToPlay = true;
}

void MediaWidget::prepareForProjection()
{
    player->pause();
    VideoInfo v;
//    v.fileName = mediaFileNames.at(ui->listWidgetMediaFiles->currentRow());
//    v.filePath = mediaFilePaths.at(ui->listWidgetMediaFiles->currentRow());
    v.fileName = currentMediaUrl.fileName();
    v.filePath = currentMediaUrl.toString();
    emit toProjector(v);
}

void MediaWidget::on_pushButtonGoLive_clicked()
{
    prepareForProjection();
}

void MediaWidget::addToLibrary()
{
    QStringList mfp = QFileDialog::getOpenFileNames(this,tr("Select Music/Video Files to Open"),".",
                                                    tr("Media Files (%1);;Audio Files (%2);;Video Files (%3)")
                                                    .arg(audioExt + " " + videoExt) // media files
                                                    .arg(audioExt) // audio files
                                                    .arg(videoExt)); // video files

    if(mfp.count()>0)
        insertFiles(mfp);
}

void MediaWidget::removeFromLibrary()
{
    int cm = ui->listWidgetMediaFiles->currentRow();
    if(cm>=0 && cm<mediaDbPaths.count())
    {
        // Delete by full path: deleting by file name removed every same-named file
        // in other folders, and the old string-built SQL broke on apostrophes.
        QSqlQuery sq;
        sq.prepare("DELETE FROM Media WHERE long_path = ?");
        sq.addBindValue(mediaDbPaths.at(cm));
        sq.exec();
        mediaDbPaths.removeAt(cm);
        mediaFilePaths.removeAt(cm);
        mediaFileNames.removeAt(cm);

        ui->listWidgetMediaFiles->setCurrentRow(-1);
        ui->listWidgetMediaFiles->clear();
        if(mediaFileNames.count()>0)
            ui->listWidgetMediaFiles->addItems(mediaFileNames);
        player->stop();

        hasVideoChanged(false);

        ui->labelInfo->setText(tr("<center>No media</center>"));
    }
}

void MediaWidget::on_listWidgetMediaFiles_itemSelectionChanged()
{
    int cRow = ui->listWidgetMediaFiles->currentRow();
    if(cRow>=0)
    {
        playFile(mediaFilePaths.at(cRow));
    }
}

void MediaWidget::on_listWidgetMediaFiles_doubleClicked(const QModelIndex &index)
{
    if(ui->pushButtonGoLive->isEnabled())
        prepareForProjection();
}

VideoInfo MediaWidget::getMedia()
{
    VideoInfo v;
    int cm = ui->listWidgetMediaFiles->currentRow();
    if(cm < 0 || cm >= mediaFilePaths.count())
        return v;
    v.fileName = mediaFileNames.at(cm);
    v.filePath = mediaFilePaths.at(cm);
    return v;
}

void MediaWidget::setMediaFromSchedule(VideoInfo &v)
{
    if(currentMediaUrl == v.filePath)
    {
        // Same current Media File, do not update.
        return;
    }
    ui->listWidgetMediaFiles->clearSelection();
    playFile(v.filePath);
    player->pause();
}

void MediaWidget::goLiveFromSchedule()
{
    // The old version slept in a loop on the GUI thread until the file loaded.
    // The load notification can only arrive through the event loop it was
    // blocking, so the whole app froze. Now we wait for statusChanged().
    if(!isReadyToPlay)
    {
        goLivePending = true;
        return;
    }
    goLivePending = false;

    if(player->hasVideo())
        prepareForProjection(); // video goes to the projector
    else
        player->play();         // audio plays from the operator PC
}

bool MediaWidget::isValidMedia()
{
    if(ui->listWidgetMediaFiles->selectionModel()->selectedRows().count() > 0)
        return true;
    else
        return false;
}
