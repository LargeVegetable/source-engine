#!/bin/sh
# Builds the Vita launcher against the engine modules from a waf build, then
# packs everything into a VPK.
#
# usage: launcher_vita/build.sh [waf output dir, default build-vita]
#
# The game (hl2 or portal) is the one the waf build was configured for; see
# game.sh.
#
# The launcher has to be linked after the modules: its export table is
# generated from what they import.

set -e

: "${VITASDK:?set VITASDK to the VitaSDK install}"
export PATH="$VITASDK/bin:$PATH"

SRC=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SRC/.." && pwd)
BUILD=$(cd "$ROOT/${1:-build-vita}" && pwd)
OUT="$BUILD/launcher_vita"

# vitaGL built from source with error logging (VitaSDK's package is built
# with NO_DEBUG=1, which compiles error handling out); falls back to the
# SDK's copy when not present
VITAGL_DIR=${VITAGL_DIR:-$ROOT/../vitaGL}
VITAGL_LDFLAGS=""
if [ -f "$VITAGL_DIR/libvitaGL.a" ]; then
	VITAGL_LDFLAGS="-L$VITAGL_DIR"
	echo "using vitaGL from $VITAGL_DIR"
fi

# the game: title ID, names, art (game.sh)
. "$SRC/game.sh"
echo "game: $GAME ($TITLE, $TITLE_ID)"
# version.h: VITA_<GAME>_APP_VER is param.sfo's XX.YY version
APP_VER=$(sed -n "s/^#define ${VERSION_PREFIX}_APP_VER[^\"]*\"\([^\"]*\)\".*/\1/p" "$SRC/version.h")
[ -n "$APP_VER" ] || { echo "no ${VERSION_PREFIX}_APP_VER in version.h" >&2; exit 1; }

# HL2_VITA_DEV=1: a testing build (of either game). It logs in detail
# without -vitadebug (VITA_DEV_BUILD in main.cpp) and takes its LiveArea art
# from sce_sys_dev/ (sce_sys_portal_dev/ for Portal), a local, untracked
# folder for trying other art, when present. Otherwise it's sce_sys/
# (sce_sys_portal/), the release art (in git). package_release.sh refuses
# testing builds
DEV_CFLAGS=""
ART=$SRC/$ART_DIR
ART_KIND=public
if [ "${HL2_VITA_DEV:-0}" = 1 ]; then
	DEV_CFLAGS="-DVITA_DEV_BUILD"
	ART_KIND=dev
	[ -d "$SRC/${ART_DIR}_dev" ] && ART=$SRC/${ART_DIR}_dev
fi
echo "build: $ART_KIND (art $ART)"

mkdir -p "$OUT/bin"

MODULES=$(find "$BUILD" -name '*.so' ! -path "$OUT/*" | sort)
if [ -z "$MODULES" ]; then
	echo "no modules in $BUILD; run waf build first" >&2
	exit 1
fi

python3 "$SRC/gen_exports.py" arm-vita-eabi-nm "$OUT/exports.S" $MODULES
python3 "$SRC/check_imports.py" arm-vita-eabi-nm arm-vita-eabi-readelf "$OUT/exports.S" $MODULES

CFLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fno-short-enums -O2 -Wall -I$ROOT/common/vita"

arm-vita-eabi-g++ $CFLAGS $GAME_CFLAGS $DEV_CFLAGS -std=gnu++11 -c "$SRC/main.cpp" -o "$OUT/main.o"
arm-vita-eabi-g++ $CFLAGS $GAME_CFLAGS -std=gnu++11 -c "$SRC/so_loader.cpp" -o "$OUT/so_loader.o"
arm-vita-eabi-gcc $CFLAGS -c "$SRC/vita_stubs.c" -o "$OUT/vita_stubs.o"
arm-vita-eabi-gcc $CFLAGS -c "$SRC/vita_iconv.c" -o "$OUT/vita_iconv.o"
arm-vita-eabi-gcc $CFLAGS -c "$OUT/exports.S" -o "$OUT/exports.o"

