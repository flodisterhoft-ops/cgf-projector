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

#include "../headers/slideshow.hpp"

SlideShowItem::SlideShowItem()
{
    slideId = -1;
    order = -1;
}

SlideShowInfo::SlideShowInfo()
{
}

SlideShow::SlideShow()
{
    slideShowId = -1;
}

void SlideShow::loadSlideShow(int id)
{
    slides.clear();
    slideShowId = id;
    QSqlQuery sq;
    sq.prepare("SELECT name, info FROM SlideShows WHERE id = ?");
    sq.addBindValue(slideShowId);
    sq.exec();
    if(sq.first())
    {
        name = sq.value(0).toString();
        info = sq.value(1).toString();
    }

    sq.prepare("SELECT COUNT(*) FROM Slides WHERE ss_id = ?");
    sq.addBindValue(slideShowId);
    sq.exec();
    int max = sq.first() ? sq.value(0).toInt() : 0;

    QProgressDialog progress;
    progress.setLabelText("Loading Slides...");
    progress.setMaximum(max+1);
    int ct(0);

    // Let SQLite sort by p_order. The old loop ran its body once even when the
    // slideshow had no slides (adding a blank slide), and its manual sort
    // silently dropped any slide whose p_order was not exactly 0..n-1.
    sq.prepare("SELECT id, p_order, name, path, pix, pix_small, pix_prev FROM Slides "
               "WHERE ss_id = ? ORDER BY p_order, id");
    sq.addBindValue(slideShowId);
    sq.exec();
    while(sq.next())
    {
        SlideShowItem si;
        si.slideId = sq.value(0).toInt();
        si.order = sq.value(1).toInt();
        si.name = sq.value(2).toString();
        si.path = sq.value(3).toString();
        si.image.loadFromData(sq.value(4).toByteArray());
        si.imageSmall.loadFromData(sq.value(5).toByteArray());
        si.imagePreview.loadFromData(sq.value(6).toByteArray());

        slides.append(si);
        ++ct;
        progress.setValue(ct);
    }

    ++ct;
    progress.setValue(ct);
}

void SlideShow::saveSideShow(QString savelbl, QWidget *ptW, QList<int> delList)
{
    QSqlQuery sq;
    QSqlDatabase::database().transaction();

    QProgressDialog prg(savelbl,"Cancel",0,slides.count()+1+delList.count(),ptW);

    int ct(0);

    // Create new slide show in not yet in database
    if(slideShowId == -1)
    {
        sq.prepare("INSERT INTO SlideShows (name, info) VALUES (?,?)");
        sq.addBindValue(name);
        sq.addBindValue(info);
        sq.exec();
        sq.clear();

        // get currently added slideShow id
        sq.exec("SELECT seq FROM sqlite_sequence WHERE name = 'SlideShows'");
        sq.first();
        slideShowId = sq.value(0).toInt();
        sq.clear();

        // Insert new slides
        sq.prepare("INSERT INTO Slides (ss_id, p_order, name, path, pix, pix_small, pix_prev) VALUES (?,?,?,?,?,?,?)");
        foreach(const SlideShowItem &si, slides)
        {
            sq.addBindValue(slideShowId);
            sq.addBindValue(ct);
            sq.addBindValue(si.name);
            sq.addBindValue(si.path);
            sq.addBindValue(pixToByte(si.image));
            sq.addBindValue(pixToByte(si.imageSmall));
            sq.addBindValue(pixToByte(si.imagePreview));
            sq.exec();
            ++ct;
            prg.setValue(ct);
        }
        QSqlDatabase::database().commit();
        return;
    }
    else
    {
        // Update Slide Show
        // Update slide show info
        sq.prepare("UPDATE SlideShows SET name = ?, info = ? WHERE id  = ?");
        sq.addBindValue(name);
        sq.addBindValue(info);
        sq.addBindValue(slideShowId);
        sq.exec();
        sq.clear();

        // Update slides (order only can be updated)
        int c(0);
        foreach(const SlideShowItem &si, slides)
        {
            if(si.slideId != -1)
            {
                sq.exec(QString("UPDATE Slides SET p_order = %1 WHERE id = %2").arg(c).arg(si.slideId));
                ++ct;
                prg.setValue(ct);
            }
            ++c;
        }
        sq.clear();

        // Insert new slides
        c = 0;
        sq.prepare("INSERT INTO Slides (ss_id, p_order, name, path, pix, pix_small, pix_prev) VALUES (?,?,?,?,?,?,?)");
        foreach(const SlideShowItem &si, slides)
        {
            if(si.slideId == -1)
            {
                sq.addBindValue(slideShowId);
                sq.addBindValue(c);
                sq.addBindValue(si.name);
                sq.addBindValue(si.path);
                sq.addBindValue(pixToByte(si.image));
                sq.addBindValue(pixToByte(si.imageSmall));
                sq.addBindValue(pixToByte(si.imagePreview));
                sq.exec();
                ++ct;
                prg.setValue(ct);
            }
            ++c;
        }
    }
    sq.clear();

    // Delete slides
    foreach(const int sid, delList)
    {
        if(sid>=0)
            sq.exec(QString("DELETE FROM Slides WHERE id = %1").arg(sid));
        ++ct;
        prg.setValue(ct);
    }

    prg.setLabelText("Saving Slide Show to Database");
    QSqlDatabase::database().commit();
    ++ct;
    prg.setValue(ct);
}
