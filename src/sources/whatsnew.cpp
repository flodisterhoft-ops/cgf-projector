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

#include "../headers/whatsnew.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace {

const char stateFileName[] = "cgf-whats-new.json";
// Up to this many songs (or songbooks) of one kind are listed by name; a
// larger batch shows only its number.
const int listLimit = 5;
const char *const changeKinds[] = {"added", "updated", "removed"};

QString T(const char *text)
{
    return QCoreApplication::translate("WhatsNew", text);
}

QString statePath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QLatin1String(stateFileName));
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

void writeJson(const QString &path, const QJsonObject &o)
{
    QSaveFile f(path);
    if(f.open(QIODevice::WriteOnly))
    {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        if(f.commit())
            return;
    }
    qWarning("What's new: cannot write %s", qPrintable(path));
}

int compareVersions(QString a, QString b)
{
    auto parts = [](QString v) {
        if(v.startsWith(QLatin1String("app-")))
            v = v.mid(4);
        QList<int> p;
        for(const QString &s : v.split(QLatin1Char('.')))
            p << s.toInt();
        while(p.size() < 3)
            p << 0;
        return p;
    };
    const QList<int> x = parts(a), y = parts(b);
    for(int i = 0; i < 3; ++i)
        if(x.at(i) != y.at(i))
            return x.at(i) < y.at(i) ? -1 : 1;
    return 0;
}

QJsonArray programVersions() // newest first
{
    QFile f(QStringLiteral(":/whats-new.json"));
    if(!f.open(QIODevice::ReadOnly))
        return QJsonArray();
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("versions")).toArray();
}

int kindOrder(const QString &kind)
{
    static const QStringList order = {"new", "changed", "fixed", "removed"};
    const int i = order.indexOf(kind);
    return i < 0 ? order.size() : i;
}

// Program changes in the versions after `seen`, up to this build's, oldest
// first within each kind.
QJsonArray programItems(const QString &seen)
{
    const QString current = WhatsNew::appVersion();
    const QJsonArray versions = programVersions();
    QList<QJsonObject> items;
    for(qsizetype i = versions.size() - 1; i >= 0; --i)
    {
        const QJsonObject v = versions.at(i).toObject();
        const QString version = v.value(QStringLiteral("version")).toString();
        if(compareVersions(version, seen) <= 0 || compareVersions(version, current) > 0)
            continue;
        for(const QJsonValue &item : v.value(QStringLiteral("items")).toArray())
            items << item.toObject();
    }
    std::stable_sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return kindOrder(a.value(QStringLiteral("kind")).toString()) < kindOrder(b.value(QStringLiteral("kind")).toString());
    });
    QJsonArray out;
    for(const QJsonObject &i : std::as_const(items))
        out << i;
    return out;
}

// Several releases before the next start: a song added and then edited is
// still new; one added and removed again is no news.
QString mergedState(const QString &before, const QString &now)
{
    if(before.isEmpty())
        return now;
    if(before == QLatin1String("added"))
        return now == QLatin1String("removed") ? QString() : before;
    if(before == QLatin1String("removed") && now == QLatin1String("added"))
        return QStringLiteral("updated");
    return now;
}

QString number(qint64 n)
{
    return QLocale(QLocale::English).toString(n);
}

QJsonObject item(const QString &kind, const QString &title, const QJsonArray &lines = QJsonArray())
{
    QJsonObject o{{"kind", kind}, {"title", title}};
    if(!lines.isEmpty())
        o["lines"] = lines;
    return o;
}

QList<QJsonObject> entries(const QJsonObject &map, const char *state)
{
    QList<QJsonObject> out;
    for(auto it = map.begin(); it != map.end(); ++it)
        if(it->toObject().value(QStringLiteral("s")).toString() == QLatin1String(state))
            out << it->toObject();
    return out;
}

