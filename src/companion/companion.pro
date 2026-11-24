## CGF Projector companion: relays library releases from the push server to
## the projector app (docs/song-push.md). Builds cgf-companion.exe next to
## CGFProjector.exe, sharing its Qt libraries.

QT = core network websockets
CONFIG += c++17
# No console window when the app starts it.
win32: CONFIG += windows
CONFIG -= console app_bundle

TARGET = cgf-companion
TEMPLATE = app

RES_DIR = $${PWD}/../unknownsys_build
win32: RES_DIR = $${PWD}/../win32_build
unix:  RES_DIR = $${PWD}/../unix_build

DESTDIR = $${RES_DIR}/bin
OBJECTS_DIR = $${RES_DIR}/companion/obj
MOC_DIR = $${RES_DIR}/companion/moc

SOURCES += main.cpp companion.cpp
HEADERS += companion.hpp
