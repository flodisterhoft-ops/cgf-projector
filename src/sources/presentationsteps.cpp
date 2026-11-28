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

// A .pptx is a zip of XML files. Each slide's XML lists its shapes and, in
// <p:timing>, its animations: which shape or paragraph, which effect, on
// which click. This file turns that into one slide per step, with the shapes
// that have not appeared yet taken out and the text lines that have not
// appeared yet made invisible (so the lines above keep their place).
//
// The XML is read without namespace processing and elements are matched by
// the part of their name after the colon: the file is then written back
// exactly as it was, apart from what is changed here.

#include "../headers/presentationsteps.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

namespace
{
const char stepPrefix[] = "cgf-step:";

// ---------------------------------------------------------------- XML helpers

QString local(const QDomNode &node)
{
    return node.nodeName().section(QLatin1Char(':'), -1);
}

QString prefixOf(const QDomElement &e)
{
    const QString name = e.nodeName();
    const int colon = name.indexOf(QLatin1Char(':'));
    return colon < 0 ? QString() : name.left(colon + 1);
}

// An attribute by the part of its name after the colon (r:id, r:embed, ...).
QString attr(const QDomElement &e, const QString &name)
{
    if(e.hasAttribute(name))
        return e.attribute(name);
    const QDomNamedNodeMap all = e.attributes();
    for(int i = 0; i < all.count(); ++i)
    {
        const QString n = all.item(i).nodeName();
        if(n.section(QLatin1Char(':'), -1) == name && !n.startsWith(QLatin1String("xmlns")))
            return all.item(i).nodeValue();
    }
    return QString();
}

QDomElement child(const QDomElement &e, const QString &name)
{
    for(QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if(local(c) == name)
            return c;
    return QDomElement();
}

QList<QDomElement> children(const QDomElement &e, const QString &name)
{
    QList<QDomElement> out;
    for(QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if(local(c) == name)
            out << c;
    return out;
}

// All elements below e with this name, in document order. Newer features sit
// in <mc:AlternateContent> with an older <mc:Fallback> beside them; with
// skipFallback only the newer branch is read, so nothing counts twice.
void descendants(const QDomElement &e, const QString &name, QList<QDomElement> &out, bool skipFallback = false)
{
    for(QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
    {
        const QString n = local(c);
        if(skipFallback && n == QLatin1String("Fallback"))
            continue;
        if(n == name)
            out << c;
        descendants(c, name, out, skipFallback);
    }
}

QList<QDomElement> descendants(const QDomElement &e, const QString &name, bool skipFallback = false)
{
    QList<QDomElement> out;
    descendants(e, name, out, skipFallback);
    return out;
}

bool parse(const QByteArray &xml, QDomDocument &doc)
{
    // Runs that hold a single space are common; the default would drop them.
    return bool(doc.setContent(xml, QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
}

// ---------------------------------------------------------------- the package

struct Relationship
{
    QString id, type, target;   // target: the part's path in the zip, or the address when external
    bool external = false;
};

struct Package
{
    QStringList names;                  // in the order of the zip
    QHash<QString, QByteArray> parts;

    bool has(const QString &name) const { return parts.contains(name); }

    static QString relsName(const QString &part)
    {
        const int slash = part.lastIndexOf(QLatin1Char('/'));
        return part.left(slash + 1) + QLatin1String("_rels/") + part.mid(slash + 1) + QLatin1String(".rels");
    }

    QList<Relationship> relationships(const QString &part) const
    {
        QList<Relationship> out;
        QDomDocument doc;
        if(!parse(parts.value(relsName(part)), doc))
            return out;
        const QString folder = part.left(part.lastIndexOf(QLatin1Char('/')) + 1);
        for(const QDomElement &r : children(doc.documentElement(), QStringLiteral("Relationship")))
        {
            Relationship rel;
            rel.id = r.attribute(QStringLiteral("Id"));
            rel.type = r.attribute(QStringLiteral("Type")).section(QLatin1Char('/'), -1);
            rel.external = r.attribute(QStringLiteral("TargetMode")) == QLatin1String("External");
            const QString target = r.attribute(QStringLiteral("Target"));
            if(rel.external)
                rel.target = target;
            else if(target.startsWith(QLatin1Char('/')))
                rel.target = target.mid(1);
            else
                rel.target = QDir::cleanPath(folder + target);
            out << rel;
        }
        return out;
    }

    void add(const QString &name, const QByteArray &data)
    {
        if(!parts.contains(name))
            names << name;
        parts.insert(name, data);
    }
};

bool readPackage(const QString &file, Package &pkg)
{
    QZipReader zip(file);
    if(!zip.isReadable())
        return false;
    const auto entries = zip.fileInfoList();
    for(const QZipReader::FileInfo &info : entries)
    {
        if(!info.isFile)
            continue;
        pkg.names << info.filePath;
        pkg.parts.insert(info.filePath, zip.fileData(info.filePath));
    }
    return zip.status() == QZipReader::NoError && !pkg.names.isEmpty();
}

bool writePackage(const QString &file, const Package &pkg)
{
    QFile::remove(file);
    QZipWriter zip(file);
    if(!zip.isWritable())
        return false;
    // Pictures, videos and sounds are compressed already.
    static const QSet<QString> packed = {"png", "jpg", "jpeg", "gif", "mp4", "m4v", "mov", "wmv", "avi", "mpg",
                                         "mpeg", "mp3", "m4a", "wma", "wdp", "jxr", "zip", "bin"};
    for(const QString &name : pkg.names)
    {
        zip.setCompressionPolicy(packed.contains(QFileInfo(name).suffix().toLower())
                                 ? QZipWriter::NeverCompress : QZipWriter::AlwaysCompress);
        zip.addFile(name, pkg.parts.value(name));
    }
    zip.close();
    return zip.status() == QZipWriter::NoError;
}

// The relationships of a slide that is a copy of another. Pictures, media
// and the layout are shared with the original. Notes, comments and custom
// data ("tags") belong to one slide only and are left out; PowerPoint
// refuses a file in which two slides share a tags part. Anything else (a
// chart, SmartArt, an embedded object) is copied, with what it links to, so
// every slide has its own.
QByteArray copyRelationships(Package &pkg, QDomDocument &types, const QString &source, const QString &copyTag, int depth = 0)
{
    static const QSet<QString> shared = {QStringLiteral("slideLayout"), QStringLiteral("slideMaster"), QStringLiteral("theme"),
                                         QStringLiteral("image"), QStringLiteral("hdphoto"), QStringLiteral("media"),
                                         QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("hyperlink"),
                                         QStringLiteral("font"), QStringLiteral("slide")};
    static const QSet<QString> dropped = {QStringLiteral("notesSlide"), QStringLiteral("tags"), QStringLiteral("comments"),
                                          QStringLiteral("commentAuthors"), QStringLiteral("customXml")};
    QDomDocument doc;
    if(!parse(pkg.parts.value(Package::relsName(source)), doc))
        return QByteArray();
    const QString folder = source.left(source.lastIndexOf(QLatin1Char('/')) + 1);
    QDomElement all = doc.documentElement();
    for(QDomElement r : children(all, QStringLiteral("Relationship")))
    {
        const QString type = r.attribute(QStringLiteral("Type")).section(QLatin1Char('/'), -1);
        if(dropped.contains(type))
        {
            all.removeChild(r);
            continue;
        }
        if(shared.contains(type) || r.attribute(QStringLiteral("TargetMode")) == QLatin1String("External") || depth > 4)
            continue;
        const QString written = r.attribute(QStringLiteral("Target"));
        const QString target = written.startsWith(QLatin1Char('/')) ? written.mid(1) : QDir::cleanPath(folder + written);
        if(!pkg.has(target))
            continue;
        // chart3.xml becomes chart3_cgf7.xml beside it.
        const int dot = target.lastIndexOf(QLatin1Char('.'));
        const QString copy = target.left(dot) + QLatin1String("_cgf") + copyTag + target.mid(dot);
        if(!pkg.has(copy))
        {
            pkg.add(copy, pkg.parts.value(target));
            for(const QDomElement &o : children(types.documentElement(), QStringLiteral("Override")))
            {
                if(o.attribute(QStringLiteral("PartName")) != QLatin1Char('/') + target)
                    continue;
                QDomElement mine = types.createElement(QStringLiteral("Override"));
                mine.setAttribute(QStringLiteral("PartName"), QLatin1Char('/') + copy);
                mine.setAttribute(QStringLiteral("ContentType"), o.attribute(QStringLiteral("ContentType")));
                types.documentElement().appendChild(mine);
                break;
            }
            const QByteArray below = copyRelationships(pkg, types, target, copyTag, depth + 1);
            if(!below.isEmpty())
                pkg.add(Package::relsName(copy), below);
        }
        const int slash = written.lastIndexOf(QLatin1Char('/'));
        r.setAttribute(QStringLiteral("Target"), written.left(slash + 1) + copy.mid(copy.lastIndexOf(QLatin1Char('/')) + 1));
    }
    return doc.toByteArray(-1);
}

// ---------------------------------------------------------------- animations

// What an animation acts on: a whole shape (paragraph -1) or one line of its text.
struct Target
{
    int shape = 0;
    int paragraph = -1;
    bool operator==(const Target &o) const { return shape == o.shape && paragraph == o.paragraph; }
};
size_t qHash(const Target &t, size_t seed = 0) { return ::qHash(quint64(t.shape) << 20 | quint32(t.paragraph + 1), seed); }

struct Effect
{
    QString start;          // clickEffect, withEffect, afterEffect
    QString cls;            // entr, exit, emph, path, mediacall, verb
    int preset = 0, subtype = 0;
    int offsetMs = 0;       // after the click that starts its group
    int durationMs = 500;
    QList<Target> targets;
    QStringList filters;    // animEffect filters: "wipe(left)", "fade", ...
    QSet<QString> animated; // attributes moved by <p:anim>: ppt_x, ppt_y, ppt_w, ppt_h, ...
    bool scales = false, rotates = false;
    QList<int> plays;       // shapes whose video or sound this starts
    QStringList sounds;     // relationship ids of sounds played with the effect
    bool interactive = false;   // from a sequence that starts when a shape is clicked
};

int milliseconds(const QString &text, int fallback = 0)
{
    bool ok = false;
    const int v = text.toInt(&ok);
    return ok ? v : fallback;
}

// The delay in <p:cTn><p:stCondLst><p:cond delay="..."/>: 0 when it waits for a click ("indefinite").
int startDelay(const QDomElement &cTn)
{
    const QDomElement conditions = child(cTn, QStringLiteral("stCondLst"));
    for(const QDomElement &c : children(conditions, QStringLiteral("cond")))
    {
        if(c.hasAttribute(QStringLiteral("evt")))
            continue;
        return milliseconds(c.attribute(QStringLiteral("delay")));
    }
    return 0;
}

QList<Effect> readSequence(const QDomElement &sequence, bool interactive)
{
    QList<Effect> out;
    for(const QDomElement &node : descendants(sequence, QStringLiteral("cTn"), true))
    {
        if(!node.hasAttribute(QStringLiteral("presetClass")))
            continue;
        Effect e;
        e.interactive = interactive;
        e.start = node.attribute(QStringLiteral("nodeType"));
        e.cls = node.attribute(QStringLiteral("presetClass"));
        e.preset = node.attribute(QStringLiteral("presetID")).toInt();
        e.subtype = node.attribute(QStringLiteral("presetSubtype")).toInt();
        // <p:par><p:cTn (time group)><p:childTnLst><p:par><p:cTn (this effect)>
        const QDomElement group = node.parentNode().parentNode().parentNode().toElement();
        e.offsetMs = startDelay(node) + (local(group) == QLatin1String("cTn") ? startDelay(group) : 0);

        int longest = 0;
        const QDomElement behaviours = child(node, QStringLiteral("childTnLst"));
        for(const QDomElement &b : descendants(behaviours, QStringLiteral("cTn"), true))
            longest = qMax(longest, startDelay(b) + milliseconds(b.attribute(QStringLiteral("dur"))));
        e.durationMs = qBound(150, longest > 1 ? longest : 500, 1500);

        for(const QDomElement &t : descendants(behaviours, QStringLiteral("spTgt"), true))
        {
            Target target;
            target.shape = t.attribute(QStringLiteral("spid")).toInt();
            const QDomElement range = child(child(t, QStringLiteral("txEl")), QStringLiteral("pRg"));
            if(!range.isNull())
            {
                // One target per line, so every line keeps its own state.
                const int from = range.attribute(QStringLiteral("st")).toInt();
                const int to = qMax(from, range.attribute(QStringLiteral("end")).toInt());
                for(int p = from; p <= qMin(to, from + 200); ++p)
                {
                    target.paragraph = p;
                    if(!e.targets.contains(target))
                        e.targets << target;
                }
                continue;
            }
            if(target.shape > 0 && !e.targets.contains(target))
                e.targets << target;
        }
        for(const QDomElement &f : descendants(behaviours, QStringLiteral("animEffect"), true))
            e.filters << f.attribute(QStringLiteral("filter"));
        for(const QDomElement &a : descendants(behaviours, QStringLiteral("anim"), true))
            for(const QDomElement &n : descendants(a, QStringLiteral("attrName")))
                e.animated << n.text();
        e.scales = !descendants(behaviours, QStringLiteral("animScale"), true).isEmpty();
        e.rotates = !descendants(behaviours, QStringLiteral("animRot"), true).isEmpty();
        for(const QDomElement &c : descendants(behaviours, QStringLiteral("cmd"), true))
        {
            if(!c.attribute(QStringLiteral("cmd")).startsWith(QLatin1String("playFrom")))
                continue;
            for(const QDomElement &t : descendants(c, QStringLiteral("spTgt")))
                e.plays << t.attribute(QStringLiteral("spid")).toInt();
        }
        for(const QDomElement &s : descendants(node, QStringLiteral("sndTgt"), true))
            e.sounds << attr(s, QStringLiteral("embed"));
        out << e;
    }
    return out;
}

QString sideOf(int subtype)
{
    // PowerPoint's direction bits: 1 top, 2 right, 4 bottom, 8 left (corners combine two).
    if(subtype & 8) return QStringLiteral("left");
    if(subtype & 2) return QStringLiteral("right");
    if(subtype & 1) return QStringLiteral("top");
    if(subtype & 4) return QStringLiteral("bottom");
    return QStringLiteral("left");
}

// PowerPoint's effect as one of ours. Chosen from what the effect does (its
// behaviours), with the preset number for the ones that need telling apart.
void classify(const Effect &e, SlideStep &step)
{
    step.exit = e.cls == QLatin1String("exit");
    step.durationMs = e.durationMs;
    const bool moves = e.animated.contains(QStringLiteral("ppt_x")) || e.animated.contains(QStringLiteral("ppt_y"));
    const bool sizes = e.animated.contains(QStringLiteral("ppt_w")) || e.animated.contains(QStringLiteral("ppt_h")) || e.scales;
    QString filter;
    for(const QString &f : e.filters)
        if(!f.startsWith(QLatin1String("fade")) && !f.startsWith(QLatin1String("image")))
            filter = f;
    const bool fades = e.filters.join(QLatin1Char(' ')).contains(QLatin1String("fade"));

    if(filter.startsWith(QLatin1String("wipe(")))
    {
        const QString arg = filter.mid(5).chopped(1);
        step.effect = QStringLiteral("wipe");
        step.direction = arg == QLatin1String("up") ? QStringLiteral("top")
                       : arg == QLatin1String("down") ? QStringLiteral("bottom") : arg;
    }
    else if(filter.startsWith(QLatin1String("barn(")))
    {
        step.effect = QStringLiteral("split");
        step.direction = filter.contains(QLatin1String("Horizontal")) ? QStringLiteral("horizontal") : QStringLiteral("vertical");
    }
    else if(filter.startsWith(QLatin1String("box(")) || filter.startsWith(QLatin1String("circle("))
            || filter.startsWith(QLatin1String("diamond(")) || filter.startsWith(QLatin1String("plus(")))
        step.effect = QStringLiteral("box");
    else if(e.preset == 27)             // Color Typewriter: letter after letter
    {
        step.effect = QStringLiteral("wipe");
        step.direction = QStringLiteral("left");
    }
    else if(!filter.isEmpty())          // blinds, checkerboard, dissolve, strips, wheel, ...
        step.effect = QStringLiteral("fade");
    else if(sizes)                      // Zoom, Grow & Turn, Compress, Stretch, Swivel, Spinner, ...
        step.effect = QStringLiteral("zoom");
    else if(moves && fades)             // Float In (ascend, descend), Rise Up, ...
    {
        step.effect = QStringLiteral("float");
        step.direction = e.preset == 47 ? QStringLiteral("down") : QStringLiteral("up");
    }
    else if(moves)                      // Fly In, Crawl In, Bounce, ...
    {
        step.effect = QStringLiteral("fly");
        step.direction = sideOf(e.subtype ? e.subtype : 4);
    }
    else if(fades)
        step.effect = QStringLiteral("fade");
    else if(e.rotates)
        step.effect = QStringLiteral("fade");
    else                                // Appear, Disappear
        step.effect = QStringLiteral("cut");
}

QString opposite(const QString &dir)
{
    // A transition's dir is where the slides move to; ours is the side they start from.
    if(dir.startsWith(QLatin1Char('l'))) return QStringLiteral("right");
    if(dir.startsWith(QLatin1Char('r'))) return QStringLiteral("left");
    if(dir.startsWith(QLatin1Char('u'))) return QStringLiteral("bottom");
    if(dir.startsWith(QLatin1Char('d'))) return QStringLiteral("top");
    return QStringLiteral("right");
}

// The slide's transition as the effect of its first step. A slide without
// one keeps the app's usual fade between pictures (no effect named), which
// is what operators know; PowerPoint would cut.
void readTransition(const QDomElement &slide, SlideStep &step)
{
    step.effect.clear();
    step.durationMs = 500;
    QList<QDomElement> found = descendants(slide, QStringLiteral("transition"));
    // A newer transition comes first, an older one PowerPoint 2007 can show
    // second. Take the first one we can do; any other effect is a fade.
    bool any = false;
    int length = -1;
    for(const QDomElement &t : found)
    {
        const QDomElement kind = t.firstChildElement();
        if(kind.isNull())
            continue;
        any = true;
        const QString name = local(kind);
        int duration = milliseconds(attr(t, QStringLiteral("dur")), -1);
        if(duration < 0)
        {
            const QString speed = t.attribute(QStringLiteral("spd"), QStringLiteral("fast"));
            duration = speed == QLatin1String("slow") ? 1000 : speed == QLatin1String("med") ? 750 : 500;
        }
        const QString dir = kind.attribute(QStringLiteral("dir"), QStringLiteral("l"));
        QString effect, direction;
        if(name == QLatin1String("fade")) effect = QStringLiteral("fade");
        else if(name == QLatin1String("cut")) effect = QStringLiteral("cut");
        else if(name == QLatin1String("push") || name == QLatin1String("pan")) { effect = QStringLiteral("push"); direction = opposite(dir); }
        else if(name == QLatin1String("wipe")) { effect = QStringLiteral("wipe"); direction = opposite(dir); }
        else if(name == QLatin1String("cover")) { effect = QStringLiteral("cover"); direction = opposite(dir); }
        else if(name == QLatin1String("pull")) { effect = QStringLiteral("uncover"); direction = opposite(dir); }
        else if(name == QLatin1String("split"))
        {
            effect = QStringLiteral("split");
            direction = kind.attribute(QStringLiteral("orient"), QStringLiteral("horz")) == QLatin1String("vert")
                    ? QStringLiteral("vertical") : QStringLiteral("horizontal");
        }
        if(!effect.isEmpty())
        {
            step.effect = effect;
            step.direction = direction;
            step.durationMs = effect == QLatin1String("cut") ? 0 : qBound(150, duration, 2000);
            return;
        }
        if(length < 0)
            length = qBound(150, duration, 2000);
    }
    // Only effects we do not have: a fade as long as the first of them.
    if(any)
    {
        step.effect = QStringLiteral("fade");
        step.durationMs = length;
    }
}

// ---------------------------------------------------------------- shapes

// The shape element (p:sp, p:pic, p:grpSp, p:graphicFrame, p:cxnSp) for every shape id.
void indexShapes(const QDomElement &e, QHash<int, QDomElement> &out)
{
    for(QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
    {
        if(local(c) == QLatin1String("cNvPr"))
        {
            const QDomElement shape = c.parentNode().parentNode().toElement();
            const int id = c.attribute(QStringLiteral("id")).toInt();
            if(id > 0 && !shape.isNull() && !out.contains(id))
                out.insert(id, shape);
        }
        indexShapes(c, out);
    }
}

// Removes a shape, in the newer and the older branch when it has both, and
// the <mc:AlternateContent> around it when nothing is left inside.
void removeShape(QDomElement root, int id)
{
    QHash<int, QDomElement> all;
    for(;;)
    {
        all.clear();
        indexShapes(root, all);
        QDomElement shape = all.value(id);
        if(shape.isNull())
            return;
        QDomNode parent = shape.parentNode();
        parent.removeChild(shape);
        while(parent.isElement() && (local(parent) == QLatin1String("Choice") || local(parent) == QLatin1String("Fallback")
                                     || local(parent) == QLatin1String("AlternateContent"))
              && parent.firstChildElement().isNull())
        {
            QDomNode up = parent.parentNode();
            up.removeChild(parent);
            parent = up;
        }
    }
}

QDomElement make(QDomDocument &doc, const QString &prefix, const char *name)
{
    return doc.createElement(prefix + QLatin1String(name));
}

QDomElement invisibleFill(QDomDocument &doc, const QString &prefix, const char *holder)
{
    QDomElement fill = make(doc, prefix, holder);
    QDomElement colour = make(doc, prefix, "srgbClr");
    colour.setAttribute(QStringLiteral("val"), QStringLiteral("FFFFFF"));
    QDomElement alpha = make(doc, prefix, "alpha");
    alpha.setAttribute(QStringLiteral("val"), QStringLiteral("0"));
    colour.appendChild(alpha);
    fill.appendChild(colour);
    return fill;
}

void removeChildren(QDomElement e, const QSet<QString> &names)
{
    for(QDomElement c = e.firstChildElement(); !c.isNull();)
    {
        QDomElement next = c.nextSiblingElement();
        if(names.contains(local(c)))
            e.removeChild(c);
        c = next;
    }
}

// Makes a line of text invisible without moving anything: transparent
// letters, no outline, shadow, highlight or link colour, and a space for a
// bullet. Taking the line out instead would let the lines above it shift.
// The bullet stays a bullet (LibreOffice ignores a transparent bullet
// colour, and no bullet at all would move the text of an indented line); a
// line without one gets none, which would push its text to the right.
void hideParagraph(QDomDocument &doc, QDomElement paragraph, bool bullet)
{
    const QString a = prefixOf(paragraph);
    QDomElement properties = child(paragraph, QStringLiteral("pPr"));
    if(properties.isNull())
    {
        properties = make(doc, a, "pPr");
        paragraph.insertBefore(properties, paragraph.firstChild());
    }
    if(bullet)
    {
        removeChildren(properties, {QStringLiteral("buNone"), QStringLiteral("buAutoNum"),
                                    QStringLiteral("buChar"), QStringLiteral("buBlip")});
        // The schema's order: spacing, bullet colour, size and font, then the bullet itself.
        static const QSet<QString> before = {QStringLiteral("lnSpc"), QStringLiteral("spcBef"), QStringLiteral("spcAft"),
                                             QStringLiteral("buClrTx"), QStringLiteral("buClr"), QStringLiteral("buSzTx"),
                                             QStringLiteral("buSzPct"), QStringLiteral("buSzPts"), QStringLiteral("buFontTx"),
                                             QStringLiteral("buFont")};
        QDomElement after;
        for(QDomElement c = properties.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            if(before.contains(local(c)))
                after = c;
        QDomElement space = make(doc, a, "buChar");
        space.setAttribute(QStringLiteral("char"), QStringLiteral(" "));
        if(after.isNull())
            properties.insertBefore(space, properties.firstChild());
        else
            properties.insertAfter(space, after);
    }

    static const QSet<QString> looks = {QStringLiteral("ln"), QStringLiteral("solidFill"), QStringLiteral("noFill"),
                                        QStringLiteral("gradFill"), QStringLiteral("blipFill"), QStringLiteral("pattFill"),
                                        QStringLiteral("grpFill"), QStringLiteral("effectLst"), QStringLiteral("effectDag"),
                                        QStringLiteral("highlight"), QStringLiteral("hlinkClick"), QStringLiteral("hlinkMouseOver")};
    for(QDomElement run = paragraph.firstChildElement(); !run.isNull(); run = run.nextSiblingElement())
    {
        const QString n = local(run);
        if(n != QLatin1String("r") && n != QLatin1String("fld") && n != QLatin1String("br") && n != QLatin1String("endParaRPr"))
            continue;
        QDomElement look = n == QLatin1String("endParaRPr") ? run : child(run, QStringLiteral("rPr"));
        if(look.isNull())
        {
            look = make(doc, a, "rPr");
            run.insertBefore(look, run.firstChild());
        }
        removeChildren(look, looks);
        // In the schema's order: outline, fill, effects, then whatever was there.
        QDomElement outline = make(doc, a, "ln");
        outline.appendChild(make(doc, a, "noFill"));
        const QDomNode first = look.firstChild();
        for(const QDomElement &e : {outline, invisibleFill(doc, a, "solidFill"), make(doc, a, "effectLst")})
        {
            if(first.isNull())
                look.appendChild(e);
            else
                look.insertBefore(e, first);
        }
    }
}

// 1 when these paragraph properties turn a bullet on, 0 when off, -1 when they do not say.
int bulletIn(const QDomElement &properties)
{
    for(QDomElement c = properties.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
    {
        const QString n = local(c);
        if(n == QLatin1String("buNone"))
            return 0;
        if(n == QLatin1String("buChar") || n == QLatin1String("buAutoNum") || n == QLatin1String("buBlip"))
            return 1;
    }
    return -1;
}

int bulletInList(const QDomElement &listStyle, int level)
{
    return bulletIn(child(listStyle, QStringLiteral("lvl%1pPr").arg(level + 1)));
}

QDomElement placeholderOf(const QDomElement &shape)
{
    return descendants(child(shape, QStringLiteral("nvSpPr")), QStringLiteral("ph")).value(0);
}

// The slide's layout and master: a line that says nothing about its bullet
// has the one they give its placeholder.
struct Inherited
{
    QDomDocument layout, master;
};

Inherited inheritedBy(const Package &pkg, const QString &slidePart)
{
    Inherited in;
    for(const Relationship &r : pkg.relationships(slidePart))
    {
        if(r.type != QLatin1String("slideLayout") || !parse(pkg.parts.value(r.target), in.layout))
            continue;
        for(const Relationship &m : pkg.relationships(r.target))
            if(m.type == QLatin1String("slideMaster"))
                parse(pkg.parts.value(m.target), in.master);
    }
    return in;
}

bool hasBullet(const QDomElement &paragraph, const QDomElement &shape, const Inherited &in)
{
    const QDomElement properties = child(paragraph, QStringLiteral("pPr"));
    const int level = properties.attribute(QStringLiteral("lvl")).toInt();
    int b = bulletIn(properties);
    if(b >= 0)
        return b;
    b = bulletInList(child(child(shape, QStringLiteral("txBody")), QStringLiteral("lstStyle")), level);
    if(b >= 0)
        return b;
    const QDomElement styles = child(in.master.documentElement(), QStringLiteral("txStyles"));
    const QDomElement holder = placeholderOf(shape);
    if(holder.isNull())     // a text box
        return bulletInList(child(styles, QStringLiteral("otherStyle")), level) == 1;
    const QString type = holder.attribute(QStringLiteral("type"), QStringLiteral("obj"));
    const QString index = holder.attribute(QStringLiteral("idx"));
    const bool title = type == QLatin1String("title") || type == QLatin1String("ctrTitle");
    for(const QDomElement &sp : descendants(in.layout.documentElement(), QStringLiteral("sp")))
    {
        const QDomElement theirs = placeholderOf(sp);
        if(theirs.isNull())
            continue;
        const bool same = index.isEmpty() ? theirs.attribute(QStringLiteral("type"), QStringLiteral("obj")) == type
                                          : theirs.attribute(QStringLiteral("idx")) == index;
        if(!same)
            continue;
        b = bulletInList(child(child(sp, QStringLiteral("txBody")), QStringLiteral("lstStyle")), level);
        if(b >= 0)
            return b;
        break;
    }
    for(const QDomElement &sp : descendants(in.master.documentElement(), QStringLiteral("sp")))
    {
        const QString theirs = placeholderOf(sp).attribute(QStringLiteral("type"));
        if(theirs != (title ? QLatin1String("title") : QLatin1String("body")))
            continue;
        b = bulletInList(child(child(sp, QStringLiteral("txBody")), QStringLiteral("lstStyle")), level);
        if(b >= 0)
            return b;
        break;
    }
    return bulletInList(child(styles, title ? QStringLiteral("titleStyle") : QStringLiteral("bodyStyle")), level) == 1;
}

// Where a shape sits on the slide, 0..1 of the slide's size, through the
// groups it is in.
QRectF shapeRect(const QDomElement &shape, const QSizeF &slide)
{
    const QDomElement frame = child(child(shape, QStringLiteral("spPr")), QStringLiteral("xfrm"));
    const QDomElement off = child(frame, QStringLiteral("off")), ext = child(frame, QStringLiteral("ext"));
    if(off.isNull() || ext.isNull() || slide.isEmpty())
        return QRectF();
    QRectF r(off.attribute(QStringLiteral("x")).toDouble(), off.attribute(QStringLiteral("y")).toDouble(),
             ext.attribute(QStringLiteral("cx")).toDouble(), ext.attribute(QStringLiteral("cy")).toDouble());
    for(QDomElement g = shape.parentNode().toElement(); !g.isNull(); g = g.parentNode().toElement())
    {
        if(local(g) != QLatin1String("grpSp"))
            continue;
        const QDomElement gf = child(child(g, QStringLiteral("grpSpPr")), QStringLiteral("xfrm"));
        const QDomElement gOff = child(gf, QStringLiteral("off")), gExt = child(gf, QStringLiteral("ext"));
        const QDomElement cOff = child(gf, QStringLiteral("chOff")), cExt = child(gf, QStringLiteral("chExt"));
        if(gOff.isNull() || gExt.isNull() || cOff.isNull() || cExt.isNull())
            continue;
        const double sx = gExt.attribute(QStringLiteral("cx")).toDouble() / qMax(1.0, cExt.attribute(QStringLiteral("cx")).toDouble());
        const double sy = gExt.attribute(QStringLiteral("cy")).toDouble() / qMax(1.0, cExt.attribute(QStringLiteral("cy")).toDouble());
        r = QRectF(gOff.attribute(QStringLiteral("x")).toDouble() + (r.x() - cOff.attribute(QStringLiteral("x")).toDouble()) * sx,
                   gOff.attribute(QStringLiteral("y")).toDouble() + (r.y() - cOff.attribute(QStringLiteral("y")).toDouble()) * sy,
                   r.width() * sx, r.height() * sy);
    }
    return QRectF(r.x() / slide.width(), r.y() / slide.height(), r.width() / slide.width(), r.height() / slide.height());
}

bool turnedOrCropped(const QDomElement &shape)
{
    const QDomElement frame = child(child(shape, QStringLiteral("spPr")), QStringLiteral("xfrm"));
    if(frame.attribute(QStringLiteral("rot"), QStringLiteral("0")) != QLatin1String("0")
            || frame.attribute(QStringLiteral("flipH")) == QLatin1String("1")
            || frame.attribute(QStringLiteral("flipV")) == QLatin1String("1"))
        return true;
    const QDomElement crop = child(child(shape, QStringLiteral("blipFill")), QStringLiteral("srcRect"));
    for(const char *side : {"l", "t", "r", "b"})
        if(crop.attribute(QLatin1String(side), QStringLiteral("0")) != QLatin1String("0"))
            return true;
    return false;
}

// ---------------------------------------------------------------- media

struct MediaSource
{
    QString kind;       // video, audio, gif
    QString part;       // in the package, or
    QString path;       // a file on disk (a linked video)
    QString suffix;
    QRectF rect;
};

// Copies a video, sound or animated picture into the media folder under a
// name made from its content, so the same file is kept once.
QString keepMedia(const Package &pkg, const MediaSource &m, const QString &mediaDir)
{
    QByteArray data;
    if(!m.part.isEmpty())
        data = pkg.parts.value(m.part);
    else
    {
        QFile f(m.path);
        if(f.open(QIODevice::ReadOnly))
            data = f.readAll();
    }
    if(data.isEmpty() || mediaDir.isEmpty())
        return QString();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(20))
            + QLatin1Char('.') + m.suffix;
    const QString target = QDir(mediaDir).filePath(name);
    if(QFileInfo(target).size() == data.size())
        return name;
    if(!QDir().mkpath(mediaDir))
        return QString();
    QFile out(target);
    if(!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
        return QString();
    return name;
}

bool isSound(const QString &suffix)
{
    static const QSet<QString> sounds = {"wav", "mp3", "m4a", "wma", "aac", "ogg", "mid", "midi", "aif", "aiff", "au"};
    return sounds.contains(suffix);
}

// A linked video is only named in the file. Look where the name says, then
// beside the presentation.
QString findLinked(const QString &address, const QString &deckFolder)
{
    QString path = address;
    if(path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        path = QUrl(path).toLocalFile();
    else if(path.contains(QLatin1String("://")))
        return QString();       // on the internet
    path = QUrl::fromPercentEncoding(path.toUtf8());
    QStringList tries;
    if(QFileInfo(path).isAbsolute())
        tries << path;
    tries << QDir(deckFolder).filePath(path) << QDir(deckFolder).filePath(QFileInfo(QDir::fromNativeSeparators(path)).fileName());
    for(const QString &t : std::as_const(tries))
        if(QFileInfo(t).isFile())
            return t;
    return QString();
}

// The videos, sounds and animated pictures on a slide, by shape id.
QHash<int, MediaSource> readMedia(const Package &pkg, const QString &slidePart, const QHash<int, QDomElement> &shapes,
                                  const QSizeF &slideSize, const QString &deckFolder)
{
    QHash<QString, Relationship> rels;
    for(const Relationship &r : pkg.relationships(slidePart))
        rels.insert(r.id, r);
    QHash<int, MediaSource> out;
    for(auto it = shapes.constBegin(); it != shapes.constEnd(); ++it)
    {
        const QDomElement shape = it.value();
        if(local(shape) != QLatin1String("pic"))
            continue;
        MediaSource m;
        m.rect = shapeRect(shape, slideSize);
        const QDomElement nv = child(shape, QStringLiteral("nvPicPr"));
        QStringList ids;
        for(const char *name : {"media", "videoFile", "audioFile", "wavAudioFile", "quickTimeFile"})
            for(const QDomElement &e : descendants(nv, QLatin1String(name)))
                ids << attr(e, QStringLiteral("embed")) << attr(e, QStringLiteral("link"));
        const bool sound = !descendants(nv, QStringLiteral("audioFile")).isEmpty()
                || !descendants(nv, QStringLiteral("wavAudioFile")).isEmpty();
        for(const QString &id : std::as_const(ids))
        {
            if(id.isEmpty() || !rels.contains(id))
                continue;
            const Relationship r = rels.value(id);
            if(r.external)
            {
                const QString found = findLinked(r.target, deckFolder);
                if(found.isEmpty())
                    continue;
                m.path = found;
                m.suffix = QFileInfo(found).suffix().toLower();
            }
            else if(pkg.has(r.target))
            {
                m.part = r.target;
                m.suffix = QFileInfo(r.target).suffix().toLower();
            }
            else
                continue;
            m.kind = sound || isSound(m.suffix) ? QStringLiteral("audio") : QStringLiteral("video");
            break;
        }
        if(m.kind.isEmpty() && !turnedOrCropped(shape))
        {
            // An animated GIF: more than one frame.
            const QDomElement blip = child(child(shape, QStringLiteral("blipFill")), QStringLiteral("blip"));
            const Relationship r = rels.value(attr(blip, QStringLiteral("embed")));
            if(r.target.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive) && pkg.has(r.target)
                    && pkg.parts.value(r.target).count(QByteArray("\x00\x21\xF9\x04", 4)) > 1)
            {
                m.kind = QStringLiteral("gif");
                m.part = r.target;
                m.suffix = QStringLiteral("gif");
            }
        }
        if(!m.kind.isEmpty() && m.rect.isValid())
            out.insert(it.key(), m);
    }
    return out;
}

// The speaker notes of a slide, as plain text.
QString readNotes(const Package &pkg, const QString &slidePart)
{
    for(const Relationship &r : pkg.relationships(slidePart))
    {
        if(r.type != QLatin1String("notesSlide") || !pkg.has(r.target))
            continue;
        QDomDocument doc;
        if(!parse(pkg.parts.value(r.target), doc))
            return QString();
        QStringList lines;
        for(const QDomElement &shape : descendants(doc.documentElement(), QStringLiteral("sp")))
        {
            const QDomElement holder = descendants(child(shape, QStringLiteral("nvSpPr")), QStringLiteral("ph")).value(0);
            if(holder.attribute(QStringLiteral("type")) != QLatin1String("body"))
                continue;
            for(const QDomElement &p : descendants(shape, QStringLiteral("p")))
            {
                QString line;
                for(const QDomElement &t : descendants(p, QStringLiteral("t")))
                    line += t.text();
                if(!line.trimmed().isEmpty())
                    lines << line.trimmed();
            }
        }
        return lines.join(QLatin1Char('\n')).left(1500);
    }
    return QString();
}

// ---------------------------------------------------------------- one slide

struct State
{
    QSet<int> hiddenShapes;
    QSet<Target> hiddenLines;
    bool operator==(const State &o) const { return hiddenShapes == o.hiddenShapes && hiddenLines == o.hiddenLines; }
};

// The slide's XML with the animations and transition taken out and the
// state's hidden shapes and lines applied. A copy also loses its custom data
// (see copyRelationships).
QByteArray renderState(const QByteArray &xml, const State &state, const Inherited &inherited, bool copy)
{
    QDomDocument doc;
    if(!parse(xml, doc))
        return xml;
    QDomElement slide = doc.documentElement();
    if(copy)
    {
        for(QDomElement data : descendants(slide, QStringLiteral("custDataLst")))
            data.parentNode().removeChild(data);
    }
    for(QDomElement c = slide.firstChildElement(); !c.isNull();)
    {
        QDomElement next = c.nextSiblingElement();
        const QString n = local(c);
        const bool wrapsTransition = n == QLatin1String("AlternateContent")
                && !descendants(c, QStringLiteral("transition")).isEmpty();
        if(n == QLatin1String("timing") || n == QLatin1String("transition") || wrapsTransition)
            slide.removeChild(c);
        c = next;
    }
    QHash<int, QDomElement> shapes;
    indexShapes(slide, shapes);
    for(const Target &line : state.hiddenLines)
    {
        if(state.hiddenShapes.contains(line.shape))
            continue;
        const QDomElement shape = shapes.value(line.shape);
        const QList<QDomElement> paragraphs = children(child(shape, QStringLiteral("txBody")), QStringLiteral("p"));
        if(line.paragraph >= 0 && line.paragraph < paragraphs.count())
            hideParagraph(doc, paragraphs.at(line.paragraph), hasBullet(paragraphs.at(line.paragraph), shape, inherited));
    }
    for(int id : state.hiddenShapes)
        removeShape(slide, id);
    return doc.toByteArray(-1);
}

struct SlidePlan
{
    QList<State> states;
    QList<SlideStep> steps;     // one per state
};

SlidePlan planSlide(const Package &pkg, const QString &part, const QSizeF &slideSize, const QString &deckFolder,
                    const QString &mediaDir, int slideNumber)
{
    SlidePlan plan;
    SlideStep first;
    first.slide = slideNumber;
    first.notes = readNotes(pkg, part);
    State state;

    QDomDocument doc;
    if(!parse(pkg.parts.value(part), doc))
    {
        plan.states << state;
        plan.steps << first;
        return plan;
    }
    const QDomElement slide = doc.documentElement();
    readTransition(slide, first);

    QHash<int, QDomElement> shapes;
    indexShapes(slide, shapes);
    const QHash<int, MediaSource> media = readMedia(pkg, part, shapes, slideSize, deckFolder);
    QHash<QString, Relationship> rels;
    for(const Relationship &r : pkg.relationships(part))
        rels.insert(r.id, r);

    // The main sequence (clicks), then the sequences that start when a shape
    // is clicked: the operator has no mouse on the slide, so those follow as
    // further clicks.
    QList<Effect> effects;
    const QDomElement timing = child(slide, QStringLiteral("timing"));
    for(const QDomElement &node : descendants(timing, QStringLiteral("cTn"), true))
    {
        const QString type = node.attribute(QStringLiteral("nodeType"));
        if(type == QLatin1String("mainSeq"))
            effects << readSequence(node, false);
    }
    for(const QDomElement &node : descendants(timing, QStringLiteral("cTn"), true))
    {
        if(node.attribute(QStringLiteral("nodeType")) != QLatin1String("interactiveSeq"))
            continue;
        QList<Effect> triggered = readSequence(node, true);
        if(!triggered.isEmpty())
            triggered.first().start = QStringLiteral("clickEffect");
        effects << triggered;
    }

    // What is hidden when the slide appears: everything whose first effect is an entrance.
    QSet<Target> seen;
    for(const Effect &e : std::as_const(effects))
    {
        if(e.cls != QLatin1String("entr") && e.cls != QLatin1String("exit"))
            continue;
        for(const Target &t : e.targets)
        {
            if(seen.contains(t) || !shapes.contains(t.shape))
                continue;
            seen.insert(t);
            if(e.cls == QLatin1String("entr"))
            {
                if(t.paragraph < 0)
                    state.hiddenShapes.insert(t.shape);
                else
                    state.hiddenLines.insert(t);
            }
        }
    }

    // A sound or video set to hide while it is not playing: its picture
    // stays off the stills.
    for(const QDomElement &node : descendants(timing, QStringLiteral("cMediaNode"), true))
    {
        if(node.attribute(QStringLiteral("showWhenStopped")) != QLatin1String("0"))
            continue;
        for(const QDomElement &t : descendants(node, QStringLiteral("spTgt")))
            if(shapes.contains(t.attribute(QStringLiteral("spid")).toInt()))
                state.hiddenShapes.insert(t.attribute(QStringLiteral("spid")).toInt());
    }

    auto gifs = [&](const State &s) {
        QList<SlideMedia> out;
        for(auto it = media.constBegin(); it != media.constEnd(); ++it)
        {
            if(it.value().kind != QLatin1String("gif") || s.hiddenShapes.contains(it.key()))
                continue;
            SlideMedia m;
            m.kind = it.value().kind;
            m.file = keepMedia(pkg, it.value(), mediaDir);
            m.rect = it.value().rect;
            m.loop = true;
            if(!m.file.isEmpty())
                out << m;
        }
        return out;
    };
    auto playable = [&](int shape) {
        SlideMedia m;
        const MediaSource source = media.value(shape);
        if(source.kind == QLatin1String("video") || source.kind == QLatin1String("audio"))
        {
            m.kind = source.kind;
            m.file = keepMedia(pkg, source, mediaDir);
            m.rect = source.rect;
        }
        return m;
    };

    first.media = gifs(state);
    plan.states << state;
    plan.steps << first;

    // Effects that start together are one step.
    QSet<int> played;
    int previousOffset = 0;
    bool clickWaiting = false;      // a click whose own effects showed nothing new
    for(int i = 0; i < effects.count();)
    {
        const Effect &lead = effects.at(i);
        int end = i + 1;
        while(end < effects.count() && effects.at(end).start == QLatin1String("withEffect") && !effects.at(end).interactive == !lead.interactive)
            ++end;

        // What follows a click that showed nothing (it only emphasised or
        // moved something) still waited for that click in PowerPoint.
        const bool clicked = lead.start == QLatin1String("clickEffect");
        SlideStep step;
        step.slide = slideNumber;
        step.automatic = !clicked && !clickWaiting;
        const int offset = clicked ? 0 : lead.offsetMs;
        step.delayMs = step.automatic ? qBound(0, offset - previousOffset, 60000) : 0;
        State next = state;
        bool classified = false;
        for(int k = i; k < end; ++k)
        {
            const Effect &e = effects.at(k);
            const bool appears = e.cls == QLatin1String("entr"), leaves = e.cls == QLatin1String("exit");
            bool changes = false;
            for(const Target &t : e.targets)
            {
                if(!shapes.contains(t.shape) || (!appears && !leaves))
                    continue;
                if(t.paragraph < 0)
                {
                    changes |= appears ? next.hiddenShapes.remove(t.shape) : !next.hiddenShapes.contains(t.shape);
                    if(leaves)
                        next.hiddenShapes.insert(t.shape);
                }
                else
                {
                    changes |= appears ? next.hiddenLines.remove(t) : !next.hiddenLines.contains(t);
                    if(leaves)
                        next.hiddenLines.insert(t);
                }
            }
            if(changes && !classified)
            {
                classify(e, step);
                classified = true;
            }
            for(int shape : e.plays)
            {
                // PowerPoint adds "play when its picture is clicked" to every
                // video and sound; one that a click has started already is not played again.
                if(e.interactive && played.contains(shape))
                    continue;
                const SlideMedia m = playable(shape);
                if(!m.file.isEmpty())
                {
                    step.media << m;
                    played.insert(shape);
                }
            }
            for(const QString &id : e.sounds)
            {
                const Relationship r = rels.value(id);
                if(r.external || !pkg.has(r.target))
                    continue;
                MediaSource source;
                source.kind = QStringLiteral("audio");
                source.part = r.target;
                source.suffix = QFileInfo(r.target).suffix().toLower();
                SlideMedia m;
                m.kind = source.kind;
                m.file = keepMedia(pkg, source, mediaDir);
                if(!m.file.isEmpty())
                    step.media << m;
            }
        }
        // A step that shows nothing new and plays nothing (an emphasis or a
        // motion path alone) would be a click with no effect: left out.
        if(!(next == state) || !step.media.isEmpty())
        {
            if(next == state)
            {
                step.effect = QStringLiteral("cut");
                step.durationMs = 0;
            }
            else if(!classified)
                step.effect = QStringLiteral("fade");
            step.index = plan.steps.count();
            step.media << gifs(next);
            state = next;
            plan.states << state;
            plan.steps << step;
            previousOffset = offset;
            clickWaiting = false;
        }
        else if(clicked)
            clickWaiting = true;
        i = end;
    }

    // A video nothing starts would never play: in PowerPoint it waits for a
    // click on the picture. It gets a step of its own at the end.
    for(auto it = media.constBegin(); it != media.constEnd(); ++it)
    {
        if(it.value().kind != QLatin1String("video") || played.contains(it.key()) || state.hiddenShapes.contains(it.key()))
            continue;
        SlideStep step;
        step.slide = slideNumber;
        step.index = plan.steps.count();
        step.effect = QStringLiteral("cut");
        step.durationMs = 0;
        const SlideMedia m = playable(it.key());
        if(m.file.isEmpty())
            continue;
        step.media << m << gifs(state);
        plan.states << state;
        plan.steps << step;
    }
    return plan;
}
}   // namespace

// ---------------------------------------------------------------- SlideStep as text

QString SlideStep::toText() const
{
    if(!isValid())
        return QString();
    QJsonObject o;
    o.insert(QStringLiteral("s"), slide);
    o.insert(QStringLiteral("i"), index);
    if(automatic) o.insert(QStringLiteral("a"), 1);
    if(delayMs) o.insert(QStringLiteral("d"), delayMs);
    if(!effect.isEmpty()) o.insert(QStringLiteral("e"), effect);
    if(!direction.isEmpty()) o.insert(QStringLiteral("r"), direction);
    if(exit) o.insert(QStringLiteral("x"), 1);
    o.insert(QStringLiteral("t"), durationMs);
    if(!media.isEmpty())
    {
        QJsonArray list;
        for(const SlideMedia &m : media)
        {
            QJsonObject j;
            j.insert(QStringLiteral("k"), m.kind);
            j.insert(QStringLiteral("f"), m.file);
            j.insert(QStringLiteral("r"), QJsonArray{m.rect.x(), m.rect.y(), m.rect.width(), m.rect.height()});
            if(m.volume != 1.0) j.insert(QStringLiteral("v"), m.volume);
            if(m.loop) j.insert(QStringLiteral("l"), 1);
            list << j;
        }
        o.insert(QStringLiteral("m"), list);
    }
    if(!notes.isEmpty()) o.insert(QStringLiteral("n"), notes);
    return QLatin1String(stepPrefix) + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

SlideStep SlideStep::fromText(const QString &text)
{
    SlideStep step;
    if(!text.startsWith(QLatin1String(stepPrefix)))
        return step;
    const QJsonObject o = QJsonDocument::fromJson(text.mid(int(sizeof(stepPrefix)) - 1).toUtf8()).object();
    step.slide = o.value(QStringLiteral("s")).toInt();
    step.index = o.value(QStringLiteral("i")).toInt();
    step.automatic = o.value(QStringLiteral("a")).toInt() != 0;
    step.delayMs = o.value(QStringLiteral("d")).toInt();
    step.effect = o.value(QStringLiteral("e")).toString();
    step.direction = o.value(QStringLiteral("r")).toString();
    step.exit = o.value(QStringLiteral("x")).toInt() != 0;
    step.durationMs = o.value(QStringLiteral("t")).toInt(500);
    for(const QJsonValue &v : o.value(QStringLiteral("m")).toArray())
    {
        const QJsonObject j = v.toObject();
        SlideMedia m;
        m.kind = j.value(QStringLiteral("k")).toString();
        m.file = j.value(QStringLiteral("f")).toString();
        const QJsonArray r = j.value(QStringLiteral("r")).toArray();
        m.rect = QRectF(r.at(0).toDouble(), r.at(1).toDouble(), r.at(2).toDouble(), r.at(3).toDouble());
        m.volume = j.value(QStringLiteral("v")).toDouble(1.0);
        m.loop = j.value(QStringLiteral("l")).toInt() != 0;
        // Only a plain file name: the text may come from a schedule file made elsewhere.
        if(!m.file.isEmpty() && QFileInfo(m.file).fileName() == m.file)
            step.media << m;
    }
    step.notes = o.value(QStringLiteral("n")).toString();
    return step;
}

// ---------------------------------------------------------------- the whole file

QString PresentationSteps::mediaDir()
{
    return QFileInfo(QSqlDatabase::database().databaseName()).absoluteDir().filePath(QStringLiteral("slide-media"));
}

QStringList PresentationSteps::mediaOf(int slideShowId)
{
    QStringList files;
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT path FROM Slides WHERE ss_id = ? AND path LIKE 'cgf-step:%'"));
    q.addBindValue(slideShowId);
    q.exec();
    while(q.next())
        for(const SlideMedia &m : SlideStep::fromText(q.value(0).toString()).media)
            if(!files.contains(m.file))
                files << m.file;
    return files;
}

void PresentationSteps::removeUnusedMedia(const QStringList &files)
{
    if(files.isEmpty())
        return;
    QSet<QString> used;
    QSqlQuery q(QStringLiteral("SELECT path FROM Slides WHERE path LIKE 'cgf-step:%'"));
    while(q.next())
        for(const SlideMedia &m : SlideStep::fromText(q.value(0).toString()).media)
            used.insert(m.file);
    const QDir folder(mediaDir());
    for(const QString &file : files)
        if(!used.contains(file) && QFileInfo(file).fileName() == file)
            QFile::remove(folder.filePath(file));
}

bool PresentationSteps::expand(const QString &pptx, const QString &expanded, const QString &mediaDir,
                               QList<SlideStep> &steps, QString &error)
{
    steps.clear();
    error.clear();
    Package pkg;
    if(!readPackage(pptx, pkg))
    {
        error = QStringLiteral("not a zip file");
        return false;
    }

    // The presentation part, its slides in order and the slide size.
    QString main;
    for(const Relationship &r : pkg.relationships(QString()))
        if(r.type == QLatin1String("officeDocument"))
            main = r.target;
    QDomDocument presentation;
    if(main.isEmpty() || !parse(pkg.parts.value(main), presentation))
    {
        error = QStringLiteral("no presentation part");
        return false;
    }
    QDomElement root = presentation.documentElement();
    QDomElement list = child(root, QStringLiteral("sldIdLst"));
    const QDomElement size = child(root, QStringLiteral("sldSz"));
    const QSizeF slideSize(size.attribute(QStringLiteral("cx")).toDouble(), size.attribute(QStringLiteral("cy")).toDouble());
    QHash<QString, Relationship> mainRels;
    for(const Relationship &r : pkg.relationships(main))
        mainRels.insert(r.id, r);
    QDomDocument mainRelsDoc, types;
    if(list.isNull() || !parse(pkg.parts.value(Package::relsName(main)), mainRelsDoc)
            || !parse(pkg.parts.value(QStringLiteral("[Content_Types].xml")), types))
    {
        error = QStringLiteral("no slide list");
        return false;
    }

    int maxId = 255;
    for(const QDomElement &s : children(list, QStringLiteral("sldId")))
        maxId = qMax(maxId, s.attribute(QStringLiteral("id")).toInt());
    const QString mainFolder = main.left(main.lastIndexOf(QLatin1Char('/')) + 1);
    const QString deckFolder = QFileInfo(pptx).absolutePath();

    int shown = 0, added = 0;
    for(QDomElement entry : children(list, QStringLiteral("sldId")))
    {
        const QString idName = [&] {
            const QDomNamedNodeMap all = entry.attributes();
            for(int i = 0; i < all.count(); ++i)
                if(all.item(i).nodeName().endsWith(QLatin1String(":id")))
                    return all.item(i).nodeName();
            return QStringLiteral("r:id");
        }();
        const QString part = mainRels.value(entry.attribute(idName)).target;
        if(!pkg.has(part))
            continue;
        QDomDocument peek;
        if(parse(pkg.parts.value(part), peek) && peek.documentElement().attribute(QStringLiteral("show")) == QLatin1String("0"))
            continue;       // a hidden slide: PowerPoint and LibreOffice leave it out

        const SlidePlan plan = planSlide(pkg, part, slideSize, deckFolder, mediaDir, ++shown);
        const QByteArray original = pkg.parts.value(part);
        const Inherited inherited = inheritedBy(pkg, part);
        pkg.add(part, renderState(original, plan.states.first(), inherited, false));
        steps << plan.steps.first();

        // The further states are slides of their own, right after this one.
        // They share its pictures; the notes stay with the first.
        QDomElement after = entry;
        for(int k = 1; k < plan.states.count(); ++k)
        {
            ++added;
            const QString folder = part.left(part.lastIndexOf(QLatin1Char('/')) + 1);
            const QString name = folder + QStringLiteral("cgfstep%1.xml").arg(added);
            pkg.add(name, renderState(original, plan.states.at(k), inherited, true));
            const QByteArray relationships = copyRelationships(pkg, types, part, QString::number(added));
            if(!relationships.isEmpty())
                pkg.add(Package::relsName(name), relationships);

            QDomElement override = types.createElement(QStringLiteral("Override"));
            override.setAttribute(QStringLiteral("PartName"), QLatin1Char('/') + name);
            override.setAttribute(QStringLiteral("ContentType"),
                                  QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.slide+xml"));
            types.documentElement().appendChild(override);

            const QString relId = QStringLiteral("rIdCgf%1").arg(added);
            QDomElement rel = mainRelsDoc.createElement(QStringLiteral("Relationship"));
            rel.setAttribute(QStringLiteral("Id"), relId);
            rel.setAttribute(QStringLiteral("Type"),
                             QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"));
            rel.setAttribute(QStringLiteral("Target"), name.startsWith(mainFolder) ? name.mid(mainFolder.length())
                                                                                   : QLatin1Char('/') + name);
            mainRelsDoc.documentElement().appendChild(rel);

            QDomElement extra = presentation.createElement(entry.nodeName());
            extra.setAttribute(QStringLiteral("id"), ++maxId);
            extra.setAttribute(idName, relId);
            list.insertAfter(extra, after);
            after = extra;
            steps << plan.steps.at(k);
        }
    }
    if(shown == 0)
    {
        error = QStringLiteral("no slides");
        return false;
    }

    // Sections list every slide by id; the new slides are in none. The
    // sections do not matter for drawing, so they go.
    for(const QDomElement &sections : descendants(root, QStringLiteral("sectionLst")))
    {
        QDomNode holder = sections.parentNode();
        QDomNode owner = holder.parentNode();
        owner.removeChild(holder);
        if(owner.firstChildElement().isNull())
            owner.parentNode().removeChild(owner);
    }

    pkg.add(main, presentation.toByteArray(-1));
    pkg.add(Package::relsName(main), mainRelsDoc.toByteArray(-1));
    pkg.add(QStringLiteral("[Content_Types].xml"), types.toByteArray(-1));
    if(!writePackage(expanded, pkg))
    {
        error = QStringLiteral("cannot write %1").arg(expanded);
        steps.clear();
        return false;
    }
    return true;
}
