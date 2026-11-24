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

#ifndef SPFUNCTIONS_HPP
#define SPFUNCTIONS_HPP

#include <QString>
#include <QStringList>
#include <QImageReader>
#include <QPixmap>
#include <QIcon>
#include <QBuffer>


QByteArray pixToByte(const QPixmap & pmap);
// The splash picture (splash screen and About), sharp at any display scaling.
// size is in device-independent pixels; the picture is 450 x 180.
QPixmap cgfSplashPixmap(const QSize &size, qreal devicePixelRatio);
// The window and taskbar icon, with each size drawn on its own
// (tools/branding/make_branding.py).
QIcon cgfAppIcon();
bool isAnnounceTitle(QString string);
QString getSupportedImageFormats();
//class spFunctions
//{
//public:
//    spFunctions();
//};

#endif // SPFUNCTIONS_HPP
