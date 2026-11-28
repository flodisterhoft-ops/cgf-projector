#!/usr/bin/env python3
"""Which Windows font is nearest in width to a font that comes with Office?

    python tools/presentations/fonts.py [font name ...]

On a computer without PowerPoint the import draws slides with LibreOffice,
and fonts that come only with Office are missing there. The list of
replacements in src/scripts/import-presentation.ps1 ($FontReplacements) was
chosen with this: for each font it measures a line of text and prints the
fonts every Windows 10 and 11 has, nearest in width first (+ is wider, so
lines may wrap earlier; - is narrower). Run it on a computer that has Office,
because the Office fonts have to be there to be measured. Needs Pillow.

Without names it measures the fonts in the list below.
"""

import os
import re
import sys
import winreg

from PIL import ImageFont

SAMPLE = ("The grace of our Lord Jesus Christ, and the love of God, and the communion of the Holy Spirit "
          "be with you all. Die Gnade unseres Herrn Jesus Christus und die Liebe Gottes sei mit euch allen. 0123456789")

# Fonts that come with Windows 10 and 11, by kind.
WINDOWS = {
    "serif": ["Times New Roman", "Georgia", "Cambria", "Constantia", "Palatino Linotype", "Sitka Text", "Sylfaen"],
    "sans": ["Arial", "Calibri", "Candara", "Corbel", "Franklin Gothic Medium", "Segoe UI", "Tahoma", "Trebuchet MS",
             "Verdana", "Microsoft Sans Serif", "Lucida Sans Unicode", "Bahnschrift"],
    "script": ["Segoe Script", "Segoe Print", "Gabriola", "Ink Free", "Comic Sans MS"],
    "display": ["Impact", "Arial Black", "Segoe UI Black", "Georgia", "Bahnschrift"],
}

OFFICE = [("Garamond", "serif"), ("Baskerville Old Face", "serif"), ("Goudy Old Style", "serif"), ("Perpetua", "serif"),
          ("Book Antiqua", "serif"), ("Rockwell", "serif"), ("Bookman Old Style", "serif"), ("Century Schoolbook", "serif"),
          ("Calisto MT", "serif"), ("Century Gothic", "sans"), ("Gill Sans MT", "sans"), ("Tw Cen MT", "sans"),
          ("Berlin Sans FB", "sans"), ("Franklin Gothic Book", "sans"), ("Arial Narrow", "sans"), ("Montserrat", "sans"),
          ("Monotype Corsiva", "script"), ("Kunstler Script", "script"), ("Cooper Black", "display"), ("Broadway", "display")]


def installed():
    files = {}
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            key = winreg.OpenKey(hive, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts")
        except OSError:
            continue
        i = 0
        while True:
            try:
                name, value, _ = winreg.EnumValue(key, i)
            except OSError:
                break
            i += 1
            name = re.sub(r"\s*\((TrueType|OpenType|All res)\)\s*$", "", name)
            for part in name.split(" & "):
                files[part.strip().lower()] = value if os.path.isabs(value) else os.path.join(
                    os.environ.get("SystemRoot", r"C:\Windows"), "Fonts", value)
    return files


def width(files, family):
    path = files.get(family.lower())
    if not path or not os.path.exists(path):
        return None
    try:
        return ImageFont.truetype(path, 100).getlength(SAMPLE)
    except OSError:
        return None


def main():
    files = installed()
    wanted = [(name, None) for name in sys.argv[1:]] or OFFICE
    for name, kind in wanted:
        own = width(files, name)
        if own is None:
            print("%-22s not on this computer" % name)
            continue
        rows = []
        for group, families in WINDOWS.items():
            if kind and group != kind:
                continue
            for family in families:
                w = width(files, family)
                if w and family != name:
                    rows.append((w / own - 1, family))
        rows.sort(key=lambda r: abs(r[0]))
        print("%-22s %s" % (name, ", ".join("%s %+.1f%%" % (f, d * 100) for d, f in rows[:5])))


if __name__ == "__main__":
    main()
