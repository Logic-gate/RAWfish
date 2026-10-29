# RAWfish-indRAW Sailfish application (the interface; package rawfish-ir-gui).
TEMPLATE = app
TARGET = rawfish-ir
# The engine's program is 'rawfish-ir-cli' (package rawfish-ir-cli); the app
# is 'rawfish-ir' and launches it as a child process. The sources keep the
# engine's development name, indraw.

QT += qml quick
CONFIG += sailfishapp

SOURCES += \
    main.cpp \
    indrawpreview.cpp

HEADERS += \
    indrawpreview.h

# The .desktop is installed by the .spec (%install), not here: the sailfishapp
# qmake integration can shadow a .pro desktop INSTALLS rule.
OTHER_FILES += \
    qml/indraw.qml \
    qml/ShadingAssistant.qml \
    rpm/rawfish-ir-gui.spec \
    rawfish-ir.desktop \
    icons/rawfish-ir-86.png icons/rawfish-ir-108.png icons/rawfish-ir-128.png icons/rawfish-ir-172.png \
    settings/rawfish-ir.json \
    settings/SettingsPage.qml
