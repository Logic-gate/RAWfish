Name:       rawfish-ir-cli
Summary:    RAWfish-indRAW camera engine (Camera2 bridge library, controller, CLI)
# Keep in step with indraw_version.h (scripts/build-engine-rpm.sh checks).
Version:    0.1.10
Release:    1
License:    BSD-3-Clause
URL:        https://github.com/Logic-gate/RAWfish
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
# librawfish-ir.so is an Android (bionic) library loaded through libhybris: keep
# rpm's automatic dependency scanner away from it, or the package would
# "need" the Android libc.so / libcamera2ndk.so, which no Sailfish package
# provides. The CLI's glibc dependencies are still generated normally.
%global __requires_exclude_from ^%{_libexecdir}/droid-hybris/.*$
%global __provides_exclude_from ^%{_libexecdir}/droid-hybris/.*$

%description
The RAWfish-indRAW engine: librawfish-ir.so (Android side, built beforehand
with the NDK by scripts/build-android.sh and shipped prebuilt in
build/android/) and the rawfish-ir-cli program (glibc side, compiled here:
the bridge to the library, the photographic controller and the command
line), installed where the app expects them:
/usr/libexec/droid-hybris/system/lib64/librawfish-ir.so (lib/ on 32-bit) and
/usr/libexec/rawfish-ir/rawfish-ir-cli (also /usr/bin/rawfish-ir-cli for the
command line). Usable on its own, without the gui package: a terminal, or an
interface of your own talking to the controller. The CLI refuses a library of
another version. Internally the engine keeps its development name, indraw.

%prep
%setup -q -n %{name}-%{version}
test -f build/android/librawfish-ir.so || { echo "build/android/librawfish-ir.so missing: run scripts/build-android.sh first" >&2; exit 1; }
grep -q "\"%{version}\"" indraw_version.h || { echo "indraw_version.h is not %{version}: fix the spec Version" >&2; exit 1; }

%build
make -C sailfish clean all

%install
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

%files
%license LICENSE
%{_libexecdir}/rawfish-ir/rawfish-ir-cli
%{_bindir}/rawfish-ir-cli
%{_libexecdir}/droid-hybris/system/%{hybrislib}/librawfish-ir.so
%{_datadir}/rawfish-ir/lens-shading
%{_datadir}/rawfish-ir/device-profiles
