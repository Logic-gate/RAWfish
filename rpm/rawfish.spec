# SPDX-FileCopyrightText: 2013 - 2024 Jolla Ltd.
# SPDX-FileCopyrightText: 2019 - 2020 Open Mobile Platform LLC.
# SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

Name:       rawfish
Summary:    RAWfish application
Version:    1.3.1
Release:    2
License:    BSD-3-Clause
URL:        https://github.com/Logic-gate/RAWfish
Source0:    %{name}-%{version}.tar.bz2

# libsfoscamera2.so is an Android/Bionic library loaded through libhybris from
# /usr/libexec/droid-hybris. Its NDK dependencies are provided by the Android
# compatibility image, not by Sailfish RPM packages.
%global __requires_exclude_from ^.*/usr/libexec/droid-hybris/system/lib64/libsfoscamera2\\.so$
%global __provides_exclude_from ^.*/usr/libexec/droid-hybris/system/lib64/libsfoscamera2\\.so$
%global __requires_exclude ^(libandroid\\.so.*|libcamera2ndk\\.so.*|libmediandk\\.so.*|libnativewindow\\.so.*|liblog\\.so.*|libdl_android\\.so.*)$

BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(Qt5Test)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(qdeclarative5-boostable)
BuildRequires:  pkgconfig(libtiff-4)
BuildRequires:  pkgconfig(mlite5) >= 0.2.5
BuildRequires:  pkgconfig(systemsettings) >= 0.2.13
BuildRequires:  qt5-qttools
BuildRequires:  qt5-qttools-linguist
BuildRequires:  oneshot

Requires:  sailfishsilica-qt5 >= 1.1.79
Requires:  qt5-qtdeclarative-import-models2
Requires:  qt5-qtdeclarative-import-positioning
Requires:  qt5-qtdeclarative-import-multimedia
Requires:  qt5-qtdeclarative-import-sensors
Requires:  qt5-qtmultimedia-plugin-mediaservice-gstcamerabin >= 5.6.2+git25
Requires:  qt5-qtmultimedia-plugin-mediaservice-gstmediaplayer
Requires:  declarative-transferengine-qt5 >= 0.0.49
Requires:  nemo-qml-plugin-thumbnailer-qt5
Requires:  nemo-qml-plugin-dbus-qt5
Requires:  nemo-qml-plugin-policy-qt5
Requires:  nemo-qml-plugin-time-qt5
Requires:  nemo-qml-plugin-configuration-qt5
Requires:  nemo-qml-plugin-notifications-qt5 >= 1.1.2
Requires:  nemo-qml-plugin-systemsettings >= 0.5.21
Requires:  libkeepalive >= 1.7.0
Requires:  sailfish-components-media-qt5 >= 0.0.18
Requires:  sailfish-components-gallery-qt5 >= 1.1.10
Requires:  sailfish-policy >= 0.2.59
Requires:  jolla-settings-system >= 1.0.70
Requires:  libngf-qt5-declarative
Requires:  qr-filter-qml-plugin
Requires:  sailfish-content-graphics >= 1.2.2
Requires:  gstreamer1.0-plugins-good
Requires:  gstreamer1.0-plugins-bad
Requires:  dconf
Requires:  sailjail-launch-approval
Requires:  mapplauncherd-booster-silica-qt5-media

%{_oneshot_requires_post}

%description
The RAWfish application.

%prep
%setup -q -n %{name}-%{version}

%build

%qmake5 rawfish.pro
%make_build

%install
%qmake5_install

%post
%{_bindir}/add-oneshot dconf-update || :

%files
%license LICENSES/BSD-3-Clause.txt
%{_datadir}/applications/rawfish.desktop
%{_datadir}/icons/hicolor/86x86/apps/rawfish-sfos.png
%dir %{_datadir}/rawfish
%{_datadir}/rawfish/camera.qml
%{_datadir}/rawfish/pages
%{_datadir}/rawfish/cover
%{_bindir}/rawfish
%{_datadir}/dbus-1/services/com.rawfish.camera.service
%dir %{_datadir}/jolla-settings
%dir %{_datadir}/jolla-settings/pages
%dir %{_datadir}/jolla-settings/pages/rawfish
%{_datadir}/jolla-settings/pages/rawfish/SettingsPage.qml
%dir %{_datadir}/jolla-settings/entries
%{_datadir}/jolla-settings/entries/rawfish.json
%{_libdir}/qt5/qml/com/vivid/camera
%dir %{_libexecdir}/rawfish
%{_libexecdir}/rawfish/sfos-camera2-probe
%dir %{_libexecdir}/droid-hybris
%dir %{_libexecdir}/droid-hybris/system
%dir %{_libexecdir}/droid-hybris/system/lib64
%{_libexecdir}/droid-hybris/system/lib64/libsfoscamera2.so
%{_sysconfdir}/dconf/db/vendor.d/00-rawfish.txt
