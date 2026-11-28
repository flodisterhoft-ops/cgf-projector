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

#include <QFileDialog>
#include <QMessageBox>
#include "../headers/picturewidget.hpp"
#include "../headers/presentationimport.hpp"
#include "ui_picturewidget.h"

PictureWidget::PictureWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::PictureWidget)
{
    ui->setupUi(this);
    loadSlideShows();
    ui->pushButtonGoLive->setEnabled(false);
    updateRotateButtons();
}

PictureWidget::~PictureWidget()
{
    delete ui;
}

void PictureWidget::changeEvent(QEvent *e)
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

void PictureWidget::loadSlideShows()
{
    QSqlQuery sq;
    sq.exec("SELECT * FROM SlideShows");
    QStringList sl;

    slideShows.clear();

    while(sq.next())
    {
        SlideShowInfo ssi;
        ssi.slideSwId = sq.value(0).toInt();
        ssi.name = sq.value(1).toString();
        ssi.info = sq.value(2).toString();
        slideShows.append(ssi);
        sl.append( sq.value(1).toString());
    }
    ui->listWidgetSlideShow->clear();
    ui->listWidgetSlideShow->addItems(sl);
}

// The list shows one row per slide. A slide of an imported presentation can
// be several stills, one for each step of its animations (slideRows in
// slideshow.hpp): its row shows the last of them, the finished slide, and
// Remove, the arrows and Rotate work on all of them together.
int PictureWidget::firstStill(int row) const
{
    return rows.value(row, -1);
}

int PictureWidget::lastStill(int row) const
{
    return lastStillOfRow(rows, row, slides.count());
}

void PictureWidget::refreshSlideList(int selectRow)
{
    rows = slideRows(slides);
    ui->listWidgetSlides->clear();
    ui->listWidgetSlides->setIconSize(QSize(100,100));
    for(int r = 0; r < rows.count(); ++r)
    {
        QListWidgetItem *itm = new QListWidgetItem;
        itm->setIcon(QIcon(slides.at(lastStill(r)).imageSmall));
        ui->listWidgetSlides->addItem(itm);
    }
    if(selectRow >= 0)
        ui->listWidgetSlides->setCurrentRow(qMin(selectRow, int(rows.count()) - 1));
    updateRotateButtons();
}

void PictureWidget::on_listWidgetSlides_currentRowChanged(int currentRow)
{
    if(currentRow>=0 && currentRow<rows.count())
    {
        const SlideShowItem &shown = slides.at(lastStill(currentRow));
        int pw,ph;
        pw = shown.imagePreview.width();
        ph = shown.imagePreview.height();
        if(pw>300 || ph>200)
            ui->labelPreview->setPixmap(shown.imagePreview.scaled(300,200,Qt::KeepAspectRatio));
        else
            ui->labelPreview->setPixmap(shown.imagePreview);
        ui->labelPixInfo->setText(tr("Preview slide: ")+slides.at(firstStill(currentRow)).name);
    }
    updateRotateButtons();
}

void PictureWidget::updateRotateButtons()
{
    const int c = ui->listWidgetSlides->currentRow();
    const bool on = c >= 0 && c < rows.count();
    ui->pushButtonRotateLeft->setEnabled(on);
    ui->pushButtonRotateRight->setEnabled(on);
}

void PictureWidget::on_pushButtonRotateLeft_clicked()
{
    rotateSlide(-90);
}

void PictureWidget::on_pushButtonRotateRight_clicked()
{
    rotateSlide(90);
}

void PictureWidget::rotateSlide(int degrees)
{
    const int c = ui->listWidgetSlides->currentRow();
    if(c < 0 || c >= rows.count())
        return;
    for(int i = firstStill(c); i <= lastStill(c); ++i)
    {
        SlideShowItem &slide = slides[i];
        slide.rotate(degrees);

        // A slide of a saved slide show keeps the turn. A schedule file carries
        // slide ids from the day it was saved, which may be other slides by now,
        // so its slides are never written to the database.
        if(!previewFromSchedule && slide.slideId != -1 && slide.saveImages())
            slide.rotated = false;
        emit slideRotated(slide, i, degrees);
    }
    ui->listWidgetSlides->item(c)->setIcon(QIcon(slides.at(lastStill(c)).imageSmall));
    on_listWidgetSlides_currentRowChanged(c);
}

void PictureWidget::on_listWidgetSlides_doubleClicked(const QModelIndex &index)
{
    sendToProjector();
}

void PictureWidget::on_pushButtonAddImages_clicked()
{
    QStringList imageFilePaths = QFileDialog::getOpenFileNames(this,tr("Select Images to Open"),".",
                                                               tr("Images(%1)").arg(getSupportedImageFormats()));
    addImageFiles(imageFilePaths);
}

void PictureWidget::addImageFiles(const QStringList &imageFilePaths)
{
    if(imageFilePaths.count()>0)
    {
        this->setCursor(Qt::WaitCursor);
        int i(0);
        const int firstNew = slides.count();
        const int selected = ui->listWidgetSlides->currentRow();
        QProgressDialog progress(tr("Adding files..."), tr("Cancel"), 0, imageFilePaths.count(), this);
        foreach(const QString &file, imageFilePaths)
        {
            ++i;
            progress.setValue(i);

            SlideShowItem sd;
            if(!sd.loadFile(file, mySettings.resize ? mySettings.boundWidth : 0))
                continue; // unreadable or unsupported file: don't add a blank slide

            // add to slideshow
            slides.append(sd);
        }
        this->setCursor(Qt::ArrowCursor);
        ui->pushButtonGoLive->setEnabled(true);
        // Keep the selection, or show the first new picture, ready to turn or send.
        refreshSlideList(-1);
        if(selected >= 0)
            ui->listWidgetSlides->setCurrentRow(selected);
        else if(firstNew < slides.count())
            ui->listWidgetSlides->setCurrentRow(rowOfStill(rows, firstNew));
    }
}

