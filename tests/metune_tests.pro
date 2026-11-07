QT += core gui quick quickcontrols2 testlib dbus network
CONFIG += c++17 testcase
TARGET = metune_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/theme.h ../src/translator.h ../src/filepicker.h ../src/portalfilepicker.h ../src/cli.h
SOURCES += metune_tests.cpp ../src/backend.cpp ../src/theme.cpp ../src/translator.cpp ../src/portalfilepicker.cpp ../src/cli.cpp