QJsonArray libraryItems(const QJsonObject &lib)
{
    QJsonArray out;

    // Bibles: one line each.
    const QJsonObject bibles = lib.value(QStringLiteral("bibles")).toObject();
    for(const char *state : changeKinds)
        for(const QJsonObject &b : entries(bibles, state))
        {
            const QString name = b.value(QStringLiteral("name")).toString();
            if(qstrcmp(state, "added") == 0)
                out << item("new", T("New Bible: %1").arg(name));
            else if(qstrcmp(state, "updated") == 0)
                out << item("fixed", T("Bible text corrected: %1").arg(name));
            else
                out << item("removed", T("Bible removed: %1").arg(name));
        }

    // Songbooks: one line per kind of change; a few are named.
    const QJsonObject books = lib.value(QStringLiteral("songbooks")).toObject();
    for(const char *state : changeKinds)
    {
        QList<QJsonObject> list = entries(books, state);
        if(list.isEmpty())
            continue;
        std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("name")).toString() < b.value(QStringLiteral("name")).toString();
        });
        const int n = list.size();
        const QString first = list.first().value(QStringLiteral("name")).toString();
        QJsonArray names;
        if(n > 1 && n <= listLimit)
            for(const QJsonObject &b : std::as_const(list))
                names << b.value(QStringLiteral("name")).toString();
        if(qstrcmp(state, "added") == 0)
            out << item("new", n == 1 ? T("New songbook: %1").arg(first) : T("%1 new songbooks").arg(number(n)), names);
        else if(qstrcmp(state, "updated") == 0)
            out << item("changed", n == 1 ? T("Songbook details updated: %1").arg(first)
                                          : T("Details updated for %1 songbooks").arg(number(n)), names);
        else
            out << item("removed", n == 1 ? T("Songbook removed: %1").arg(first)
                                          : T("%1 songbooks removed").arg(number(n)), names);
    }

    // Songs: one line per kind of change; a few are listed, a batch only counted.
    const QJsonObject songs = lib.value(QStringLiteral("songs")).toObject();
    for(const char *state : changeKinds)
    {
        QList<QJsonObject> list = entries(songs, state);
        if(list.isEmpty())
            continue;
        const int n = list.size();
        QJsonArray lines;
        if(n <= listLimit)
        {
            std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
                const int x = a.value(QStringLiteral("n")).toInt(), y = b.value(QStringLiteral("n")).toInt();
                return x != y ? x < y : a.value(QStringLiteral("t")).toString() < b.value(QStringLiteral("t")).toString();
            });
            for(const QJsonObject &s : std::as_const(list))
            {
                const int no = s.value(QStringLiteral("n")).toInt();
                const QString title = s.value(QStringLiteral("t")).toString();
                lines << (no > 0 ? QStringLiteral("%1  %2").arg(no).arg(title) : title);
            }
        }
        if(qstrcmp(state, "added") == 0)
            out << item("new", n == 1 ? T("1 new song") : T("%1 new songs").arg(number(n)), lines);
        else if(qstrcmp(state, "updated") == 0)
            out << item("changed", n == 1 ? T("1 song updated") : T("%1 songs updated").arg(number(n)), lines);
        else
            out << item("removed", n == 1 ? T("1 song removed") : T("%1 songs removed").arg(number(n)), lines);
    }
    return out;
}

QJsonObject content(const QJsonArray &program, const QJsonArray &library, int libraryVersion)
{
    QJsonArray sections;
    if(!program.isEmpty())
        sections << QJsonObject{{"title", T("Program")}, {"items", program}};
    if(!library.isEmpty())
        sections << QJsonObject{{"title", T("Library")}, {"items", library}};
    return QJsonObject{{"app", WhatsNew::appVersion()}, {"library", libraryVersion}, {"sections", sections}};
}

} // namespace

QString WhatsNew::appVersion()
{
    const QJsonArray versions = programVersions();
    return versions.isEmpty() ? QString() : versions.first().toObject().value(QStringLiteral("version")).toString();
}

void WhatsNew::recordLibrary(const QString &dataDir, const QJsonObject &changes)
{
    const QString path = statePath(dataDir);
    QJsonObject state = readJson(path);
    QJsonObject lib = state.value(QStringLiteral("library")).toObject();
    for(const char *kind : {"songs", "songbooks", "bibles"})
    {
        QJsonObject map = lib.value(QLatin1String(kind)).toObject();
        for(const QJsonValue &v : changes.value(QLatin1String(kind)).toArray())
        {
            const QJsonObject e = v.toObject();
            const QString key = QString::number(e.value(QStringLiteral("id")).toInteger());
            QJsonObject entry = map.value(key).toObject();
            const QString s = mergedState(entry.value(QStringLiteral("s")).toString(), e.value(QStringLiteral("s")).toString());
            if(s.isEmpty())
            {
                map.remove(key);
                continue;
            }
            for(auto it = e.begin(); it != e.end(); ++it)
                if(it.key() != QLatin1String("id"))
                    entry[it.key()] = it.value();
            entry[QStringLiteral("s")] = s;
            map[key] = entry;
        }
        lib[QLatin1String(kind)] = map;
    }
    state[QStringLiteral("library")] = lib;
    writeJson(path, state);
}