void PictureWidget::on_pushButtonRemoveImage_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>=0 && c<rows.count())
    {
        const int first = firstStill(c);
        slides.remove(first, lastStill(c) - first + 1);
        refreshSlideList(c);
    }
}

void PictureWidget::on_pushButtonClearImages_clicked()
{
    ui->listWidgetSlideShow->setCurrentRow(-1);
    slides.clear();
    refreshSlideList(-1);
    ui->labelPreviewSlideShow->clear();
    ui->labelPreview->clear();
    ui->labelPreview->setText(tr("Picture Preview"));
    ui->labelPixInfo->setText(tr("Preview slide: "));
    ui->pushButtonGoLive->setEnabled(false);
    currentSlideShow = SlideShow();
    previewFromSchedule = false;
    updateRotateButtons();
}

void PictureWidget::on_pushButtonMoveUp_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>0 && c<rows.count())
    {
        moveSlideRowUp(slides, rows, c);
        refreshSlideList(c-1);
    }
}

void PictureWidget::on_pushButtonMoveDown_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>=0 && c+1<rows.count())
    {
        // Down is the row below moving up.
        moveSlideRowUp(slides, rows, c+1);
        refreshSlideList(c+1);
    }
}

void PictureWidget::on_pushButtonGoLive_clicked()
{
    sendToProjector();
}

void PictureWidget::sendToProjector()
{
    // The selected slide from its first still.
    const int c = ui->listWidgetSlides->currentRow();
    emit sendSlideShow(slides, c < 0 ? -1 : firstStill(c), currentSlideShow.name);
}

void PictureWidget::loadSlideShow(int ss_id)
{
    SlideShow ss;
    ss.loadSlideShow(ss_id);
    previewFromSchedule = false;
    sendToPreview(ss);
    ui->pushButtonGoLive->setEnabled(true);
}

void PictureWidget::sendToPreview(SlideShow &sshow)
{
    currentSlideShow = sshow;
    slides = currentSlideShow.slides;
    ui->labelPreviewSlideShow->setText(tr("Slide Show: %1").arg(currentSlideShow.name));
    refreshSlideList(0);
}

void PictureWidget::sendToPreviewFromSchedule(SlideShow &sshow)
{
    ui->listWidgetSlideShow->clearSelection();
    previewFromSchedule = true;
    sendToPreview(sshow);
}

SlideShow PictureWidget::getCurrentSlideshow()
{
    currentSlideShow.slides = slides;
    return currentSlideShow;
}

bool PictureWidget::isSlideShowSelected()
{
    int css = ui->listWidgetSlideShow->selectionModel()->selection().count();
    if(css>0)
        return true;
    else
        return false;
}

void PictureWidget::deleteSlideShow()
{
    QSqlQuery sq;
    int ssId = slideShows.at(ui->listWidgetSlideShow->currentRow()).slideSwId;
    // Videos and sounds of an imported presentation go with it, unless
    // another slide show uses the same files.
    const QStringList media = PresentationSteps::mediaOf(ssId);
    sq.exec(QString("DELETE FROM SlideShows WHERE id = %1").arg(ssId));
    sq.exec(QString("DELETE FROM Slides WHERE ss_id = %1").arg(ssId));
    PresentationSteps::removeUnusedMedia(media);
    loadSlideShows();
    on_pushButtonClearImages_clicked();
}

void PictureWidget::on_listWidgetSlideShow_itemSelectionChanged()
{
    int cRow = ui->listWidgetSlideShow->currentRow();
    if(cRow>=0 && ui->listWidgetSlideShow->selectedItems().count()>0)
        loadSlideShow(slideShows.at(cRow).slideSwId);
}

void PictureWidget::on_listWidgetSlideShow_doubleClicked(const QModelIndex &index)
{
    currentSlideShow.slides = slides;
    emit sendToSchedule(currentSlideShow);
}

void PictureWidget::on_pushButtonImport_clicked()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Import PowerPoint or PDF"),
                                                      QString(), PresentationImport::fileFilter());
    if(!file.isEmpty())
        importPresentation(file);
}

void PictureWidget::importPresentation(const QString &file)
{
    // Saved as a slide show named after the file, selected and ready to go live.
    QString error;
    SlideShow show;
    ui->pushButtonImport->setEnabled(false);
    show.slides = PresentationImport::run(file, mySettings, this, error);
    ui->pushButtonImport->setEnabled(true);
    if(show.slides.isEmpty())
    {
        if(!error.isEmpty())
            QMessageBox::warning(this, tr("Import PowerPoint or PDF"), error);
        return;
    }

    // Importing the same file again keeps the first one: "Name (2)".
    const QString base = QFileInfo(file).completeBaseName();
    auto taken = [this](const QString &name) {
        for(const SlideShowInfo &s : std::as_const(slideShows))
            if(s.name == name)
                return true;
        return false;
    };
    show.name = base;
    for(int n = 2; taken(show.name); ++n)
        show.name = QString("%1 (%2)").arg(base).arg(n);
    show.info = QFileInfo(file).fileName();
    show.saveSideShow(tr("Saving slide show..."), this, QList<int>());

    loadSlideShows();
    for(int i = 0; i < slideShows.count(); ++i)
    {
        if(slideShows.at(i).slideSwId == show.slideShowId)
            ui->listWidgetSlideShow->setCurrentRow(i);
    }
}
