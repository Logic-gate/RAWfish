#!/bin/sh
# Updates the translation files from the sources, after a text shown on
# screen was added or changed (docs/TRANSLATING.md). The build never does
# it: it only compiles the .ts files.
#   app/translations/rawfish-ir.ts           English source of the app
#   app/translations/rawfish-ir-LANG.ts      the app, one per language
#   app/translations/settings-rawfish-ir-LANG.ts   the Settings page
# New texts appear as unfinished in each language file; texts gone from
# the code are removed (-no-obsolete). Needs Qt's lupdate (qttools5-dev-tools
# on a PC; LUPDATE=... to point at another).
set -eu
cd "$(dirname -- "$0")/../app"
lupdate=${LUPDATE:-$(command -v lupdate || echo /usr/lib/qt5/bin/lupdate)}
app_sources="qml/*.qml indrawpreview.cpp main.cpp"
# shellcheck disable=SC2086
"$lupdate" -no-obsolete -locations relative $app_sources -ts translations/rawfish-ir.ts translations/rawfish-ir-*.ts
"$lupdate" -no-obsolete -locations relative settings/SettingsPage.qml -ts translations/settings-rawfish-ir-*.ts
echo "updated; translate the unfinished entries (Qt Linguist or a text editor), then commit"
