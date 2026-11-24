/***************************************************************************
//
//    CGF Projector - publisher signatures (docs/song-push.md)
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

#ifndef CGFSIGNATURE_HPP
#define CGFSIGNATURE_HPP

#include <QByteArray>
#include <QJsonArray>
#include <QList>

// ECDSA P-256 / SHA-256, checked with Windows' own crypto (bcrypt). The key is
// X||Y (64 bytes), the signature r||s (64 bytes). Shared by the app (library
// releases) and the companion (app releases).
bool cgfVerifySignature(const QByteArray &publicKey, const QByteArray &message, const QByteArray &signature);

// True when any of the keys (base64, from cgf-cloud.json) signed the message.
bool cgfSignedByAny(const QList<QByteArray> &publicKeys, const QByteArray &message, const QByteArray &signatureBase64);

// The publisherKeys array of cgf-cloud.json, decoded; malformed keys are skipped.
QList<QByteArray> cgfPublisherKeys(const QJsonArray &keys);

#endif // CGFSIGNATURE_HPP