QJsonObject WhatsNew::pending(const QString &dataDir, const QString &previousApp, int libraryVersion)
{
    const QString path = statePath(dataDir);
    QJsonObject state = readJson(path);
    QString seen = state.value(QStringLiteral("seen_app")).toString();
    if(seen.isEmpty())
    {
        seen = previousApp;
        if(seen.isEmpty())
        {
            // A new installation: count from this version on.
            state[QStringLiteral("seen_app")] = appVersion();
            writeJson(path, state);
        }
    }
    const QJsonArray program = seen.isEmpty() ? QJsonArray() : programItems(seen);
    const QJsonArray library = libraryItems(state.value(QStringLiteral("library")).toObject());
    if(program.isEmpty() && library.isEmpty())
        return QJsonObject();
    return content(program, library, libraryVersion);
}

void WhatsNew::markShown(const QString &dataDir, const QJsonObject &shown)
{
    const QString path = statePath(dataDir);
    QJsonObject state = readJson(path);
    const QString app = shown.value(QStringLiteral("app")).toString();
    if(compareVersions(app, state.value(QStringLiteral("seen_app")).toString()) > 0)
        state[QStringLiteral("seen_app")] = app;
    state.remove(QStringLiteral("library"));
    state[QStringLiteral("last")] = shown;
    writeJson(path, state);
}

QJsonObject WhatsNew::lastShown(const QString &dataDir, int libraryVersion)
{
    const QJsonObject last = readJson(statePath(dataDir)).value(QStringLiteral("last")).toObject();
    if(!last.isEmpty() && last.value(QStringLiteral("app")).toString() == appVersion())
        return last;
    // This version's own changes: those after the version before it.
    const QJsonArray versions = programVersions();
    const QString previous = versions.size() > 1
            ? versions.at(1).toObject().value(QStringLiteral("version")).toString() : QStringLiteral("0");
    return content(programItems(previous), QJsonArray(), libraryVersion);
}

// ---------------------------------------------------------------------------
// The window

namespace {

struct KindStyle
{
    const char *tag;
    const char *ink;
    const char *tint;
};

KindStyle kindStyle(const QString &kind)
{
    if(kind == QLatin1String("new"))
        return {"New", "#1a7f37", "#e6f4ea"};
    if(kind == QLatin1String("fixed"))
        return {"Fixed", "#0b5cad", "#e5effa"};
    if(kind == QLatin1String("changed"))
        return {"Changed", "#a85b00", "#fcefdc"};
    return {"Removed", "#5b6573", "#eceff3"};
}

QLabel *plainLabel(const QString &text, const char *name, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(QLatin1String(name));
    l->setTextFormat(Qt::PlainText); // song titles are text, never markup
    return l;
}

QWidget *itemRow(const QJsonObject &it, QWidget *parent)
{
    const QString kind = it.value(QStringLiteral("kind")).toString();
    const KindStyle st = kindStyle(kind);
    const QJsonArray lines = it.value(QStringLiteral("lines")).toArray();

    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(4, 9, 4, 9);
    h->setSpacing(12);

    auto *icon = new QLabel(row);
    icon->setFixedSize(34, 34);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral("background: %1; border-radius: 8px;").arg(QLatin1String(st.tint)));
    icon->setPixmap(QIcon(QStringLiteral(":/icons/icons/whatsnew-%1.svg").arg(kind))
                    .pixmap(QSize(18, 18), row->devicePixelRatioF()));
    h->addWidget(icon, 0, lines.isEmpty() ? Qt::AlignVCenter : Qt::AlignTop);

    auto *text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(plainLabel(it.value(QStringLiteral("title")).toString(), "title", row));
    const QString detail = it.value(QStringLiteral("detail")).toString();
    if(!detail.isEmpty())
    {
        QLabel *d = plainLabel(detail, "detail", row);
        d->setWordWrap(true);
        text->addWidget(d);
    }
    for(const QJsonValue &line : lines)
        text->addWidget(plainLabel(line.toString(), "line", row));
    h->addLayout(text, 1);

