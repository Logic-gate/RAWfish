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

# Translations (docs.sailfishos.org, Reference > I18n > Application
# Configuration). sailfishapp_i18n runs lupdate over this directory at every
# build (translations/rawfish-ir.ts: the English source for translators, and
# every file in TRANSLATIONS), compiles them, and installs the .qm files in
# /usr/share/rawfish-ir/translations, where libsailfishapp loads the one for
# the phone's language. A new language: add translations/rawfish-ir-LANG.ts
# here (docs/TRANSLATING.md).
CONFIG += sailfishapp_i18n
TRANSLATIONS += translations/rawfish-ir-fr.ts

# The page in the system Settings app is loaded by the Settings app, which
# reads the catalogue its entry names ("translation_catalog" in
# settings/rawfish-ir.json) from /usr/share/translations, as
# settings-rawfish-ir-LANG.qm (the platform convention: Reference > I18n >
# Platform Configuration).
SETTINGS_TS = $$files($$PWD/translations/settings-rawfish-ir-*.ts)
settings_qm.input = SETTINGS_TS
settings_qm.output = $$OUT_PWD/${QMAKE_FILE_BASE}.qm
settings_qm.commands = lrelease ${QMAKE_FILE_IN} -qm ${QMAKE_FILE_OUT}
settings_qm.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += settings_qm
settings_tr.path = /usr/share/translations
settings_tr.CONFIG += no_check_exist
for(ts, SETTINGS_TS): settings_tr.files += $$OUT_PWD/$$basename(ts)
settings_tr.files ~= s/\.ts$/.qm/g
INSTALLS += settings_tr

