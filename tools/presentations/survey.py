#!/usr/bin/env python3
"""What do real presentations use? Counts animations, transitions, media,
fonts and other features across PowerPoint files, so the import
(BUILD.md, "Presentation steps") is built for the files people bring.

    python tools/presentations/survey.py <file, folder or @list.txt> ...
            [--json report.json] [--per-file]

@list.txt is a text file with one path per line. Folders are searched for
.pptx .pptm .ppsx; the older .ppt .pps and .odp are counted but not opened
(convert them to .pptx first if they should be counted in detail). Files that
live only in the cloud (OneDrive placeholders) are skipped, so nothing is
downloaded. Nothing is changed and nothing leaves this computer.

The report has no file names unless --per-file is given: presentations are
personal, and this repository is public.
"""

import argparse
import collections
import json
import os
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

P = "http://schemas.openxmlformats.org/presentationml/2006/main"
A = "http://schemas.openxmlformats.org/drawingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
REL = "http://schemas.openxmlformats.org/package/2006/relationships"
MC = "http://schemas.openxmlformats.org/markup-compatibility/2006"
NEW = (".pptx", ".pptm", ".ppsx")
OLD = (".ppt", ".pps", ".odp")

# PowerPoint's names for the preset numbers (entrance and exit share them).
PRESETS = {
    1: "Appear", 2: "Fly In", 3: "Blinds", 4: "Box", 5: "Checkerboard", 6: "Circle", 7: "Crawl In",
    8: "Diamond", 9: "Dissolve In", 10: "Fade", 11: "Flash Once", 12: "Peek In", 13: "Plus",
    14: "Random Bars", 15: "Spiral In", 16: "Split", 17: "Stretch", 18: "Strips", 19: "Swivel",
    20: "Wedge", 21: "Wheel", 22: "Wipe", 23: "Zoom", 24: "Random Effects", 25: "Boomerang",
    26: "Bounce", 27: "Color Typewriter", 28: "Credits", 29: "Easy In", 30: "Float", 31: "Grow & Turn",
    34: "Light Speed", 35: "Pinwheel", 37: "Rise Up", 38: "Swish", 39: "Thin Line", 40: "Unfold",
    41: "Whip", 42: "Ascend / Float In", 43: "Center Revolve", 45: "Faded Swivel", 47: "Descend / Float Down",
    48: "Sling", 49: "Spinner", 50: "Compress", 51: "Zip", 52: "Arc Up", 53: "Faded Zoom", 54: "Glide",
    55: "Expand", 56: "Flip", 58: "Fold",
}


def q(ns, tag):
    return "{%s}%s" % (ns, tag)


def cloud_only(path):
    """True for a OneDrive placeholder whose data is not on this disk."""
    try:
        attrs = os.stat(path).st_file_attributes
    except (OSError, AttributeError):
        return False
    return bool(attrs & 0x400000) or bool(attrs & 0x1000)   # recall on data access, offline


def collect(args):
    seen, files = set(), []

    def add(path):
        key = os.path.normcase(os.path.abspath(path))
        if key not in seen and not os.path.basename(path).startswith("~$"):
            seen.add(key)
            files.append(path)

    for arg in args:
        if arg.startswith("@"):
            with open(arg[1:], encoding="utf-8-sig") as f:
                for line in f:
                    if line.strip():
                        add(line.strip())
        elif os.path.isdir(arg):
            for folder, _, names in os.walk(arg):
                for name in names:
                    if name.lower().endswith(NEW + OLD):
                        add(os.path.join(folder, name))
        else:
            add(arg)
    return files


def rels_of(z, part):
    folder, name = part.rsplit("/", 1) if "/" in part else ("", part)
    path = (folder + "/" if folder else "") + "_rels/" + name + ".rels"
    out = {}
    if path in z.namelist():
        for rel in ET.fromstring(z.read(path)):
            target = rel.get("Target", "")
            external = rel.get("TargetMode") == "External"
            if not external:
                target = os.path.normpath(os.path.join(folder, target)).replace("\\", "/") \
                    if not target.startswith("/") else target[1:]
            out[rel.get("Id")] = (rel.get("Type", "").rsplit("/", 1)[-1], target, external)
    return out


def behaviours(node):
    """A short signature of what an effect does, from its behaviour elements."""
    sig = []
    for el in node.iter():
        tag = el.tag.rsplit("}", 1)[-1]
        if tag == "animEffect":
            sig.append("effect:%s:%s" % (el.get("transition", ""), el.get("filter", "")))
        elif tag == "anim":
            names = [n.text for n in el.iter(q(P, "attrName"))]
            sig.append("anim:" + "+".join(sorted(set(names))))
        elif tag == "animScale":
            sig.append("scale")
        elif tag == "animRot":
            sig.append("rotate")
        elif tag == "animMotion":
            sig.append("motion")
        elif tag == "animClr":
            sig.append("colour")
        elif tag == "cmd":
            sig.append("cmd:" + el.get("cmd", "").split("(")[0])
        elif tag == "set":
            names = [n.text for n in el.iter(q(P, "attrName"))]
            if names and names[0] != "style.visibility":
                sig.append("set:" + names[0])
    return ",".join(sorted(set(sig))) or "visibility only"


