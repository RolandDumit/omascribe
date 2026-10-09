QT += core gui quick testlib
CONFIG += testcase c++17
TEMPLATE = app
TARGET = tst_omascribe

INCLUDEPATH += ../src
SOURCES += \
    tst_omascribe.cpp \
    ../src/backend.cpp \
    ../src/markdownhighlighter.cpp
HEADERS += \
    ../src/backend.h \
    ../src/markdownhighlighter.h

QT += widgets printsupport quickcontrols2 quickdialogs2 dbus

QT += network
SOURCES += ../src/runtimeprofile.cpp ../src/editorsession.cpp ../src/controltransport.cpp
HEADERS += ../src/runtimeprofile.h ../src/editorsession.h ../src/controltransport.h
RESOURCES += ../src/resources.qrc
