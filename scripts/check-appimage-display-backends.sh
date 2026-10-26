#!/bin/bash
# Check the display backend packaging of a Pyrolight AppDir (the build's
# deploy folder or an extracted AppImage's squashfs-root): native Wayland and
# X11 are both present, and host graphics-stack libraries are not bundled.
# See scripts/build-appimage.sh for the packaging boundary.

set -uo pipefail

APPDIR=${1:?usage: $0 <AppDir>}
failures=0

pass() { echo "ok:   $1"; }
fail() { echo "FAIL: $1" >&2; failures=$((failures + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

needed() { LC_ALL=C readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/ {print $2}'; }
has_needed() { needed "$1" | grep -x "$2" >/dev/null; }

BIN=$APPDIR/usr/bin/pyrolight
check "Pyrolight binary exists" '[ -x "$BIN" ]'
for lib in libwayland-client.so.0 libX11.so.6 libva-x11.so.2; do
    check "Pyrolight links $lib (Wayland and X11 support compiled)" 'has_needed "$BIN" $lib'
done
check "Pyrolight loads libva-wayland at runtime instead of linking it" \
    '! has_needed "$BIN" libva-wayland.so.2 && strings "$BIN" | grep -x libva-wayland.so.2 >/dev/null'

PLUGINS=$APPDIR/usr/plugins
for plugin in platforms/libqxcb.so platforms/libqwayland-generic.so platforms/libqwayland-egl.so \
              xcbglintegrations/libqxcb-egl-integration.so \
              wayland-shell-integration/libxdg-shell.so \
              wayland-decoration-client/libbradient.so \
              wayland-graphics-integration-client/libqt-plugin-wayland-egl.so; do
    check "Qt plugin $plugin is deployed" '[ -f "$PLUGINS/$plugin" ]'
done

SDL3=$APPDIR/usr/lib/libSDL3.so.0
for soname in libwayland-client.so.0 libwayland-egl.so.1 libwayland-cursor.so.0 libdecor-0.so.0 libxkbcommon.so.0 libX11.so.6; do
    check "SDL3 loads $soname (Wayland/X11 video drivers built)" 'strings "$SDL3" | grep -x "$soname" >/dev/null'
done

# Host graphics stack: never bundled where the loader would find it first.
bundled=$(find "$APPDIR/usr" \( -name 'libwayland-*.so*' -o -name 'libva*.so*' -o -name 'libEGL*.so*' \
                                -o -name 'libGL*.so*' -o -name 'libgbm.so*' -o -name 'libdrm.so*' -o -name 'libdecor-0.so*' \
                                -o -name 'libvulkan.so*' \) -print)
check "No host graphics-stack libraries are bundled in usr/${bundled:+ (found: $bundled)}" '[ -z "$bundled" ]'

check "libwayland-client fallback is staged" '[ -f "$APPDIR/opt/wayland-fallback/libwayland-client.so.0" ] && [ -x "$APPDIR/opt/wayland-fallback/wayland-probe" ]'
check "libva fallback includes libva-wayland" '[ -e "$APPDIR/opt/libva-fallback/libva-wayland.so.2" ]'
probe_line() { grep -n -m1 "^if .*$1" "$APPDIR/AppRun.wrapped" | cut -d: -f1; }
check "AppRun checks libwayland before libva" \
    '[ -n "$(probe_line wayland-probe)" ] && [ -n "$(probe_line libva-probe)" ] && [ "$(probe_line wayland-probe)" -lt "$(probe_line libva-probe)" ]'

# Every bundled ELF must resolve its dependencies from the AppDir or the
# build host (which stands in for the host graphics stack).
unresolved=$(find "$APPDIR/usr" -type f \( -name '*.so*' -o -path '*/bin/*' \) -print0 |
    while IFS= read -r -d '' elf; do
        file -b "$elf" | grep -q '^ELF' || continue
        LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$elf" 2>/dev/null | grep 'not found' | sed "s|^|${elf#$APPDIR/}: |"
    done)
check "All bundled ELF dependencies resolve${unresolved:+:
$unresolved}" '[ -z "$unresolved" ]'

if [ $failures -ne 0 ]; then
    echo "$failures display backend packaging check(s) failed" >&2
    exit 1
fi
echo "Display backend packaging checks passed"