def survey_slide(xml, stats, deck):
    root = ET.fromstring(xml)
    if root.get("show") == "0":
        stats["hidden slides"] += 1
    text = xml.decode("utf-8", "replace")

    # ---- features
    for needle, label in (("<p:graphicFrame", None), ("<a:tbl>", "tables"), ("/chart\"", "charts"),
                          ("dgm:relIds", "SmartArt"), ("<p:oleObj", "embedded objects (OLE)"),
                          ("<p:grpSp>", "grouped shapes"), ("am3d:", "3D models"), ("<p14:contentPart", "ink"),
                          ("<a:prstTxWarp", "WordArt (warped text)"), ("<a:hlinkClick", "links or actions"),
                          ("<m:oMath", "equations")):
        if label and needle in text:
            stats["slides with " + label] += 1
            deck.add(label)

    # ---- transition (a newer one sits in mc:AlternateContent with an older fallback)
    transitions = [el for el in root.iter(q(P, "transition"))]
    if transitions:
        first = transitions[0]
        kinds = [c.tag.rsplit("}", 1)[-1] for c in first]
        kind = kinds[0] if kinds else "(no effect, timing only)"
        for c in first:
            if c.tag.rsplit("}", 1)[-1] == "prstTrans":
                kind = "prstTrans:" + c.get("prst", "")
        fallback = ""
        if len(transitions) > 1:
            fb = [c.tag.rsplit("}", 1)[-1] for c in transitions[-1]]
            fallback = " (older fallback: %s)" % (fb[0] if fb else "none")
        stats["transition: " + kind + fallback] += 1
        deck.add("transitions")
        if first.get("advTm") is not None:
            stats["slides that advance by themselves (advTm)"] += 1
        if first.get("advClick") == "0":
            stats["slides that ignore clicks (advClick=0)"] += 1

    # ---- animations
    timing = root.find(q(P, "timing"))
    if timing is not None:
        effects = [c for c in timing.iter(q(P, "cTn")) if c.get("presetClass")]
        if effects:
            stats["slides with animations"] += 1
            deck.add("animations")
        for seq in timing.iter(q(P, "cTn")):
            if seq.get("nodeType") == "interactiveSeq":
                stats["trigger sequences (start when a shape is clicked)"] += 1
        for c in effects:
            cls = c.get("presetClass")
            pid = int(c.get("presetID", "0") or 0)
            sub = c.get("presetSubtype", "0")
            name = PRESETS.get(pid, "#%d" % pid) if cls in ("entr", "exit") else "#%d" % pid
            stats["effect class: " + cls] += 1
            stats["effect start: " + (c.get("nodeType") or "?")] += 1
            stats["effect: %s %s/%s -> %s" % (cls, name, sub, behaviours(c))] += 1
            if c.get("repeatCount") not in (None, "1000"):
                stats["effects that repeat"] += 1
            if c.get("autoRev") == "1":
                stats["effects that auto-reverse"] += 1
        for el in timing.iter(q(P, "pRg")):
            stats["paragraph targets (text built line by line)"] += 1
        for el in timing.iter(q(P, "graphicEl")):
            stats["chart or SmartArt part targets"] += 1
        for el in timing.iter(q(P, "iterate")):
            stats["text animated by %s" % {"wd": "word", "lt": "letter"}.get(el.get("type"), el.get("type"))] += 1
        clicks = sum(1 for c in effects if c.get("nodeType") == "clickEffect")
        stats["click steps"] += clicks
        deck.clicks = max(getattr(deck, "clicks", 0), clicks)

    # ---- fonts named on the slide
    for typeface in re.findall(r'<a:(?:latin|ea|cs|sym) typeface="([^"]+)"', text):
        if not typeface.startswith("+"):
            stats["font: " + typeface] += 1


class Deck(set):
    pass


