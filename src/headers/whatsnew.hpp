/***************************************************************************
//
//    CGF Projector - the What's new window
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

#ifndef WHATSNEW_HPP
#define WHATSNEW_HPP

#include <QDialog>
#include <QJsonObject>
#include <QString>

// One window at the first start after the program or the library changed
// (docs/whats-new.md). Program changes come from whats-new.json in this
// build's resources. Library changes are recorded as releases apply, in
// cgf-whats-new.json beside the database, and shown at the next start, so the
// window never opens during a service.
namespace WhatsNew
{
// The program version this build describes: the newest entry in whats-new.json.
QString appVersion();

// Adds one applied release's changes: {"songs": [...], "songbooks": [...],
// "bibles": [...]}, each entry {"id", "s": "added" | "updated" | "removed"}
// plus "n" and "t" (song number and title) or "name".
void recordLibrary(const QString &dataDir, const QJsonObject &changes);

// What the window shows now; empty when nothing changed since it was last
// shown. previousApp (e.g. "0.2.6") counts as last shown when nothing was
// recorded yet: the first start after an update from a version without this
// window. With neither, this version counts as seen and nothing is shown.
QJsonObject pending(const QString &dataDir, const QString &previousApp, int libraryVersion);

// The content was shown: it becomes what Help > What's new opens, and the
// recorded library changes start over.
void markShown(const QString &dataDir, const QJsonObject &content);

// What Help > What's new shows: the last content shown, or this version's
// program changes.
QJsonObject lastShown(const QString &dataDir, int libraryVersion);
}

class WhatsNewDialog : public QDialog
{
public:
    explicit WhatsNewDialog(const QJsonObject &content, QWidget *parent = nullptr);
};

#endif // WHATSNEW_HPP
