QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2 dbus network

CONFIG += c++17 release
TARGET = omascribe
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/markdownhighlighter.h \
    src/systemtheme.h

HEADERS += src/runtimeprofile.h src/editorsession.h src/controltransport.h
HEADERS += src/appversion.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/markdownhighlighter.cpp \
    src/systemtheme.cpp

SOURCES += src/runtimeprofile.cpp src/editorsession.cpp src/controltransport.cpp

RESOURCES += src/resources.qrc
