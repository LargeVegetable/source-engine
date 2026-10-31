# Half-Life 2 for PS Vita

An unofficial port of the Source engine to the PlayStation Vita that runs **Half-Life 2 from your own copy of the game**. It's built on [nillerusr/source-engine](https://github.com/nillerusr/source-engine), with [vitaGL](https://github.com/Rinnegatamante/vitaGL) for graphics and [kubridge](https://github.com/bythos14/kubridge) to load the engine's modules.

**No game files are included.** You need Half-Life 2 on Steam and you copy its files to your Vita yourself: see **[INSTALL_VITA.md](INSTALL_VITA.md)**.

**Version 0.1.3-beta.** [Download it from the releases page](../../releases/latest).

## Status

- Played from Point Insertion into the middle of Highway 17 (map d2_coast_05), including the buggy. Every later chapter's maps load and run: an automated tour went through ten of them, from Eli's lab to Breen's office.
- Renders at 640×368. Mostly 30–60 fps; the heaviest fights can dip into the low 20s.
- Memory stays flat over long sessions, including with the game paused.
- Half-Life 2 only: Episodes One and Two, Lost Coast, multiplayer and mods aren't supported.

## Features

- Menus made for the Vita: new game by chapter, save and load with screenshots, and options in tabs for controls, game, audio and video. Use the buttons or tap.
- **Prepare shaders** in the main menu: compiles every shader the game uses ahead of time (about 20 minutes the first time), so play doesn't pause the first time something new appears.
- All chapters can be unlocked from New Game (△).
- Every action can be remapped, including to the D-pad, SELECT, and the left and right halves of both the rear touchpad and the touchscreen. Look sensitivity, look acceleration and invert look too.
- Field of view from 55 to 90 (Options > Video).
- Texture detail (Options > Video): Medium, the default, keeps world and character textures sharper; Low uses less video memory. A change applies at the next map or save load.
- Optional button driving for the airboat and the buggy: ✕ accelerates, ○ reverses (Options > Controls).
- Facial animation and lip sync, with a distance setting for performance.
- A larger HUD (three sizes) and PlayStation button symbols in on-screen hints.
- Video options for the heavy effects: distant small props, and the smoke above fires.
- Water with reflections and splashes.
- Lighting on characters and objects changes smoothly as they move between light and shade.
- If something is missing at launch (kubridge, the shader compiler, the game files, or game files from the wrong Steam build), a message says what and how to fix it.
- Rear touchpad crouch (left half) and sprint (right half).
- Optional gyro aiming: turn and tilt the Vita to aim, on top of the right stick, with a sensitivity setting (Options > Controls).
- Closing the game from LiveArea or quitting from the menu goes straight back to the home screen.

## Requirements

