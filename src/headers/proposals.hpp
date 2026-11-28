/***************************************************************************
//
//    CGF Projector - operators' song changes on their way to the library
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

#ifndef PROPOSALS_HPP
#define PROPOSALS_HPP

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

// A song an operator adds or changes in the song editor becomes a proposal
// for the master library (docs/proposals-up.md). Proposals wait in the
// CgfProposals table until the companion is connected, so projection never
// depends on the internet. Until the master has the change, the operator's
// text stays on this laptop: a release that brings the song gets the
// operator's text written back over it in the same transaction. Florian's
// Undo puts the library's text back.
class Proposals : public QObject
{
    Q_OBJECT

public:
    explicit Proposals(QObject *parent = nullptr);
    ~Proposals() override;
    static Proposals *instance();

    // The text fields of a Songs row as a proposal carries them (with the
    // songbook's name for Florian's message). Empty if the song is missing.
    static QJsonObject songFields(int songId);

    // After the editor saved a song: before is songFields() read just before
    // the save (empty for a new song). Makes or updates a queued proposal.
    static void recordSave(int songId, const QJsonObject &before, bool isNew);

    // Inside a release's transaction, after its songs were written: puts the
    // operator's text back over songs with a change the master lacks, ends
    // overrides the release brings, and folds an accepted new song into its
    // library copy. Adds removed quick-add ids to deleted. Returns an error.
    static QString applyOverrides(QSqlDatabase &db, const QList<int> &songs, QList<int> &deleted);

    // Proposal messages for the companion, oldest first.
    QList<QJsonObject> queued() const;

public slots:
    // The library version proposals start from (LibraryPush::version()).
    void setLibraryVersion(int version);
    // A message from the companion: proposalsReady, proposalStored or
    // proposalState. Returns the answer for it (or an empty object).
    QJsonObject handleMessage(const QJsonObject &message);

signals:
    // A proposal is ready to send (the companion sends it when it can).
    void outgoing(const QJsonObject &message);
    // Songs whose text this changed (an undo), for the lists and schedule.
    void songsChanged(const QList<int> &songs, const QList<int> &deleted);

private:
    static bool ensureTable(QSqlDatabase &db);
};

#endif // PROPOSALS_HPP