    auto *tag = new QLabel(QCoreApplication::translate("WhatsNew", st.tag).toUpper(), row);
    tag->setStyleSheet(QStringLiteral("background: %1; color: %2; border-radius: 9px; padding: 2px 9px;"
                                      " font-weight: 700; font-size: 7.5pt;")
                       .arg(QLatin1String(st.tint), QLatin1String(st.ink)));
    h->addWidget(tag, 0, lines.isEmpty() ? Qt::AlignVCenter : Qt::AlignTop);
    return row;
}

} // namespace

WhatsNewDialog::WhatsNewDialog(const QJsonObject &content, QWidget *parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("whatsNew"));
    setWindowTitle(QStringLiteral("CGF Projector"));
    setStyleSheet(QStringLiteral(
        "QDialog#whatsNew, QWidget#body { background: #ffffff; }"
        "QLabel#heading { font-size: 17pt; font-weight: 600; color: #1b1f24; }"
        "QLabel#version { color: #5f6873; }"
        "QLabel#section { color: #7a838e; font-weight: 700; font-size: 7.5pt; }"
        "QLabel#title { color: #1b1f24; font-weight: 600; font-size: 10pt; }"
        "QLabel#detail, QLabel#line { color: #5f6873; }"
        "QFrame#rule { background: #e3e6ea; border: none; }"
        "QWidget#foot { background: #fafbfc; border-top: 1px solid #e3e6ea; }"
        "QPushButton#close { background: #2f7fd6; color: #ffffff; font-weight: 600; border: none;"
        " border-radius: 4px; padding: 6px 24px; }"
        "QPushButton#close:hover { background: #2a72c1; }"
        "QPushButton#close:pressed { background: #2564aa; }"));

    const int width = 580;
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("body"));
    auto *v = new QVBoxLayout(body);
    v->setContentsMargins(22, 18, 22, 12);
    v->setSpacing(0);

    auto *head = new QHBoxLayout;
    head->setSpacing(12);
    head->addWidget(plainLabel(QCoreApplication::translate("WhatsNew", "What's new"), "heading", body), 0, Qt::AlignBottom);
    const int lib = content.value(QStringLiteral("library")).toInt();
    const QString app = content.value(QStringLiteral("app")).toString();
    QLabel *version = plainLabel(lib > 0 ? QCoreApplication::translate("WhatsNew", "Program %1, Library v%2").arg(app).arg(lib)
                                         : QCoreApplication::translate("WhatsNew", "Program %1").arg(app),
                                 "version", body);
    version->setContentsMargins(0, 0, 0, 4);
    head->addWidget(version, 0, Qt::AlignBottom);
    head->addStretch(1);
    v->addLayout(head);

    for(const QJsonValue &sv : content.value(QStringLiteral("sections")).toArray())
    {
        const QJsonObject section = sv.toObject();
        v->addSpacing(16);
        QLabel *title = plainLabel(section.value(QStringLiteral("title")).toString().toUpper(), "section", body);
        QFont f = title->font();
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
        title->setFont(f);
        v->addWidget(title);
        v->addSpacing(2);
        const QJsonArray items = section.value(QStringLiteral("items")).toArray();
        for(int i = 0; i < items.size(); ++i)
        {
            if(i > 0)
            {
                auto *rule = new QFrame(body);
                rule->setObjectName(QStringLiteral("rule"));
                rule->setFixedHeight(1);
                v->addWidget(rule);
            }
            v->addWidget(itemRow(items.at(i).toObject(), body));
        }
    }
    v->addSpacing(8);

    // Long lists (a laptop that skipped many versions) scroll; normally the
    // window fits its content.
    auto *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(body);
    const int wanted = v->hasHeightForWidth() ? v->totalHeightForWidth(width) : body->sizeHint().height();
    const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const int most = screen ? screen->availableGeometry().height() * 3 / 4 : 700;
    scroll->setMinimumHeight(qMin(wanted, most));

    auto *foot = new QWidget(this);
    foot->setObjectName(QStringLiteral("foot"));
    foot->setAttribute(Qt::WA_StyledBackground);
    auto *fh = new QHBoxLayout(foot);
    fh->setContentsMargins(18, 12, 18, 12);
    fh->addStretch(1);
    auto *close = new QPushButton(QCoreApplication::translate("WhatsNew", "Close"), foot);
    close->setObjectName(QStringLiteral("close"));
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    fh->addWidget(close);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(scroll);
    outer->addWidget(foot);
    setFixedWidth(width);
}
