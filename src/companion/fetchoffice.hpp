/***************************************************************************
//
//    CGF Projector companion - relays releases from the push server
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

#ifndef FETCHOFFICE_HPP
#define FETCHOFFICE_HPP

#include <QString>

// Downloads LibreOffice from The Document Foundation and unpacks it into
// <dataDir>/libreoffice, for the app's PowerPoint import on computers without
// PowerPoint (BUILD.md, "LibreOffice for PowerPoint files"). Nothing is
// installed: Windows Installer only unpacks the files (an "administrative
// install"), which needs no administrator rights and adds no Start menu
// entry. The download must match the SHA-256 built into this program.
//
// Prints "progress <received> <total>" while downloading, then "unpacking"
// and "done". Returns the exit code: 0 ready (also when it was there
// already), 1 another download is running, 3 could not download, 4 the
// download was damaged, 5 could not unpack, 6 not enough disk space. The
// reason goes to stderr.
int fetchLibreOffice(const QString &dataDir);

#endif // FETCHOFFICE_HPP
