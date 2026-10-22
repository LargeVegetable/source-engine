# Installing Half-Life 2 on PS Vita

This guide takes you from a hacked Vita and a Steam copy of Half-Life 2 to playing. It takes about half an hour, most of it copying files.

The download is `hl2_vita-<version>.zip` on the [releases page](../../releases/latest); `hl2_vita.vpk` inside it is the game.

## What you need

- A PS Vita or PS TV running **HENkaku or Ensō**, with **VitaShell** installed.
- **Half-Life 2 on Steam**, on a PC or Mac.
- A USB cable or Wi-Fi, to copy files to the Vita.
- About 3.9 GB free on ux0 (the memory card, or SD2Vita card) for the game files.

## 1. Install kubridge

kubridge is a kernel plugin that lets the game load its engine modules. The port needs **bythos14's v0.3.1_hotfix** or later: older versions lack functions the port uses or can freeze the console.

1. Download `kubridge.skprx` from [bythos14's releases](https://github.com/bythos14/kubridge/releases) (v0.3.1_hotfix: 11,630 bytes).
2. Copy it to `ur0:tai/` on your Vita.
3. Open your taiHEN config: `ux0:tai/config.txt` if it exists (taiHEN uses that one first), otherwise `ur0:tai/config.txt`. Under the `*KERNEL` line, add:

   ```
   ur0:tai/kubridge.skprx
   ```

   If an older kubridge is already listed, replace that line: bythos14's version is meant as a drop-in replacement for the older one, which other ports use.
4. Reboot the Vita.

## 2. Extract the shader compiler

The game compiles its shaders on the Vita with Sony's shader compiler, `libshacccg.suprx`, which you extract from your own console. If you've already done this for another port, skip this step.

1. Install and run [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D).
2. It places `libshacccg.suprx` in `ur0:data/`. The game also accepts `ur0:data/external/libshacccg.suprx`.

## 3. Switch Half-Life 2 to the Pre-20th Anniversary build

The port only works with the game files from before Half-Life 2's 20th anniversary update (November 2024): that update changed the shaders. Valve keeps the older build as a Steam beta.

1. In your Steam library, right-click **Half-Life 2** and choose **Properties**.
2. Open **Betas** and select **steam_legacy - Pre-20th Anniversary Build**.
3. Wait for Steam to finish downloading.
4. Right-click Half-Life 2 again and choose **Manage > Browse local files**. You need two folders from here: `hl2` and `platform`.

You can switch Steam back to the current build once the files are copied.

## 4. Copy the game files to the Vita

1. On the Vita, create the folder `ux0:data/halflife2/`.
2. Copy the `hl2` and `platform` folders into it, so that you get:

   ```
   ux0:data/halflife2/hl2/gameinfo.txt
   ux0:data/halflife2/hl2/hl2_misc_dir.vpk
   ux0:data/halflife2/platform/...
   ```

   You can leave out `hl2/bin`: those are PC binaries, which the Vita doesn't use.

VitaShell can copy them either way:
- over **USB**, which opens the memory card as a drive on your computer; or
- over **FTP**, where you connect from an FTP client to the address VitaShell shows. That's slower, but needs no cable.

See VitaShell's own instructions for the buttons.

## 5. Install the game

1. Copy `hl2_vita.vpk` anywhere on ux0, for example `ux0:downloads/`.
2. In VitaShell, select the VPK, press ✕ and confirm the install.
3. The Half-Life 2 bubble appears on the home screen.

## 6. Play

- **Expect hitches the first time you see new things:** the game compiles each shader on the Vita the first time it's drawn, and in the first minutes that can freeze the picture for several seconds. It isn't a crash; let it carry on. The compiled shaders are kept in `ux0:data/shader_cache/`, so it only happens once.
- Your saves are in `ux0:data/halflife2/hl2/save/` and your settings in `ux0:data/halflife2/hl2/cfg/`.
- Controls and settings: see [README_VITA.md](README_VITA.md).

## Troubleshooting

**A message appears at launch.** It names what's missing and the step to redo:

| Message | Fix |
|---|---|
| kubridge is not loaded | Redo step 1, and check you edited the config file taiHEN actually uses. |
| The shader compiler (libshacccg.suprx) is missing | Redo step 2. |
| Half-Life 2's game files weren't found, or the platform folder is missing | Check the folder names in step 4. |
| These game files are from Half-Life 2's 20th anniversary update | Redo step 3, then copy `hl2` again. |

**The game closes straight away, with no message.** Open `ux0:data/hl2_vita_boot.txt` in VitaShell; the last lines say why.

**The world is black or garbled, or the log says `SHADER DATA MISMATCH`.** The files come from the anniversary build: redo step 3 and copy `hl2` again, at least `hl2/shaders`.

**A save won't load.** Saves made with the anniversary build don't load on the Pre-20th Anniversary build.

**Anything else:** see "Reporting bugs" in [README_VITA.md](README_VITA.md). If you run other kernel plugins, try again with them disabled.

## Updating

Install the new VPK over the old one. Your saves and settings stay.

## Uninstalling

1. Delete the bubble from the home screen.
2. Delete `ux0:data/halflife2/` and `ux0:data/shader_cache/SRCV00001/`.
3. If nothing else uses it, remove the kubridge line from your taiHEN config.
