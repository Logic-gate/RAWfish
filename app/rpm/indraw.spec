Name:       indraw-app
Summary:    indRAW camera (viewfinder)
Version:    0.20.55
Release:    1
License:    BSD-3-Clause
URL:        https://github.com/Ric9k-TMO-FSO/indraw
Source0:    %{name}-%{version}.tar.bz2

BuildRequires:  pkgconfig(sailfishapp)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  qt5-qmake
# QML modules used at runtime, one per import (rpm cannot see QML imports).
# (No comment after a tag on the same line: rpm would read it as more names.)
# Sailfish.Silica (+ .private), Sailfish.Gallery
Requires:       sailfishsilica-qt5
Requires:       sailfish-components-gallery-qt5
# QtQuick, QtQuick.Window (Screen.pixelDensity), Qt.labs.folderlistmodel
Requires:       qt5-qtdeclarative-import-qtquick2plugin
Requires:       qt5-qtdeclarative-import-window2
Requires:       qt5-qtdeclarative-import-folderlistmodel
# QtSensors (the level)
Requires:       qt5-qtdeclarative-import-sensors
# Nemo.Policy (volume keys), Nemo.Configuration (Settings page),
# Nemo.KeepAlive (screen on during a stack)
Requires:       nemo-qml-plugin-policy-qt5
Requires:       nemo-qml-plugin-configuration-qt5
Requires:       libkeepalive
# The engine (library + CLI) is its own package, built by
# scripts/build-engine-rpm.sh at the top of the repository.
Requires:       indraw-engine >= 0.47.71

%description
indRAW camera application: the live viewfinder backed by the indraw engine.

%prep
%setup -q -n %{name}-%{version}

%build
# The app builds in its own tree: objects and binary from an earlier build
# for ANOTHER architecture looked "up to date" to make, and an armv7hl
# package shipped the aarch64 binary (October 2026). Two C++ files: a full
# build takes seconds, so always start clean.
rm -f *.o moc_*.cpp moc_*.o moc_predefs.h indraw-app Makefile .qmake.stash
%qmake5
%make_build

%install
%qmake5_install
# Sailjail whitelists /usr/share/<ApplicationName> = /usr/share/indraw (the
# .desktop's ApplicationName, shared with the engine's data directory
# ~/.local/share/com.indraw/indraw). The sailfishapp qmake rules install under
# /usr/share/<TARGET> = indraw-app, invisible from inside the sandbox (the QML
# was "File not found" when launched from the app grid): move it.
mv %{buildroot}%{_datadir}/indraw-app %{buildroot}%{_datadir}/indraw
# Install the .desktop launcher here (more reliable than a .pro INSTALLS rule,
# which the sailfishapp qmake integration can shadow). %{S:0} is unpacked into
# the build dir, so the file is simply next to the sources.
install -D -m 644 indraw.desktop %{buildroot}%{_datadir}/applications/indraw.desktop
# The icon at every size the launcher may use (scripts/make-icon.py).
for n in 86 108 128 172; do
    install -D -m 644 icons/indraw-$n.png %{buildroot}%{_datadir}/icons/hicolor/${n}x${n}/apps/indraw.png
done
# Commented reference files; the app copies them next to the real ones at start.
install -D -m 644 examples/indraw.conf.example %{buildroot}%{_datadir}/indraw/examples/indraw.conf.example
install -D -m 644 examples/override-hal.example.jsonc %{buildroot}%{_datadir}/indraw/examples/override-hal.example.jsonc
# The page in the system Settings app (Settings > Apps > indRAW).
install -D -m 644 settings/indraw.json %{buildroot}%{_datadir}/jolla-settings/entries/indraw.json
install -D -m 644 settings/SettingsPage.qml %{buildroot}%{_datadir}/jolla-settings/pages/indraw/SettingsPage.qml

%files
%{_bindir}/indraw-app
%{_datadir}/indraw
%{_datadir}/applications/indraw.desktop
%{_datadir}/icons/hicolor/86x86/apps/indraw.png
%{_datadir}/icons/hicolor/108x108/apps/indraw.png
%{_datadir}/icons/hicolor/128x128/apps/indraw.png
%{_datadir}/icons/hicolor/172x172/apps/indraw.png
%{_datadir}/jolla-settings/entries/indraw.json
%{_datadir}/jolla-settings/pages/indraw
