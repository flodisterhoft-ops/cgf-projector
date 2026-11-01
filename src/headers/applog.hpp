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

#ifndef APPLOG_HPP
#define APPLOG_HPP

#include <QString>

class QScreen;

// CGFProjector.log next to the database: Qt warnings, monitor and sound
// device changes, and what the app did about them, so a dropped projector
// link can be traced after a service. A log over 1 MB at start-up moves to
// CGFProjector-previous.log.
namespace AppLog
{
void start(const QString &dir);
void write(const QString &text);
QString describe(const QScreen *screen);
}

#endif // APPLOG_HPP
