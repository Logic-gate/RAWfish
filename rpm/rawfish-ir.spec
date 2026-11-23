Name:       rawfish-ir
Summary:    RAWfish-ir camera: the app with its engine
# Keep in step with rawfish-ir-version.h (scripts/build-rpm.sh checks).
Version:    0.3.9
# "beta" in the release, so the files say it: rawfish-ir-0.3.9-1.beta.<arch>.rpm.
# The package's name and the version's order are untouched: the first
# version after the beta takes a higher Version and a plain "1" here.
# scripts/with-build-release.sh puts the build number in front.
Release:    1.beta
License:    BSD-3-Clause
URL:        https://github.com/Logic-gate/RAWfish
Source0:    %{name}-%{version}.tar.bz2
# One spec, two packages built from the same tree, so that the app and its
# engine can no longer be of different versions (an app of 481 on an engine
# of 452 lost every photo, November 2026):
#   rawfish-ir           everything: the app, the engine, the data
#   rawfish-ir-cli-only  the engine alone (the command line, or an
#                        interface of your own), without the app
# The two hold the same engine files and exclude each other. Both replace
# the former pair rawfish-ir-cli + rawfish-ir-gui (up to 0.3.9-48x).
# The Android-side library lives in droid-hybris' lib64 on 64-bit ports
# (aarch64) and lib on 32-bit ones (armv7hl); the library itself is built for
# the matching ABI by scripts/build-android.sh.
%ifarch aarch64
%global hybrislib lib64
%else
%global hybrislib lib
%endif

# The engine (sailfish/: the CLI, compiled here)
BuildRequires:  gcc
# libjpeg(-turbo): the CLI compresses the YUV frames itself (the yuv command)
BuildRequires:  libjpeg-turbo-devel
# The app (app/: Qt / QML)
BuildRequires:  pkgconfig(sailfishapp)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  qt5-qmake
# lrelease: the build compiles the translations
BuildRequires:  qt5-qttools-linguist

# The engine's run-time needs, loaded with dlopen so that rpm does not find
# them by itself: libhybris (the bridge to the Android libraries) and
# droidmedia (only to start the binder thread pool the camera service calls
# back through). rpm names a 64-bit library with a "()(64bit)" suffix, a
# 32-bit one without: the right name per architecture (0.47.24: the 32-bit
# package asked for the 64-bit name and could not be installed).
%ifarch aarch64
Requires:       libhybris-common.so.1()(64bit)
%else
Requires:       libhybris-common.so.1
%endif
Requires:       droidmedia
# The app's QML modules, one per import (rpm cannot see QML imports).
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
# QtFeedback: the vibrations (release, TYPE)
Requires:       qml(QtFeedback)
Requires:       libkeepalive
# The former pair, replaced (an install over them removes them), and the
# engine-only package, which holds the same engine files.
Obsoletes:      rawfish-ir-gui < %{version}-%{release}
Obsoletes:      rawfish-ir-cli < %{version}-%{release}
Conflicts:      rawfish-ir-cli-only
# librawfish-ir.so is an Android (bionic) library loaded through libhybris: keep
# rpm's automatic dependency scanner away from it, or the package would
# "need" the Android libc.so / libcamera2ndk.so, which no Sailfish package
# provides. The glibc dependencies of the CLI and the app are still
# generated normally.
%global __requires_exclude_from ^%{_libexecdir}/droid-hybris/.*$
%global __provides_exclude_from ^%{_libexecdir}/droid-hybris/.*$

%description
Beta. RAWfish-ir camera application: the live viewfinder, the deck of controls
and the gallery, with its engine: librawfish-ir.so (Android side, built
beforehand with the NDK by scripts/build-android.sh and shipped prebuilt in
build/android/) and the rawfish-ir-cli program (glibc side: the bridge to the
library, the photographic controller and the command line). Shown as
RAWfish-ir in the app grid and in Settings. Installs next to RAWfish (its own
names and settings): both can be used on one phone. Internally the code keeps
its development name, indraw.

