QT += core gui qml quick quickcontrols2 dbus network
CONFIG += c++17 release
TARGET = metune
TEMPLATE = app
HEADERS += \
    src/backend.h \
    src/theme.h \
    src/translator.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/cli.h
SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/portalfilepicker.cpp \
    src/cli.cpp
RESOURCES += src/resources.qrc
