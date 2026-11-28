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

#ifndef SLIDESHOW_HPP
#define SLIDESHOW_HPP

#include <QtSql>
#include <QString>
#include <QPixmap>
#include <QDebug>
#include <QProgressDialog>

#include "spfunctions.hpp"
#include "presentationsteps.hpp"

class SlideShowItem
{
public:
    SlideShowItem();
    int slideId;
    int order;
    QString name;
    QString path;
    QPixmap image;
    QPixmap imageSmall;
    QPixmap imagePreview;
    // Turned since it was loaded: saveSideShow writes its pictures again.
    bool rotated;

    // Reads a picture file the right way up; with a bound above 0, larger
    // pictures are scaled down to fit it. False for an unreadable file.
    bool loadFile(const QString &file, int bound);
    // Quarter turns: 90 is clockwise, -90 counter-clockwise.
    void rotate(int degrees);
    // Writes the pictures of a slide that is already in the database.
    bool saveImages() const;
    // For a slide of an imported presentation: which step of which slide it
    // is and how it comes on. Kept as text in path, so it is saved with
    // slide shows and schedules as they are. Not valid for other pictures.
    SlideStep step() const { return SlideStep::fromText(path); }
};

class SlideShowInfo
{
public:
    SlideShowInfo();
    int slideSwId;
    QString name;
    QString info;
};

class SlideShow
{
public:
    SlideShow();
    int slideShowId;
    QString name;
    QString info;
    QList<SlideShowItem> slides;

public slots:
    void loadSlideShow(int id);
    void saveSideShow(QString savelbl, QWidget *ptW, QList<int> delList);
};

// The stills of a slide show as the rows of a list: one row per slide. A
// slide of an imported presentation can be several stills in a row, one for
// each step of its animations; the lists show the last of them, the finished
// slide, the way PowerPoint lists slides. Any other picture is a row of its
// own. slideRows gives the index of each row's first still.
QList<int> slideRows(const QList<SlideShowItem> &slides);
int lastStillOfRow(const QList<int> &rows, int row, int count);
int rowOfStill(const QList<int> &rows, int still);
// Moves a row, with all its stills, in front of the row above it.
void moveSlideRowUp(QList<SlideShowItem> &slides, const QList<int> &rows, int row);

#endif // SLIDESHOW_HPP
