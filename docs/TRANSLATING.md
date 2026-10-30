# Translating RAWfish-ir

Every text the app shows can be translated. The app follows the phone's
language; a language without a translation shows English.

## The files

- `app/translations/rawfish-ir.ts`: the English source of every text of the
  app (camera page, shooting menu, gallery, lens-shading assistant, engine
  messages). Regenerated at every build: do not translate it.
- `app/translations/rawfish-ir-LANG.ts`: one per language (`fr` today).
- `app/translations/settings-rawfish-ir-LANG.ts`: the page in the system
  Settings app (Settings > Apps > RAWfish-ir). It is loaded by the Settings
  app, not by RAWfish-ir, so it has its own catalogue
  (`/usr/share/translations/settings-rawfish-ir-LANG.qm`, named by
  `"translation_catalog"` in `app/settings/rawfish-ir.json`).
- `app/rawfish-ir.desktop`: the comment under the icon, `Comment[LANG]=`.

These follow docs.sailfishos.org, Reference > I18n (Application
Configuration for the app, Platform Configuration for the Settings page).

## Adding a language

1. Copy `app/translations/rawfish-ir-fr.ts` to `rawfish-ir-LANG.ts`, and
   `settings-rawfish-ir-fr.ts` to `settings-rawfish-ir-LANG.ts` (LANG as the
   phone names it: `de`, `fi`, `pt_BR`...), and set `language="LANG"` in
   each file's `<TS>` line.
2. Translate every `<translation>` (Qt Linguist opens these files, or any
   text editor). Notes for translators (`//:` in the code) appear next to
   the texts: the short forms under the menu's round buttons and the units on
   the shutter wheel must stay short.
3. In `app/rawfish-ir.pro`, add `translations/rawfish-ir-LANG.ts` to
   `TRANSLATIONS` (the Settings catalogue is picked up by its file name).
4. Add `Comment[LANG]=` to `app/rawfish-ir.desktop`.
5. Build: the .qm files are made and installed by the package.

## For developers

Texts shown on screen are wrapped in `qsTr()` in QML and `tr()` in C++
(allowed for applications; Jolla's own platform components use qsTrId).
Values sent to the engine (mode names, sizes, numbers) stay as they are:
only what is displayed is translated. The engine's replies and the log stay
in English.
