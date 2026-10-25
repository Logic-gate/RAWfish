# indRAW Sailfish application (phase 3 skeleton: the viewfinder).
TEMPLATE = app
TARGET = indraw-app
# The CLI engine binary is 'indraw'; the app target is 'indraw-app' to avoid a
# name clash. The app launches 'indraw' as a child process.

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
    rpm/indraw.spec \
    indraw.desktop \
    icons/indraw-86.png icons/indraw-108.png icons/indraw-128.png icons/indraw-172.png \
    settings/indraw.json \
    settings/SettingsPage.qml
