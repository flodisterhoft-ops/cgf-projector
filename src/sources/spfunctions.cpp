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

#include "../headers/spfunctions.hpp"
#include <QScreen>
//spFunctions::spFunctions()
//{
//}

QByteArray pixToByte(const QPixmap & pmap)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pmap.save(&buffer, "JPG",90);
    return buffer.data();
}

QPixmap cgfSplashPixmap(const QSize &size, qreal devicePixelRatio)
{
    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    const QPixmap source(dpr > 1.0 ? QStringLiteral(":/icons/icons/splash@2x.png")
                                   : QStringLiteral(":/icons/icons/splash.png"));
    QPixmap pixmap = source.scaled(size * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QIcon cgfAppIcon()
{
    QIcon icon;
    for(int px : {16, 20, 24, 32, 40, 48, 64, 256})
        icon.addFile(QStringLiteral(":/icons/icons/app-%1.png").arg(px), QSize(px, px));
    return icon;
}

static QString screenPosition(const QScreen *screen)
{
    const QRect g = screen->geometry();
    return QString::number(g.x()) + QLatin1Char(',') + QString::number(g.y());
}

QString screenIdentity(const QScreen *screen)
{
    if(!screen)
        return QString();
    const QString raw = screen->name() + QLatin1Char('\n') + screen->serialNumber()
            + QLatin1Char('\n') + screenPosition(screen);
    return QString::fromLatin1(raw.toUtf8().toPercentEncoding());
}

static QStringList identityParts(const QString &identity)
{
    // name, serial number, position
    return QString::fromUtf8(QByteArray::fromPercentEncoding(identity.toLatin1())).split(QLatin1Char('\n'));
}

int screenMatches(const QString &identity, const QScreen *screen)
{
    if(identity.isEmpty() || !screen)
        return 0;
    const QStringList want = identityParts(identity);
    const QString name = want.value(0), serial = want.value(1), position = want.value(2);
    const QString hasSerial = screen->serialNumber();
    // Two devices that both report a serial number are told apart by it.
    if(!serial.isEmpty() && !hasSerial.isEmpty() && serial != hasSerial)
        return 0;
    const bool sameSerial = !serial.isEmpty() && serial == hasSerial;
    const bool sameName = !name.isEmpty() && name == screen->name();
    if(!sameSerial && !sameName)
        return 0;
    // Two projectors of the same model: the position Windows gives each one
    // tells them apart while both are connected.
    return (sameSerial ? 4 : 0) + (sameName ? 2 : 0) + (position == screenPosition(screen) ? 1 : 0);
}

QString screenIdentityName(const QString &identity)
{
    return identityParts(identity).value(0);
}

bool isAnnounceTitle(QString string)
{
    // Check if the line is verse title line
    if(string.startsWith("Announce"))
        return true;
    else if (string.startsWith("Slide"))
        return true;
    else if (string.startsWith(QString::fromUtf8("Объявление")))
        return true;
    else if (string.startsWith(QString::fromUtf8("Слайд")))
        return true;
    else if (string.startsWith(QString::fromUtf8("Оголошення")))
        return true;
    else if (string.startsWith(QString::fromUtf8("Ankündigung")))
        return true;
    else if (string.startsWith(QString::fromUtf8("Oznámení")))
        return true;
    else if (string.startsWith(QString::fromUtf8("Snímek")))
        return true;
    else
        return false;
}

QString getSupportedImageFormats()
{
    QList<QByteArray> im = QImageReader::supportedImageFormats();
    QString imfor;
    foreach (QByteArray f,im)
        imfor += " *." + f;
    return imfor.trimmed();
}
