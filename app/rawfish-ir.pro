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

# Translations. The build only COMPILES the .ts files (lrelease) and
# installs the .qm: the app's in /usr/share/rawfish-ir/translations, where
# libsailfishapp loads the one for the phone's language (docs.sailfishos.org,
# Reference > I18n > Application Configuration). The .ts files are updated
# on purpose, by scripts/update-translations.sh, from every source (QML, C++,
# the Settings page): sailfishapp_i18n ran lupdate at every build over
# app/qml and app/src only, dropped the engine's messages and rewrote the
# files in its own format (0.1.8).
APP_TS = $$files($$PWD/translations/rawfish-ir-*.ts)
app_qm.input = APP_TS
app_qm.output = $$OUT_PWD/${QMAKE_FILE_BASE}.qm
app_qm.commands = lrelease ${QMAKE_FILE_IN} -qm ${QMAKE_FILE_OUT}
app_qm.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += app_qm
app_tr.path = /usr/share/rawfish-ir/translations
app_tr.CONFIG += no_check_exist
for(ts, APP_TS): app_tr.files += $$OUT_PWD/$$basename(ts)
app_tr.files ~= s/\.ts$/.qm/g
INSTALLS += app_tr

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

