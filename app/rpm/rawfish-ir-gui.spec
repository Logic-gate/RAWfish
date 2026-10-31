Name:       rawfish-ir-gui
Summary:    RAWfish-indRAW camera (the interface)
Version:    0.1.11
Release:    1
License:    BSD-3-Clause
URL:        https://github.com/Logic-gate/RAWfish
Source0:    %{name}-%{version}.tar.bz2

BuildRequires:  pkgconfig(sailfishapp)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  qt5-qmake
# lrelease: the build compiles the translations
BuildRequires:  qt5-qttools-linguist
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
# The engine (library + controller + CLI) is its own package, rawfish-ir-cli,
# built by scripts/build-engine-rpm.sh at the top of the repository; it can be
# installed alone (a terminal, or another interface). The two packages share
# one version number.
Requires:       rawfish-ir-cli >= 0.1.0

%description
RAWfish-indRAW camera application: the live viewfinder, the deck of controls
and the gallery, backed by the rawfish-ir-cli engine. Shown as RAWfish-ir in
the app grid and in Settings. Installs next to RAWfish (its own names and
settings): both can be used on one phone. Internally the code keeps its
development name, indraw.

%prep
%setup -q -n %{name}-%{version}

%build
# The app builds in its own tree: objects and binary from an earlier build
# for ANOTHER architecture looked "up to date" to make, and an armv7hl
# package shipped the aarch64 binary (October 2026). Two C++ files: a full
# build takes seconds, so always start clean.
rm -f *.o moc_*.cpp moc_*.o moc_predefs.h rawfish-ir Makefile .qmake.stash
%qmake5
%make_build

%install
%qmake5_install
# Sailjail whitelists /usr/share/<ApplicationName> = /usr/share/rawfish-ir (the
# .desktop's ApplicationName, shared with the engine's data directory
# ~/.local/share/com.rawfish/rawfish-ir). The sailfishapp qmake rules install
# under /usr/share/<TARGET>, and TARGET is rawfish-ir too: nothing to move.
# Install the .desktop launcher here (more reliable than a .pro INSTALLS rule,
# which the sailfishapp qmake integration can shadow). %{S:0} is unpacked into
# the build dir, so the file is simply next to the sources.
install -D -m 644 rawfish-ir.desktop %{buildroot}%{_datadir}/applications/rawfish-ir.desktop
# The icon at every size the launcher may use (scripts/make-icon.py).
for n in 86 108 128 172; do
    install -D -m 644 icons/rawfish-ir-$n.png %{buildroot}%{_datadir}/icons/hicolor/${n}x${n}/apps/rawfish-ir.png
done
# Commented reference files; the app copies them next to the real ones at start.
install -D -m 644 examples/rawfish-ir.conf.example %{buildroot}%{_datadir}/rawfish-ir/examples/rawfish-ir.conf.example
install -D -m 644 examples/override-hal.example.jsonc %{buildroot}%{_datadir}/rawfish-ir/examples/override-hal.example.jsonc
# The page in the system Settings app (Settings > Apps > RAWfish-ir).
install -D -m 644 settings/rawfish-ir.json %{buildroot}%{_datadir}/jolla-settings/entries/rawfish-ir.json
install -D -m 644 settings/SettingsPage.qml %{buildroot}%{_datadir}/jolla-settings/pages/rawfish-ir/SettingsPage.qml

%files
%license LICENSE
%{_bindir}/rawfish-ir
%{_datadir}/rawfish-ir
%{_datadir}/applications/rawfish-ir.desktop
%{_datadir}/icons/hicolor/86x86/apps/rawfish-ir.png
%{_datadir}/icons/hicolor/108x108/apps/rawfish-ir.png
%{_datadir}/icons/hicolor/128x128/apps/rawfish-ir.png
%{_datadir}/icons/hicolor/172x172/apps/rawfish-ir.png
%{_datadir}/jolla-settings/entries/rawfish-ir.json
%{_datadir}/jolla-settings/pages/rawfish-ir
%{_datadir}/translations/settings-rawfish-ir-*.qm