- A PS Vita or PS TV running HENkaku or Ensō. Tested on a PS Vita on firmware 3.65, not yet on a PS TV; for other firmwares, see "Known issues".
- **kubridge v0.3.1_hotfix** (bythos14's) as a kernel plugin.
- **libshacccg.suprx**, extracted on your Vita with ShaRKBR33D.
- **Half-Life 2 from Steam, on the Pre-20th Anniversary build** (the Steam beta `steam_legacy`). Files from the current anniversary build don't work.
- About 3.9 GB free on ux0 for the `hl2` and `platform` folders.

Step by step: **[INSTALL_VITA.md](INSTALL_VITA.md)**.

## Controls

| Button | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| R | Fire |
| L | Alternate fire |
| ✕ | Jump |
| □ | Use |
| ○ | Reload |
| △ | Flashlight |
| Rear touchpad, left half | Crouch |
| Rear touchpad, right half | Sprint |
| Touchscreen, left or right half | Unassigned: pick actions in Options > Controls |
| START | Menu |

The rest (switching weapons, zoom, gravity gun, quick save and quick load) is listed in **Options > Controls**. To move an action to another button, select it with ✕, then press the new button or tap a half of the touchscreen.

The touchscreen halves work as buttons only in game; in the menus you tap as usual.

**Gyro aiming** is off by default: turn it on in Options > Controls. Turning the Vita left or right turns the view, and tilting it looks up or down, on top of the right stick. At sensitivity 1 the view turns as far as the Vita does.

**Vehicle controls** in Options > Controls: set to **Cross & circle**, ✕ accelerates and ○ brakes and reverses in the airboat and the buggy, alongside the left stick, which still steers. The handbrake then moves to the rear touchpad's left half.

**Look acceleration** (Options > Controls) is on by default: the view turns slowly over most of the stick's travel, then speeds up the longer it's pushed all the way. Turned off, the view turns in proportion to how far the stick is pushed.

On a PS TV there's no rear touchpad, touchscreen or gyro, so move Crouch to another button in Options > Controls.

In the menus: D-pad or left stick to move, ✕ to select, ○ to go back, L and R to switch tabs, △ to delete a save or, in New Game, to unlock all chapters.

## Settings

The Options menu covers the usual settings. Two files cover the rest:

- **Render resolution:** put `-w 960 -h 544` in `ux0:data/halflife2/commandline.txt`. The Vita's display takes 480×272, 640×368 (the default), 720×408 and 960×544. Higher resolutions cost frame rate in busy scenes.
- **Console commands and variables:** put them in `ux0:data/halflife2/hl2/cfg/vita_user.cfg`, which runs at every launch. The game rewrites `vita_user_example.cfg` next to it at each launch, listing the useful ones with what they do.

## Performance notes

- **Shaders are compiled on the Vita, about half a second to a second each.** Choose **Prepare shaders** in the main menu to compile them all ahead of time: about 20 minutes the first time (keep the Vita plugged in), a few minutes if run again, and it can be stopped and continued. Restart the game afterwards. Without it, the game compiles each shader the first time it's drawn, which hitches briefly, early on and in each new area. Compiled shaders are kept in `ux0:data/shader_cache/` either way.
- Physics-heavy moments (many objects tumbling at once) can still hitch briefly.

## Known issues

- Rarely, the game can crash or freeze while loading the next map (0.1.3 fixes one cause of the freezes). Save often, and if it happens, please report it with the logs below.
- Reflections and specular highlights from cube maps are off on most surfaces. Water does reflect, from pre-made cube maps, so its reflections don't show characters or moving objects.
- The intro videos don't play.
- Suspending and resuming (pressing PS mid-game, or putting the Vita to sleep) hasn't been tested much. Save before you do.
- Saves made with the anniversary build don't load on the Pre-20th Anniversary build.
- kubridge's memory functions, which the port relies on, have known kernel offsets only for firmware 3.60 and 3.65. On other firmwares kubridge uses the 3.65 offsets and warns that crashes may occur. If you play on another firmware, please report how it goes, even when it works.

## Reporting bugs

If something is missing at launch, the game shows a message saying what. The last line of `ux0:data/hl2_vita_boot.txt` says the same.

If the game freezes, wait a minute before closing it: it writes a report to the end of `ux0:data/hl2_vita_boot.txt`.

For more detailed logs, add `-vitadebug` to `ux0:data/halflife2/commandline.txt` (create the file if it isn't there) and play until the problem happens again. Remove it afterwards: the extra logging costs a little performance.

For anything else, please include:

- your firmware version and how it's hacked (HENkaku, Ensō, h-encore), and the model (PS Vita 1000 or 2000, PS TV);
- where the game is installed (Sony memory card, SD2Vita and the card's brand, or internal storage), and any other plugins you run, especially overclocking or memory ones;
- whether it was the first launch, and how long you waited;
- `ux0:data/hl2_vita_boot.txt`, the log of the last session (`hl2_vita_boot.1.txt` to `.4.txt` are the sessions before it);
- `ux0:data/halflife2/engine.log` (`engine.1.log` to `.4.log` are the sessions before it);
- `ux0:data/halflife2/crash.log`, if there is one;
- where you were in the game and what happened;
- the version, shown at the top right of the menus.

Copy the logs before launching the game again more than four times: only the last five sessions are kept.

## Building from source

You need [VitaSDK](https://vitasdk.org) (with `VITASDK` set and its `bin` folder on your `PATH`) and Python 3. The launcher links VitaSDK's SDL2, freetype, libpng, libjpeg, zlib, bzip2, opus, vitashark, SceShaccCgExt, mathneon and kubridge stubs; install any that are missing with vdpm.

1. **vitaGL:** check out [the port's vitaGL fork](https://github.com/LargeVegetable/vitaGL/tree/hl2-vita) (branch `hl2-vita`) next to this repository, as `../vitaGL`, and build it with `sh build-hl2.sh`. The launcher links `../vitaGL/libvitaGL.a` when it's there and falls back to VitaSDK's copy otherwise. VitaSDK's copy is built without error logging and without the options the port needs.
2. **Engine modules:** from this repository, on the `vita` branch with submodules checked out (`git submodule update --init`):

   ```bash
   python3 ./waf configure -T release --vita --togles --disable-warns -o build-vita
   python3 ./waf build -j8
   ```

3. **Launcher and VPK:**

   ```bash
   sh launcher_vita/build.sh
   ```

   The VPK is `build-vita/launcher_vita/hl2_vita.vpk`. The launcher is linked after the modules because its table of exported symbols is generated from what they import.

4. **Release package:** `sh launcher_vita/package_release.sh` zips the VPK with these documents and the licenses as `dist/hl2_vita-<version>.zip`. The version is in `launcher_vita/version.h`; the LiveArea art and its format are described in `launcher_vita/sce_sys/ART.md`.

The port's own code is mostly in `launcher_vita/` (ELF loader, memory, crash log, LiveArea assets) and in `PLATFORM_VITA` blocks across the engine.

## Credits

- Valve, for the Source engine and Half-Life 2.
- [nillerusr](https://github.com/nillerusr/source-engine) and contributors, for the cross-platform Source engine this is built on.
- [Rinnegatamante](https://github.com/Rinnegatamante), for vitaGL, vitashark and ShaRKBR33D.
- [bythos14](https://github.com/bythos14/kubridge) and [TheFloW](https://github.com/TheOfficialFloW), for kubridge.
- The [VitaSDK](https://vitasdk.org) team, and the SDL2, freetype and minimp3 projects.
- [highda](https://github.com/highda), for fixes and speed-ups to the port's threading, loading, sound, saving and shader preparation.

## Legal

This project isn't affiliated with or endorsed by Valve or Sony. Half-Life, Half-Life 2 and the Lambda logo are trademarks of Valve Corporation.

No game content is distributed: you need your own legally purchased copy of Half-Life 2. The engine source this port builds on is based on a 2018 leak of Valve's code (see nillerusr/source-engine) and isn't licensed for commercial use: this port is free and must stay free.

The port's changes are distributed under the same terms as the engine source (the Source SDK License in `LICENSE`). The vitaGL fork is LGPL-3.0, and its source is published with the port. Other libraries in the VPK: see `THIRD_PARTY_VITA.md`.
