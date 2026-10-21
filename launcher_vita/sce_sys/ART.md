# LiveArea art

| File | Size (pixels) | Shown as | Content |
|---|---|---|---|
| `icon0.png` | 128 × 128 | the bubble on the home screen | the Half-Life 2 logo |
| `livearea/contents/bg.png` | 840 × 500 | the LiveArea page background | an in-game screenshot of City 17 |
| `livearea/contents/startup.png` | 280 × 158 | the start button ("gate") on the LiveArea page | the HALF-LIFE 2 lettering on white |

Half-Life, Half-Life 2 and the Lambda logo are trademarks of Valve Corporation; this port isn't affiliated with or endorsed by Valve.

**Format:** PNG with an 8-bit palette (indexed color, up to 256 colors). The Vita shell may reject or garble true-color PNGs. To convert, use for example `pngquant 256 --force --output out.png in.png`, or in Python with Pillow: `Image.open(p).convert("RGB").quantize(256).save(p)`. Leave out metadata (EXIF, text chunks, color profiles).

**After changing art:** raise `content-rev` in `livearea/contents/template.xml` by one. The shell caches page art by revision, so without that an updated install keeps the old background and gate.

`../sce_sys_dev/` (not in git) can hold other art for testing builds: `HL2_VITA_DEV=1 sh launcher_vita/build.sh` uses it when it's there.