# everything the modules import comes from here; see gen_exports.py
# --wrap: SDL's vitaGL init goes through the launcher, see main.cpp;
# sem_destroy: pthread-embedded's sleeps 1 ms every time; malloc & co: large
# blocks get their own arena, failures are logged; see so_loader.cpp
link_launcher() {
	arm-vita-eabi-g++ $CFLAGS "$@" -Wl,-q -Wl,--no-enum-size-warning -Wl,--wrap=vglInitExtended -Wl,--wrap=sceClibPrintf -Wl,--wrap=vglSwapBuffers \
		-Wl,--wrap=sem_destroy -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign \
		-Wl,--wrap=free -Wl,--wrap=malloc_usable_size -Wl,--wrap=mallinfo -Wl,--wrap=setvbuf \
		"$OUT/main.o" "$OUT/so_loader.o" "$OUT/vita_stubs.o" "$OUT/vita_iconv.o" "$OUT/exports.o" \
		-o "$OUT/$NAME.elf" $VITAGL_LDFLAGS \
		-Wl,--start-group \
		-lSDL2 -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon \
		-lfreetype -lpng -ljpeg -lz -lbz2 -lopus \
		-lstdc++ -lm -lpthread -lc \
		-lkubridge_stub_weak -lSceVshBridge_stub -ltaihen_stub -lSceShaccCg_stub \
		-lSceAppMgr_stub -lSceAppUtil_stub -lSceAudio_stub -lSceAudioIn_stub \
		-lSceCommonDialog_stub -lSceCtrl_stub -lSceDisplay_stub -lSceGxm_stub \
		-lSceHid_stub -lSceIme_stub -lSceKernelDmacMgr_stub -lSceKernelThreadMgr_stub \
		-lSceLibKernel_stub -lSceMotion_stub -lSceNet_stub -lSceNetCtl_stub \
		-lScePower_stub -lSceRtc_stub -lSceSysmodule_stub -lSceTouch_stub \
		-Wl,--end-group
}

# vita-elf-create appends the module's SCE tables to the end of the text
# segment, and fails when the data segment starts too close behind it. The
# linker packs the two tightly, so the data segment is moved to at least
# 64 KB past the end of the text in a second link.
link_launcher
TEXT_SEG=$(arm-vita-eabi-readelf -lW "$OUT/$NAME.elf" | awk '$1 == "LOAD" && / R E / { print $3, $5; exit }')
TEXT_ADDR=${TEXT_SEG% *}
TEXT_SIZE=${TEXT_SEG#* }
[ -n "$TEXT_SEG" ] || { echo "no text segment in $NAME.elf" >&2; exit 1; }
DATA_START=$(printf "0x%x" $(( ( TEXT_ADDR + TEXT_SIZE + 0x10000 + 0xfff ) & ~0xfff )))
link_launcher -Wl,--section-start=.init_array=$DATA_START

# -s: no debug info in the eboot (it held the SDK's absolute paths, with the
# builder's user name); crash logs are read against hl2_vita.elf
# (portal_vita.elf), which keeps it
vita-elf-create -s "$OUT/$NAME.elf" "$OUT/$NAME.velf" > /dev/null
vita-make-fself -s "$OUT/$NAME.velf" "$OUT/eboot.bin"

# ATTRIBUTE2=12 asks for the extended memory budget
vita-mksfoex -s TITLE_ID=$TITLE_ID -s APP_VER=$APP_VER -d ATTRIBUTE2=12 "$TITLE" "$OUT/param.sfo"

# LiveArea: the bubble icon, the page background and the start gate (8-bit
# palette PNGs, as the shell wants). Without them the VPK had only param.sfo,
# and closing the game crashed LiveArea
PACK_ARGS="-a $ART/icon0.png=sce_sys/icon0.png"
for f in bg.png startup.png; do
	PACK_ARGS="$PACK_ARGS -a $ART/livearea/contents/$f=sce_sys/livearea/contents/$f"
done
PACK_ARGS="$PACK_ARGS -a $SRC/$ART_DIR/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml"
for so in $MODULES; do
	name=$(basename "$so")
	arm-vita-eabi-strip --strip-debug -o "$OUT/bin/$name" "$so"
	PACK_ARGS="$PACK_ARGS -a $OUT/bin/$name=bin/$name"
done

vita-pack-vpk -s "$OUT/param.sfo" -b "$OUT/eboot.bin" $PACK_ARGS "$OUT/$NAME.vpk"
echo "$ART_KIND" > "$OUT/$NAME.vpk.art"
ls -l "$OUT/$NAME.vpk"
