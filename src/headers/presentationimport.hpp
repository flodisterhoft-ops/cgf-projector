/***************************************************************************
//
//    CGF Projector - based on softProjector, an open source media projection software
//    Copyright (C) 2026  CGF Projector contributors
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

#ifndef PRESENTATIONIMPORT_HPP
#define PRESENTATIONIMPORT_HPP

#include <QString>
#include <functional>
#include "slideshow.hpp"
#include "settings.hpp"

class QWidget;

// Turns a PowerPoint or PDF file into Pictures slides, one picture per slide.
// PowerPoint files are exported by PowerPoint on this computer, or without
// PowerPoint by LibreOffice; PDF pages are drawn by Windows, so PDFs need no
// Office. The conversion runs in scripts/import-presentation.ps1 while the
// app stays live.
namespace PresentationImport
{
QString fileFilter();
// The slides in order. Empty when the import failed or was cancelled; then
// error holds the reason (empty after a cancel). On a computer with neither
// PowerPoint nor LibreOffice it offers to download LibreOffice once.
QList<SlideShowItem> run(const QString &file, const SlideShowSettings &sets,
                         QWidget *parent, QString &error);

// LibreOffice's soffice.exe: the copy in the data folder (the folder of the
// database), else one installed on the computer. Empty when there is none.
QString findLibreOffice();
// Downloads LibreOffice and unpacks it into the data folder. The companion
// does it (cgf-companion --fetch-libreoffice), so the app itself stays off
// the internet. False after a failure (error says why) or a cancel (error
// stays empty).
bool fetchLibreOffice(QWidget *parent, QString &error);
// Asks whether to download LibreOffice. The checks replace it.
extern std::function<bool(QWidget *)> confirmDownload;
}

#endif // PRESENTATIONIMPORT_HPP
