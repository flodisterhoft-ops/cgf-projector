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

#include "../headers/cgfsignature.hpp"

#include <QCryptographicHash>
#include <QJsonValue>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#endif

bool cgfVerifySignature(const QByteArray &publicKey, const QByteArray &message, const QByteArray &signature)
{
    if(publicKey.size() != 64 || signature.size() != 64)
        return false;
#ifdef Q_OS_WIN
    BCRYPT_ALG_HANDLE alg = nullptr;
    if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    QByteArray blob(sizeof(BCRYPT_ECCKEY_BLOB), '\0');
    auto *header = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    blob.append(publicKey);
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if(BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                          reinterpret_cast<PUCHAR>(blob.data()), ULONG(blob.size()), 0)))
    {
        QByteArray hash = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
        QByteArray sig = signature;
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(hash.data()),
                                                  ULONG(hash.size()), reinterpret_cast<PUCHAR>(sig.data()),
                                                  ULONG(sig.size()), 0));
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    Q_UNUSED(message);
    qWarning("Publisher signatures need Windows to be checked; refused");
    return false;
#endif
}

bool cgfSignedByAny(const QList<QByteArray> &publicKeys, const QByteArray &message, const QByteArray &signatureBase64)
{
    const QByteArray sig = QByteArray::fromBase64(signatureBase64);
    for(const QByteArray &key : publicKeys)
        if(cgfVerifySignature(key, message, sig))
            return true;
    return false;
}

QList<QByteArray> cgfPublisherKeys(const QJsonArray &keys)
{
    QList<QByteArray> out;
    for(const QJsonValue &k : keys)
    {
        const QByteArray key = QByteArray::fromBase64(k.toString().toLatin1());
        if(key.size() == 64)
            out << key;
        else
            qWarning("cgf-cloud.json: a publisher key is not a P-256 public key");
    }
    return out;
}
