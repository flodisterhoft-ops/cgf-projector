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

#include "../headers/slideshoweditor.hpp"
#include "ui_slideshoweditor.h"

SlideShowEditor::SlideShowEditor(QWidget *parent) :
    //    QWidget(parent),
    QDialog(parent),
    ui(new Ui::SlideShowEditor)
{
    ui->setupUi(this);
    updateButtonState();
}

SlideShowEditor::~SlideShowEditor()
{
    delete ui;
}

void SlideShowEditor::setSlideShow(SlideShow ss)
{
    editSS = ss;
    deleteList.clear();
    loadSlideShow();
}

void SlideShowEditor::loadSlideShow()
{
    ui->lineEditTitle->setText(editSS.name);
    ui->plainTextEditInfo->setPlainText(editSS.info);

    reloadSlides();
    ui->listWidgetSlides->setCurrentRow(0);
}

// One row per slide: an imported presentation's slide can be several stills
// (its animation steps, slideRows in slideshow.hpp). The row shows the
// finished slide, and Remove, the arrows and Rotate work on all its stills.
int SlideShowEditor::firstStill(int row) const
{
    return rows.value(row, -1);
}

int SlideShowEditor::lastStill(int row) const
{
    return lastStillOfRow(rows, row, editSS.slides.count());
}

void SlideShowEditor::reloadSlides()
{
    rows = slideRows(editSS.slides);
    ui->listWidgetSlides->clear();
    ui->listWidgetSlides->setIconSize(QSize(100,100));
    for(int r = 0; r < rows.count(); ++r)
    {
        QListWidgetItem *itm = new QListWidgetItem;
        itm->setIcon(QIcon(editSS.slides.at(lastStill(r)).imageSmall));
        ui->listWidgetSlides->addItem(itm);
    }
}

void SlideShowEditor::updateButtonState()
{
    int c = ui->listWidgetSlides->currentRow();
    int ct = rows.count();

    ui->pushButtonRemoveImage->setEnabled(c>=0 && c<ct);
    ui->pushButtonMoveUp->setEnabled(c>0 && c<ct);
    ui->pushButtonMoveDown->setEnabled(c>=0 && (c<ct-1));
    ui->pushButtonRotateLeft->setEnabled(c>=0 && c<ct);
    ui->pushButtonRotateRight->setEnabled(c>=0 && c<ct);
}

void SlideShowEditor::on_pushButtonRotateLeft_clicked()
{
    rotateSlide(-90);
}

void SlideShowEditor::on_pushButtonRotateRight_clicked()
{
    rotateSlide(90);
}

void SlideShowEditor::rotateSlide(int degrees)
{
    // Saved with the slide show: saveSideShow writes turned slides again.
    const int c = ui->listWidgetSlides->currentRow();
    if(c < 0 || c >= rows.count())
        return;
    for(int i = firstStill(c); i <= lastStill(c); ++i)
        editSS.slides[i].rotate(degrees);
    ui->listWidgetSlides->item(c)->setIcon(QIcon(editSS.slides.at(lastStill(c)).imageSmall));
    on_listWidgetSlides_currentRowChanged(c);
}

void SlideShowEditor::on_pushButtonAddImages_clicked()
{
    QStringList imageFilePaths = QFileDialog::getOpenFileNames(this,tr("Select Images to Open"),".",
                                                               tr("Images(%1)").arg(getSupportedImageFormats()));
    if(imageFilePaths.count()>0)
    {
        this->setCursor(Qt::WaitCursor);
        int i(0);
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
            editSS.slides.append(sd);
        }
        this->setCursor(Qt::ArrowCursor);
        reloadSlides();
        ui->listWidgetSlides->setCurrentRow(selected);
        updateButtonState();
    }

}

void SlideShowEditor::on_pushButtonRemoveImage_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>=0 && c<rows.count())
    {
        const int first = firstStill(c), size = lastStill(c) - first + 1;
        for(int i = first; i < first + size; ++i)
            deleteList.append(editSS.slides.at(i).slideId);
        editSS.slides.remove(first, size);
        reloadSlides();
        updateButtonState();
    }
}

void SlideShowEditor::on_pushButtonMoveUp_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>0 && c<rows.count())
    {
        moveSlideRowUp(editSS.slides, rows, c);
        reloadSlides();
        ui->listWidgetSlides->setCurrentRow(c-1);
        updateButtonState();
    }
}

void SlideShowEditor::on_pushButtonMoveDown_clicked()
{
    int c = ui->listWidgetSlides->currentRow();
    if(c>=0 && c+1<rows.count())
    {
        // Down is the row below moving up.
        moveSlideRowUp(editSS.slides, rows, c+1);
        reloadSlides();
        ui->listWidgetSlides->setCurrentRow(c+1);
        updateButtonState();
    }
}

void SlideShowEditor::on_pushButtonSave_clicked()
{
    // Make sure that slide show is not empty before saving.
    if(editSS.slides.count()<=0)
    {
        //todo: message box saying that title cannot be empty
        close();
        return;
    }

    editSS.name = ui->lineEditTitle->text().trimmed();

    // Make sure that slide show Title is not empty before saving.
    if(editSS.name.isEmpty())
    {
        QMessageBox mb(this);
        mb.setText(tr("Slide show title cannot be left empty.\nPlease enter a title."));
        mb.setWindowTitle(tr("Slide show title is missing"));
        mb.setIcon(QMessageBox::Warning);
        mb.exec();
        ui->lineEditTitle->setFocus();
        return;
    }

    editSS.info = ui->plainTextEditInfo->toPlainText();
    editSS.saveSideShow(tr("Saving Slide Show"),this,deleteList);
    close();
}

void SlideShowEditor::on_pushButtonCancel_clicked()
{
    close();
}

void SlideShowEditor::on_listWidgetSlides_currentRowChanged(int currentRow)
{
    if(currentRow>=0 && currentRow<rows.count())
    {
        ui->labelPreview->setPixmap(editSS.slides.at(lastStill(currentRow)).imagePreview);
        ui->labelPixInfo->setText(tr("Preview slide: %1").arg(editSS.slides.at(firstStill(currentRow)).name));
        updateButtonState();
    }
}
