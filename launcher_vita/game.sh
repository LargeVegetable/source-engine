# The game a waf build is for (configure --build-games) and what its VPK
# takes from it. Sourced by build.sh and package_release.sh with BUILD set
# to the waf output dir. The C side of the same table is game.h

GAME=$(sed -n "s/^GAMES = '\(.*\)'$/\1/p" "$BUILD/c4che/game/server_cache.py" 2>/dev/null)
case "$GAME" in
hl2)
	TITLE_ID=SRCV00001
	TITLE="Half-Life 2"
	NAME=hl2_vita				# hl2_vita.vpk, .elf, -<version>.zip
	ART_DIR=sce_sys				# the release art; sce_sys_dev/ for testing builds
	VERSION_PREFIX=VITA_HL2		# version.h
	GAME_CFLAGS=""
	DOCS="README_VITA.md INSTALL_VITA.md"
	;;
portal)
	TITLE_ID=SRCV00002
	TITLE="Portal"
	NAME=portal_vita
	ART_DIR=sce_sys_portal		# sce_sys_portal_dev/ for testing builds
	VERSION_PREFIX=VITA_PORTAL
	GAME_CFLAGS="-DVITA_GAME_PORTAL"
	DOCS="README_PORTAL_VITA.md INSTALL_PORTAL_VITA.md"
	;;
*)
	echo "$BUILD is a build of ${GAME:-an unknown game}; the Vita launcher takes hl2 or portal (waf configure --build-games)" >&2
	exit 1
	;;
esac
