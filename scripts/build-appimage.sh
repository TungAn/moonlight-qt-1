BUILD_CONFIG="release"

fail()
{
	echo "$1" 1>&2
	exit 1
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

LINUXDEPLOY=linuxdeploy-$(uname -m).AppImage

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
command -v $LINUXDEPLOY >/dev/null 2>&1 || fail "Unable to find '$LINUXDEPLOY' in your PATH!"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $DEPLOY_FOLDER
mkdir $INSTALLER_FOLDER

# Enable LTO for official builds
export CFLAGS=-flto=auto
export CXXFLAGS=-flto=auto
export LDFLAGS=-flto=auto

echo Configuring the project
pushd $BUILD_FOLDER
# Both native Wayland and X11 are built. The libwayland libraries are part of the host graphics
# stack and are never bundled in usr/lib (see the libwayland staging and linuxdeploy exclusions below):
# the host's libEGL_mesa.so and Vulkan WSI drivers need the host's libwayland-client.so, and an older
# bundled copy would shadow it and break EGL even on X11.
#
# libva-wayland is loaded at runtime rather than linked so that the libva probe below only depends
# on libva and libva-x11 (libva-wayland is a separate, often uninstalled package on Debian/Ubuntu).
#
# We disable DRM support because linuxdeploy doesn't bundle the appropriate libraries for Qt EGLFS.
QMAKE_CONFIG=(CONFIG+=disable-libdrm CONFIG+=dlopen-libva-wayland PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE)
LINUXDEPLOY_EXTRA_ARGS=()
if [ -n "${PYROWAVE_PREFIX:-}" ]; then
  PYROWAVE_PREFIX=$(readlink -f "$PYROWAVE_PREFIX")
  [ -f "$PYROWAVE_PREFIX/lib/pkgconfig/pyrowave-shared.pc" ] || fail "Invalid PYROWAVE_PREFIX: pyrowave-shared.pc not found"
  [ -f "$PYROWAVE_PREFIX/lib/libpyrowave-shared.so.1" ] || fail "Invalid PYROWAVE_PREFIX: libpyrowave-shared.so.1 not found"
  [ -f "$PYROWAVE_PREFIX/share/licenses/pyrowave/LICENSE" ] || fail "Invalid PYROWAVE_PREFIX: PyroWave license not found"
  export PKG_CONFIG_PATH="$PYROWAVE_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  # linuxdeploy resolves the executable's DT_NEEDED entries before processing
  # explicit --library arguments, so the private PyroWave prefix must also be
  # visible to its dependency scanner.
  export LD_LIBRARY_PATH="$PYROWAVE_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  QMAKE_CONFIG+=(CONFIG+=enable-pyrowave)
  LINUXDEPLOY_EXTRA_ARGS+=(--library="$PYROWAVE_PREFIX/lib/libpyrowave-shared.so.1")
fi
qmake6 $SOURCE_ROOT/moonlight-qt.pro "${QMAKE_CONFIG[@]}" || fail "Qmake failed!"
popd

echo Compiling Pyrolight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(nproc) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

echo Staging license notices
LICENSE_DIR=$DEPLOY_FOLDER/usr/share/licenses/pyrolight
mkdir -p "$LICENSE_DIR"
cp "$SOURCE_ROOT/LICENSE" "$LICENSE_DIR/Pyrolight-LICENSE"
if [ -n "${PYROWAVE_PREFIX:-}" ]; then
  cp "$PYROWAVE_PREFIX/share/licenses/pyrowave/LICENSE" "$LICENSE_DIR/PyroWave-LICENSE"
fi

export QML_SOURCES_PATHS=$SOURCE_ROOT/app/gui
export QMAKE=qmake6

# Stage the build-environment libva in opt/libva-fallback (outside usr/, so linuxdeploy
# does not scan it, and outside every loader search path), together with a small probe
# binary that carries the same libva ELF requirements as Moonlight. The AppRun below
# uses the probe to decide, via the dynamic loader itself, whether the host libva can
# satisfy Moonlight; when it cannot, the fallback copy is made visible. opt/ is a
# standard linuxdeploy location for application data that must not be processed.
LIBVA_FALLBACK_DIR=$DEPLOY_FOLDER/opt/libva-fallback
SYSTEM_LIBVA=$(ldconfig -p 2>/dev/null | awk '/libva\.so\.2/{print $NF; exit}')
mkdir -p $LIBVA_FALLBACK_DIR
if [ -n "$SYSTEM_LIBVA" ]; then
  cp -a "$(dirname "$SYSTEM_LIBVA")"/libva*.so* $LIBVA_FALLBACK_DIR/ || fail "Unable to stage libva fallback copy!"

  echo Compiling libva-probe
  cc -O2 -L"$(dirname "$SYSTEM_LIBVA")" -Wl,--no-as-needed -o $LIBVA_FALLBACK_DIR/libva-probe \
    $SOURCE_ROOT/app/deploy/linux/libva-probe.c -lva -lva-x11 || fail "Unable to compile libva-probe!"

  # Keep the probe honest: it must reference every VA_API_* version node that the
  # binaries shipped in the AppImage require, otherwise a host libva could pass the
  # probe and still fail to load Moonlight or the bundled FFmpeg.
  va_nodes() { LC_ALL=C readelf -V "$1" 2>/dev/null | grep -oE 'VA_API_[0-9]+\.[0-9]+\.[0-9]+' | sort -u; }
  NEEDED_NODES=$(for b in $DEPLOY_FOLDER/usr/bin/pyrolight \
                          /usr/local/lib*/libav*.so* /usr/local/lib*/libsw*.so* \
                          /usr/lib/x86_64-linux-gnu/libav*.so* /usr/lib/x86_64-linux-gnu/libsw*.so*; do
                   [ -f "$b" ] && va_nodes "$b"; done | sort -u)
  PROBE_NODES=$(va_nodes $LIBVA_FALLBACK_DIR/libva-probe)
  [ -z "$(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES"))" ] || \
    fail "libva-probe is missing version node(s): $(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES")) - update app/deploy/linux/libva-probe.c!"
fi

# libwayland-client is never bundled in usr/lib, but Moonlight links it directly, so a host without
# it (an X11-only system) could not start at all. Stage the build-environment copy in
# opt/wayland-fallback with a probe, like libva above: AppRun exposes it only when the host copy is
# missing or too old for Moonlight. In that case nothing on the host can depend on a newer one.
# libwayland-cursor and libwayland-egl are only needed once Qt or SDL use Wayland; if they are missing,
# Qt's Wayland plugin fails to load and Qt falls back to xcb.
WAYLAND_FALLBACK_DIR=$DEPLOY_FOLDER/opt/wayland-fallback
SYSTEM_WAYLAND_CLIENT=$(ldconfig -p 2>/dev/null | awk '/libwayland-client\.so\.0/{print $NF; exit}')
[ -n "$SYSTEM_WAYLAND_CLIENT" ] || fail "Unable to find libwayland-client.so.0!"
mkdir -p $WAYLAND_FALLBACK_DIR
cp -L "$SYSTEM_WAYLAND_CLIENT" $WAYLAND_FALLBACK_DIR/libwayland-client.so.0 || fail "Unable to stage libwayland-client fallback copy!"

echo Compiling wayland-probe
cc -O2 -Wl,-z,now -Wl,--no-as-needed -o $WAYLAND_FALLBACK_DIR/wayland-probe \
  $SOURCE_ROOT/app/deploy/linux/wayland-probe.c -lwayland-client || fail "Unable to compile wayland-probe!"

# Keep the probe honest: it must reference every libwayland-client symbol that Moonlight and the
# staged libva-wayland import (libwayland has no version nodes, so compare symbol names).
wl_imports() { nm -D --undefined-only "$1" 2>/dev/null | awk '$2 ~ /^wl_/ {print $2}' | sort -u; }
NEEDED_WL=$(for b in $DEPLOY_FOLDER/usr/bin/pyrolight $LIBVA_FALLBACK_DIR/libva-wayland.so.2; do
              [ -f "$b" ] && wl_imports "$b"; done | sort -u)
PROBE_WL=$(wl_imports $WAYLAND_FALLBACK_DIR/wayland-probe)
[ -n "$NEEDED_WL" ] || fail "Moonlight does not import libwayland-client - was Wayland support compiled?"
[ -z "$(comm -13 <(echo "$PROBE_WL") <(echo "$NEEDED_WL"))" ] || \
  fail "wayland-probe is missing symbol(s): $(comm -13 <(echo "$PROBE_WL") <(echo "$NEEDED_WL")) - update app/deploy/linux/wayland-probe.c!"

APP_RUN=$BUILD_ROOT/AppRun-libva
cat > $APP_RUN <<'APPRUN_EOF'
#!/bin/bash
# AppRun: prefer the host libva; use the staged copy only if the host cannot run us.
#
# VA-API driver modules on the host are loaded by libva and export an entrypoint
# named after the libva version they were built against (__vaDriverInit_1_XX).
# A libva can only load drivers that are not newer than itself, so bundling our
# own (older) libva silently breaks hardware decoding on up-to-date distros.
# Distros keep host libva and host drivers in step, so whenever the host libva
# can satisfy Moonlight's own ELF version requirements it is the right choice.
#
# "Can satisfy" is answered here by libva-probe, a tiny program linked against
# the same versioned libva symbols as Moonlight and the bundled FFmpeg: if the
# dynamic loader can start it, the host libva works; if not (no libva installed,
# or one too old to link), we make the staged build-environment copy visible
# via LD_LIBRARY_PATH, matching the pre-existing bundled behavior.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
WAYLAND_FALLBACK="$APPDIR/opt/wayland-fallback"
LIBVA_FALLBACK="$APPDIR/opt/libva-fallback"

# libwayland-client follows the same rule and is checked first, because
# libva-wayland (and thus libva-probe) needs it. The staged copy is used only
# when the host has no usable libwayland-client (for example X11-only systems).
if [ -d "$WAYLAND_FALLBACK" ] && ! "$WAYLAND_FALLBACK/wayland-probe" 2>/dev/null; then
    export LD_LIBRARY_PATH="$WAYLAND_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-probe" 2>/dev/null; then
    export LD_LIBRARY_PATH="$LIBVA_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export VAAPI_USE_FALLBACK_PATHS=1
fi

exec "$APPDIR/usr/bin/pyrolight" "$@"
APPRUN_EOF
chmod +x $APP_RUN

# Qt's native Wayland platform plugins; xcb (deployed by linuxdeploy-plugin-qt) remains the fallback.
# They are staged here and their dependencies resolved by linuxdeploy itself, because the pinned
# linuxdeploy-plugin-qt neither deploys the shell/decoration plugins Qt 6.2 needs nor honors
# --exclude-library (it would bundle libwayland-cursor and libwayland-egl). "wayland" selects
# libqwayland-generic, which uses the wayland-egl client buffer integration for Qt Quick; xdg-shell
# is the shell every desktop compositor supports and bradient draws decorations where the
# compositor does not (GNOME).
QT_PLUGINS_DIR=$(qmake6 -query QT_INSTALL_PLUGINS)
for plugin in platforms/libqwayland-generic.so platforms/libqwayland-egl.so \
              wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
              wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so; do
  mkdir -p $DEPLOY_FOLDER/usr/plugins/$(dirname $plugin)
  cp $QT_PLUGINS_DIR/$plugin $DEPLOY_FOLDER/usr/plugins/$plugin || fail "Unable to find Qt plugin $plugin!"
  LINUXDEPLOY_EXTRA_ARGS+=(--deploy-deps-only=$DEPLOY_FOLDER/usr/plugins/$plugin)
done

echo Creating AppImage
pushd $INSTALLER_FOLDER
# Don't bundle libva: the bundled build-environment version (jammy: VA-API 1.20/1.22)
# cannot dlopen GPU drivers compiled against newer libva on the host (which export
# __vaDriverInit_1_23+), so va_openDriver() always fails and VAAPI falls back to
# software decoding. The host always provides libva on systems where VA-API is
# usable, so link against it at runtime instead (the AppRun shim above keeps a
# bundled last-resort copy for hosts without libva).
#
# Don't bundle libwayland-*: like EGL and the Vulkan ICDs, they belong to the host graphics stack
# (see opt/wayland-fallback above). SDL3 dlopen()s them and libdecor from the host.
#
# Don't bundle libvulkan: the loader must match the host's drivers and implicit layers. The
# build-environment loader has no Wayland WSI, so under Gamescope (SteamOS Game Mode) its WSI
# layer, which turns the X11 surface into a Wayland one, called a null vkCreateWaylandSurfaceKHR
# and Pyrolight crashed at stream start.
VERSION=$VERSION OUTPUT="Pyrolight-$VERSION-$(uname -m).AppImage" $LINUXDEPLOY --appdir $DEPLOY_FOLDER \
  --library=/usr/local/lib/libSDL3.so.0 \
  "${LINUXDEPLOY_EXTRA_ARGS[@]}" \
  --plugin qt \
  --custom-apprun $APP_RUN \
  --exclude-library=libva.so* \
  --exclude-library=libva-drm.so* \
  --exclude-library=libva-wayland.so* \
  --exclude-library=libva-x11.so* \
  --exclude-library=libwayland-client.so* \
  --exclude-library=libwayland-cursor.so* \
  --exclude-library=libwayland-egl.so* \
  --exclude-library=libwayland-server.so* \
  --exclude-library=libvulkan.so* \
  --output appimage || fail "linuxdeploy failed!"
popd

$SOURCE_ROOT/scripts/check-appimage-display-backends.sh $DEPLOY_FOLDER || fail "AppDir display backend check failed!"
$SOURCE_ROOT/scripts/check-linux-icons.sh $DEPLOY_FOLDER || fail "AppDir icon check failed!"

echo Build successful