%package -n rawfish-ir-cli-only
Summary:    RAWfish-ir camera engine only, CLI only (no app)
%ifarch aarch64
Requires:       libhybris-common.so.1()(64bit)
%else
Requires:       libhybris-common.so.1
%endif
Requires:       droidmedia
Obsoletes:      rawfish-ir-cli < %{version}-%{release}
Conflicts:      rawfish-ir

%description -n rawfish-ir-cli-only
Beta. The RAWfish-ir engine ALONE, for the command line (or an interface of
your own talking to the controller): librawfish-ir.so, rawfish-ir-cli
(/usr/bin/rawfish-ir-cli), the device profiles and lens-shading maps. No
app: for the camera app, install the package rawfish-ir instead (it holds
this engine too; the two exclude each other). The CLI refuses a library of
another version.

%prep
%setup -q -n %{name}-%{version}
test -f build/android/librawfish-ir.so || { echo "build/android/librawfish-ir.so missing: run scripts/build-android.sh first" >&2; exit 1; }
grep -q "\"%{version}\"" rawfish-ir-version.h || { echo "rawfish-ir-version.h is not %{version}: fix the spec Version" >&2; exit 1; }

%build
make -C sailfish clean all
# The app builds in its own tree: objects and binary from an earlier build
# for ANOTHER architecture looked "up to date" to make, and an armv7hl
# package shipped the aarch64 binary (October 2026). Two C++ files: a full
# build takes seconds, so always start clean.
cd app
rm -f *.o moc_*.cpp moc_*.o moc_predefs.h rawfish-ir Makefile .qmake.stash
# The package's version and release reach the app: at the first start of
# a new one it sets the user's earlier files aside (archiveOnNewVersion).
%qmake5 APP_VERSION=%{version}-%{release}
%make_build
cd ..

%install
# --- the engine (both packages) ---
install -D -m 755 sailfish/rawfish-ir-cli %{buildroot}%{_libexecdir}/rawfish-ir/rawfish-ir-cli
install -d %{buildroot}%{_bindir}
ln -s ../libexec/rawfish-ir/rawfish-ir-cli %{buildroot}%{_bindir}/rawfish-ir-cli
install -D -m 644 build/android/librawfish-ir.so %{buildroot}%{_libexecdir}/droid-hybris/system/%{hybrislib}/librawfish-ir.so
# Measured lens-shading calibrations shipped with the engine (one per phone
# model and camera); a user's own file in device-profiles/lens-shading/ wins.
# In the app's /usr/share/rawfish-ir: the part of /usr/share its sandbox shows.
install -d %{buildroot}%{_datadir}/rawfish-ir/lens-shading
install -m 644 android/data/lens-shading/*.json %{buildroot}%{_datadir}/rawfish-ir/lens-shading/
# Device profiles shipped per camera module (matched by make and model, or by
# the RAW size); the user's override-hal.json replaces any key it repeats.
install -d %{buildroot}%{_datadir}/rawfish-ir/device-profiles
install -m 644 android/data/device-profiles/*.json %{buildroot}%{_datadir}/rawfish-ir/device-profiles/
# --- the app (rawfish-ir only) ---
cd app
%qmake5_install
# Sailjail whitelists /usr/share/<ApplicationName> = /usr/share/rawfish-ir (the
# .desktop's ApplicationName, shared with the engine's data directory
# ~/.local/share/com.rawfish/rawfish-ir). The sailfishapp qmake rules install
# under /usr/share/<TARGET>, and TARGET is rawfish-ir too: nothing to move.
# The .desktop launcher here (more reliable than a .pro INSTALLS rule, which
# the sailfishapp qmake integration can shadow).
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
cd ..

%files
%license LICENSE
%{_libexecdir}/rawfish-ir/rawfish-ir-cli
%{_bindir}/rawfish-ir-cli
%{_libexecdir}/droid-hybris/system/%{hybrislib}/librawfish-ir.so
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

%files -n rawfish-ir-cli-only
%license LICENSE
%{_libexecdir}/rawfish-ir/rawfish-ir-cli
%{_bindir}/rawfish-ir-cli
%{_libexecdir}/droid-hybris/system/%{hybrislib}/librawfish-ir.so
%dir %{_datadir}/rawfish-ir
%{_datadir}/rawfish-ir/lens-shading
%{_datadir}/rawfish-ir/device-profiles
