## A command-line front for the app's presentation step splitter
## (src/sources/presentationsteps.cpp), for tools/presentations/corpus.py:
##   stepstool <in.pptx> <out.pptx> <media folder>
## writes the expanded presentation and prints one line per step.
QT = core xml sql core-private
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = stepstool
INCLUDEPATH += ../../../src/headers
SOURCES += main.cpp ../../../src/sources/presentationsteps.cpp
HEADERS += ../../../src/headers/presentationsteps.hpp
DESTDIR = out/bin
OBJECTS_DIR = out/obj
MOC_DIR = out/moc
