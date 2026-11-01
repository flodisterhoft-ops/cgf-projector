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
#include "slideshow.hpp"
#include "settings.hpp"

class QWidget;

// Turns a PowerPoint or PDF file into Pictures slides, one picture per slide.
// PowerPoint files are exported by PowerPoint on this computer; PDF pages are
// drawn by Windows, so PDFs need no Office. The conversion runs in
// scripts/import-presentation.ps1 while the app stays live.
namespace PresentationImport
{
QString fileFilter();
// The slides in order. Empty when the import failed or was cancelled; then
// error holds the reason (empty after a cancel).
QList<SlideShowItem> run(const QString &file, const SlideShowSettings &sets,
                         QWidget *parent, QString &error);
}

#endif // PRESENTATIONIMPORT_HPP
