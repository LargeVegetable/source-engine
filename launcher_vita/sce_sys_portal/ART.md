# LiveArea art: Portal

The same files and format as Half-Life 2's art in `../sce_sys/` (see its `ART.md`):

| File | Size (pixels) | Shown as |
|---|---|---|
| `icon0.png` | 128 × 128 | the bubble on the home screen |
| `livearea/contents/bg.png` | 840 × 500 | the LiveArea page background |
| `livearea/contents/startup.png` | 280 × 158 | the start button ("gate") on the LiveArea page |

Portal is a trademark of Valve Corporation; this port isn't affiliated with or endorsed by Valve.

**Format:** PNG with an 8-bit palette, without metadata. **After changing art:** raise `content-rev` in `livearea/contents/template.xml` by one.

`../sce_sys_portal_dev/` (not in git) can hold other art for Portal's testing builds: `HL2_VITA_DEV=1 sh launcher_vita/build.sh build-vita-portal` uses it when it's there.
