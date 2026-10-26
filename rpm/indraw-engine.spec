Name:       indraw-engine
Summary:    indRAW camera engine (Camera2 bridge library + CLI)
# Keep in step with indraw_version.h (scripts/build-engine-rpm.sh checks).
Version:    0.47.73
Release:    1
License:    BSD-3-Clause
URL:        https://github.com/Ric9k-TMO-FSO/indraw
Source0:    %{name}-%{version}.tar.bz2
# The Android-side library lives in droid-hybris' lib64 on 64-bit ports
# (aarch64) and lib on 32-bit ones (armv7hl); the library itself is built for
# the matching ABI by scripts/build-android.sh.
%ifarch aarch64
%global hybrislib lib64
%else
%global hybrislib lib
%endif
BuildRequires:  gcc
# Loaded at run time with dlopen, so rpm does not find them by itself:
# libhybris (the bridge to the Android libraries) and droidmedia (only to
# start the binder thread pool the camera service calls back through).
# rpm names a 64-bit library with a "()(64bit)" suffix, a 32-bit one
# without: the right name per architecture (0.47.24: the 32-bit package
# asked for the 64-bit name and could not be installed).
%ifarch aarch64
Requires:       libhybris-common.so.1()(64bit)
%else
Requires:       libhybris-common.so.1
%endif
Requires:       droidmedia
# libindraw.so is an Android (bionic) library loaded through libhybris: keep
# rpm's automatic dependency scanner away from it, or the package would
# "need" the Android libc.so / libcamera2ndk.so, which no Sailfish package
# provides. The CLI's glibc dependencies are still generated normally.
%global __requires_exclude_from ^%{_libexecdir}/droid-hybris/.*$
%global __provides_exclude_from ^%{_libexecdir}/droid-hybris/.*$

%description
The indRAW engine: libindraw.so (Android side, built beforehand with the NDK by
scripts/build-android.sh and shipped prebuilt in build/android/) and the indraw
CLI (glibc side, compiled here), installed where the app expects them:
/usr/libexec/droid-hybris/system/lib64/libindraw.so (lib/ on 32-bit) and /usr/libexec/indraw/indraw
(also /usr/bin/indraw for the command line). The CLI refuses a library of
another version.

%prep
%setup -q -n %{name}-%{version}
test -f build/android/libindraw.so || { echo "build/android/libindraw.so missing: run scripts/build-android.sh first" >&2; exit 1; }
grep -q "\"%{version}\"" indraw_version.h || { echo "indraw_version.h is not %{version}: fix the spec Version" >&2; exit 1; }

%build
make -C sailfish clean all

%install
install -D -m 755 sailfish/indraw %{buildroot}%{_libexecdir}/indraw/indraw
install -d %{buildroot}%{_bindir}
ln -s ../libexec/indraw/indraw %{buildroot}%{_bindir}/indraw
install -D -m 644 build/android/libindraw.so %{buildroot}%{_libexecdir}/droid-hybris/system/%{hybrislib}/libindraw.so
# Measured lens-shading calibrations shipped with the engine (one per phone
# model and camera); a user's own file in device-profiles/lens-shading/ wins.
# In the app's /usr/share/indraw: the part of /usr/share its sandbox shows.
install -d %{buildroot}%{_datadir}/indraw/lens-shading
install -m 644 android/data/lens-shading/*.json %{buildroot}%{_datadir}/indraw/lens-shading/
# Device profiles shipped per camera module (matched by make and model, or by
# the RAW size); the user's override-hal.json replaces any key it repeats.
install -d %{buildroot}%{_datadir}/indraw/device-profiles
install -m 644 android/data/device-profiles/*.json %{buildroot}%{_datadir}/indraw/device-profiles/

%files
%{_libexecdir}/indraw/indraw
%{_bindir}/indraw
%{_libexecdir}/droid-hybris/system/%{hybrislib}/libindraw.so
%{_datadir}/indraw/lens-shading
%{_datadir}/indraw/device-profiles
