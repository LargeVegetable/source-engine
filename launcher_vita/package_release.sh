#!/bin/sh
# Packages a release: the VPK and the documents a download should carry,
# as dist/hl2_vita-<version>.zip.
#
# usage: launcher_vita/package_release.sh [waf output dir, default build-vita]
#
# Build first (python3 ./waf build, then sh launcher_vita/build.sh) without
# HL2_VITA_DEV: a testing build is refused, and so are documents with TODO
# notes or @...@ placeholders (HL2_VITA_PREVIEW=1 packages anyway).

set -e

SRC=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SRC/.." && pwd)
BUILD=$(cd "$ROOT/${1:-build-vita}" && pwd)
VPK="$BUILD/launcher_vita/hl2_vita.vpk"
VERSION=$(sed -n 's/^#define VITA_PORT_VERSION[^"]*"\([^"]*\)".*/\1/p' "$SRC/version.h")

[ -n "$VERSION" ] || { echo "no VITA_PORT_VERSION in version.h" >&2; exit 1; }
[ -f "$VPK" ] || { echo "no $VPK; build it first" >&2; exit 1; }
if [ "$(cat "$VPK.art" 2>/dev/null)" != public ]; then
	echo "$VPK is a testing build (HL2_VITA_DEV); rebuild without it" >&2
	exit 1
fi
if [ "$SRC/version.h" -nt "$VPK" ]; then
	echo "version.h is newer than the VPK; rebuild first" >&2
	exit 1
fi

NAME="hl2_vita-$VERSION"
STAGE="$ROOT/dist/$NAME"
rm -rf "$STAGE" "$ROOT/dist/$NAME.zip"
mkdir -p "$STAGE"
cp "$VPK" "$STAGE/hl2_vita.vpk"
cp "$ROOT/README_VITA.md" "$ROOT/INSTALL_VITA.md" "$ROOT/THIRD_PARTY_VITA.md" "$STAGE/"
# the Source SDK license asks for both with any distribution
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/thirdpartylegalnotices.txt" "$STAGE/"

# nothing from the build machine in the download: refuse if its home folder,
# account name, full name or host name shows up in any packaged file,
# including the files inside the VPK
CHECK="$ROOT/dist/.check"
rm -rf "$CHECK"
mkdir -p "$CHECK"
unzip -q "$STAGE/hl2_vita.vpk" -d "$CHECK"
FOUND=0
for s in "$HOME" "$(id -un)" "$(id -F 2>/dev/null)" "$(hostname -s)"; do
	[ ${#s} -ge 4 ] || continue
	if LC_ALL=C grep -r -a -i -l -F -- "$s" "$STAGE" "$CHECK"; then
		echo "the files above contain \"$s\" from this machine" >&2
		FOUND=1
	fi
done
rm -rf "$CHECK"
[ $FOUND = 0 ] || exit 1

# unfinished documents: TODO notes or @...@ placeholders (@HANDLE@ is
# filled in when the repositories are published). HL2_VITA_PREVIEW=1
# packages anyway
if grep -n -E "TODO|@[A-Z]+@" "$STAGE"/*.md; then
	if [ "${HL2_VITA_PREVIEW:-0}" = 1 ]; then
		echo "preview package: the documents above are unfinished" >&2
	else
		echo "the documents above are unfinished (HL2_VITA_PREVIEW=1 to package a preview)" >&2
		exit 1
	fi
fi

# -X: no owner ids or extended attributes in the zip
(cd "$ROOT/dist" && zip -q -r -X "$NAME.zip" "$NAME" -x '*.DS_Store')
echo "$ROOT/dist/$NAME.zip"
shasum -a 256 "$ROOT/dist/$NAME.zip"
