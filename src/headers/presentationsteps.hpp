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

#ifndef PRESENTATIONSTEPS_HPP
#define PRESENTATIONSTEPS_HPP

#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

// A video, a sound or an animated picture that belongs to a step of a slide.
struct SlideMedia
{
    QString kind;       // "video", "audio" or "gif"
    QString file;       // file name in the data folder's slide-media folder
    QRectF rect;        // where on the slide, 0..1 of its width and height
    double volume = 1.0;
    bool loop = false;
};

// One still of an imported presentation and how it comes onto the screen.
// A slide without animations is one step. A slide with animations is one
// step for the slide as it first appears and one for every time something
// appears or leaves: the operator's Next moves through them the way a click
// does in PowerPoint. PowerPoint's effects are reduced to a few simple ones
// that need only the two stills (BUILD.md, "Presentation steps").
struct SlideStep
{
    int slide = 0;          // the slide's number among the slides shown, from 1; 0 = not a presentation step
    int index = 0;          // 0 = the slide as it first appears
    bool automatic = false; // follows the step before it by itself (PowerPoint's "after previous")
    int delayMs = 0;        // an automatic step starts this long after the step before it started
    // How this still replaces the one before it. For index 0 it is the
    // slide's transition, otherwise what the appearing or leaving object does.
    //   cut, fade, wipe, split, box, fly, float, zoom; push, cover, uncover (transitions only)
    // Empty: the slide has no transition, and the app's usual fade is used.
    QString effect;
    // wipe, fly, push, cover, uncover: the side it starts from (left, right, top, bottom);
    // float: up or down; split: horizontal or vertical
    QString direction;
    bool exit = false;      // the object leaves instead of appearing
    int durationMs = 500;
    QList<SlideMedia> media;    // videos and sounds that start with this step; animated pictures it shows
    QString notes;              // the slide's speaker notes (index 0 only)

    bool isValid() const { return slide > 0; }
    // One line of text for the Slides table's path column and schedule files.
    QString toText() const;
    static SlideStep fromText(const QString &text);
};

namespace PresentationSteps
{
// Rewrites a .pptx (also .pptm, .ppsx) so that every step of every slide is
// a slide of its own, without animations and transitions, for PowerPoint or
// LibreOffice to draw. steps gets one entry per slide of the new file that
// is shown, in order. Videos, sounds and animated pictures are copied into
// mediaDir under names made from their content.
//
// False when the file cannot be read as a presentation (error says why);
// the import then draws the file as it is, one still per slide.
bool expand(const QString &pptx, const QString &expanded, const QString &mediaDir,
            QList<SlideStep> &steps, QString &error);

// The folder for videos, sounds and animated pictures of imported
// presentations, beside the database.
QString mediaDir();
// The media files a saved slide show uses, and deleting those of them that
// no saved slide show uses any more (after one is deleted). A schedule file
// that still names a deleted file shows the slide's still without it.
QStringList mediaOf(int slideShowId);
void removeUnusedMedia(const QStringList &files);
}

#endif // PRESENTATIONSTEPS_HPP
