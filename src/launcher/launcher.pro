## CGF Projector launcher: CGFProjector.exe in the install folder, which
## starts the current app-<version> folder (docs/song-push.md, "App updates").
## No Qt, linked statically, so it runs even if a version's libraries break.

TEMPLATE = app
CONFIG -= qt
CONFIG += c++17 windows
CONFIG -= console app_bundle
TARGET = CGFProjector

RES_DIR = $${PWD}/../win32_build
DESTDIR = $${RES_DIR}/launcher
OBJECTS_DIR = $${RES_DIR}/launcher/obj

SOURCES += launcher.cpp
RC_FILE = launcher.rc
LIBS += -lshell32
QMAKE_LFLAGS += -static -municode
