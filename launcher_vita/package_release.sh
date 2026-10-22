#!/bin/sh
# Packages a release: the VPK and the documents a download should carry,
# as dist/hl2_vita-<version>.zip (portal_vita-<version>.zip for a Portal
# build; the game comes from the build, see game.sh).
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
. "$SRC/game.sh"
VPK="$BUILD/launcher_vita/$NAME.vpk"
VERSION=$(sed -n "s/^#define ${VERSION_PREFIX}_VERSION[^\"]*\"\([^\"]*\)\".*/\1/p" "$SRC/version.h")

[ -n "$VERSION" ] || { echo "no ${VERSION_PREFIX}_VERSION in version.h" >&2; exit 1; }
[ -f "$VPK" ] || { echo "no $VPK; build it first" >&2; exit 1; }
if [ "$(cat "$VPK.art" 2>/dev/null)" != public ]; then
	echo "$VPK is a testing build (HL2_VITA_DEV); rebuild without it" >&2
	exit 1
fi
if [ "$SRC/version.h" -nt "$VPK" ]; then
	echo "version.h is newer than the VPK; rebuild first" >&2
	exit 1
fi

ZIP="$NAME-$VERSION"
STAGE="$ROOT/dist/$ZIP"
rm -rf "$STAGE" "$ROOT/dist/$ZIP.zip"
mkdir -p "$STAGE"
cp "$VPK" "$STAGE/$NAME.vpk"
for doc in $DOCS THIRD_PARTY_VITA.md; do
	[ -f "$ROOT/$doc" ] || { echo "no $doc to package" >&2; exit 1; }
	cp "$ROOT/$doc" "$STAGE/"
done
# the Source SDK license asks for both with any distribution
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/thirdpartylegalnotices.txt" "$STAGE/"

# nothing from the build machine in the download: refuse if its home folder,
# account name, full name or host name shows up in any packaged file,
# including the files inside the VPK
CHECK="$ROOT/dist/.check"
rm -rf "$CHECK"
mkdir -p "$CHECK"
unzip -q "$STAGE/$NAME.vpk" -d "$CHECK"
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
(cd "$ROOT/dist" && zip -q -r -X "$ZIP.zip" "$ZIP" -x '*.DS_Store')
echo "$ROOT/dist/$ZIP.zip"
shasum -a 256 "$ROOT/dist/$ZIP.zip"