def survey_file(path, stats):
    deck = Deck()
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        top = rels_of(z, "")
        main = next((t for kind, t, _ in top.values() if kind == "officeDocument"), "ppt/presentation.xml")
        pres = ET.fromstring(z.read(main))
        size = pres.find(q(P, "sldSz"))
        if size is not None:
            ratio = int(size.get("cx")) / max(1, int(size.get("cy")))
            stats["slide shape: " + ("16:9" if abs(ratio - 16 / 9) < 0.02 else "4:3" if abs(ratio - 4 / 3) < 0.02
                                     else "16:10" if abs(ratio - 1.6) < 0.02 else "other (%.2f)" % ratio)] += 1
        if any(n.startswith("ppt/fonts/") for n in names):
            stats["decks with embedded fonts"] += 1
        main_rels = rels_of(z, main)
        slides = []
        ids = pres.find(q(P, "sldIdLst"))
        for sld in (ids if ids is not None else []):
            rel = main_rels.get(sld.get(q(R, "id")))
            if rel and rel[1] in names:
                slides.append(rel[1])
        stats["slides"] += len(slides)
        for part in slides:
            survey_slide(z.read(part), stats, deck)
            for kind, target, external in rels_of(z, part).values():
                ext = os.path.splitext(target)[1].lower().lstrip(".") or "?"
                if kind in ("video", "media", "audio"):
                    what = "audio" if kind == "audio" or ext in ("wav", "mp3", "m4a", "wma", "aac", "mid") else "video"
                    if kind == "media" and what == "video":
                        continue    # the same file as the "video" relationship
                    stats["%s %s: .%s" % ("linked (not in the file)" if external else "embedded", what, ext)] += 1
                    deck.add(what)
                elif kind == "image" and ext == "gif" and not external and target in names:
                    data = z.read(target)
                    frames = data.count(b"\x00\x21\xF9\x04")      # graphic control extensions
                    if frames > 1:
                        stats["animated GIF pictures"] += 1
                        deck.add("animated GIFs")
        # theme fonts
        for n in names:
            if re.fullmatch(r"ppt/theme/theme\d+\.xml", n):
                for typeface in re.findall(r'<a:latin typeface="([^"]+)"', z.read(n).decode("utf-8", "replace"))[:2]:
                    if typeface:
                        stats["theme font: " + typeface] += 1
    for feature in deck:
        stats["decks with " + feature] += 1
    stats["decks with at most %d clicks on a slide" % (1 if getattr(deck, "clicks", 0) <= 1 else
                                                       5 if deck.clicks <= 5 else 20 if deck.clicks <= 20 else 999)] += 1
    return deck


def installed_fonts():
    try:
        import winreg
    except ImportError:
        return None
    names = set()
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            key = winreg.OpenKey(hive, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts")
        except OSError:
            continue
        i = 0
        while True:
            try:
                name = winreg.EnumValue(key, i)[0]
            except OSError:
                break
            i += 1
            name = re.sub(r"\s*\((TrueType|OpenType|All res)\)\s*$", "", name)
            for part in name.split(" & "):
                names.add(part.strip().lower())
    return names


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json", help="also write the counts to this file")
    ap.add_argument("--per-file", action="store_true", help="list what each file uses (shows file names)")
    args = ap.parse_args()

    stats = collections.Counter()
    per_file = []
    for path in collect(args.paths):
        ext = os.path.splitext(path)[1].lower()
        if ext in OLD:
            stats["old-format files, not opened: " + ext] += 1
            continue
        if ext not in NEW:
            continue
        if cloud_only(path):
            stats["skipped: only in the cloud"] += 1
            continue
        try:
            deck = survey_file(path, stats)
        except (zipfile.BadZipFile, KeyError, ET.ParseError, OSError) as e:
            stats["skipped: unreadable (%s)" % type(e).__name__] += 1
            continue
        stats["decks"] += 1
        per_file.append((path, sorted(deck), getattr(deck, "clicks", 0)))

    fonts = installed_fonts()
    groups = collections.OrderedDict((title, []) for title in (
        "Files", "Animations", "Effects (class name/subtype -> what it does)", "Transitions", "Media",
        "Slides", "Fonts not installed on this computer", "Fonts"))
    for key, count in stats.most_common():
        if key.startswith("effect: "):
            groups["Effects (class name/subtype -> what it does)"].append((key[8:], count))
        elif key.startswith(("effect ", "click steps", "paragraph", "chart or", "text animated", "trigger",
                             "effects that", "slides with animations", "decks with animations",
                             "decks with at most")):
            groups["Animations"].append((key, count))
        elif key.startswith(("transition", "slides that", "decks with transitions")):
            groups["Transitions"].append((key, count))
        elif key.startswith(("embedded ", "linked ", "animated GIF", "decks with video", "decks with audio",
                             "decks with animated")):
            groups["Media"].append((key, count))
        elif key.startswith(("font: ", "theme font: ")):
            name = key.split(": ", 1)[1]
            missing = fonts is not None and not any(f == name.lower() or f.startswith(name.lower() + " ") for f in fonts)
            groups["Fonts not installed on this computer" if missing else "Fonts"].append((key, count))
        elif key.startswith(("decks", "slides", "old-format", "skipped")) and " with " not in key:
            groups["Files"].append((key, count))
        else:
            groups["Slides"].append((key, count))
    for title, rows in groups.items():
        if not rows:
            continue
        print("\n" + title)
        for key, count in rows[:60]:
            print("  %6d  %s" % (count, key))
        if len(rows) > 60:
            print("  ... and %d more" % (len(rows) - 60))
    if args.per_file:
        print("\nPer file")
        for path, features, clicks in per_file:
            print("  %s: %s%s" % (path, ", ".join(features) or "plain", "; up to %d clicks on a slide" % clicks if clicks else ""))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(dict(stats), f, indent=1, sort_keys=True)


if __name__ == "__main__":
    sys.exit(main())
