# igpSP Rockboy Plugin

> A port of the GBA emulator igpSP as a native Rockbox plugin — built for the iPod Classic 6G.

![Target](https://img.shields.io/badge/target-iPod%20Classic%206G-silver?style=flat-square)
![Language](https://img.shields.io/badge/language-C-blue?style=flat-square)
![Firmware](https://img.shields.io/badge/firmware-Rockpod-green?style=flat-square)
![Status](https://img.shields.io/badge/status-in%20development-yellow?style=flat-square)

---

## What It Does

iPod Linux and ZeroSlackr include fast GBA emulation via igpSP, but neither supports the 6th gen Classic — Apple's encrypted firmware locked them out. Rockbox supports the 6G natively. This project ports igpSP's emulation core into Rockbox's plugin system, replacing the iPod Linux platform layer (raw `/dev/fb`, `/dev/dsp`, GPIO register writes) with Rockbox plugin API calls — bringing GBA emulation to a device that has never had it.

The core engine is left completely untouched. A thin platform abstraction layer and a compile-time shim header handle everything Rockbox-specific, invisible to the emulator core.

---

## Hardware Target

| | |
|---|---|
| **Device** | iPod Classic 6G (Model A1238) |
| **SoC** | Samsung S5L8702 |
| **CPU** | ARM926EJ-S @ ~216MHz (boosted) / ~54MHz (idle) |
| **RAM** | 64 MB SDRAM |
| **Screen** | 320×240 LCD, RGB565 |
| **Audio DAC** | Wolfson WM8758 |
| **Storage** | iFlash Quad (microSD) |
| **Firmware** | Rockpod (Rockbox fork) |
| **Plugin API** | PLUGIN_API_VERSION 281 (Rockbox ~2023+) |
| **GBA source res** | 240×160 RGB565 → scaled 4:3 H, 3:2 V → 320×240 |

---

## How It Works

```
igpSP core engine (cpu.c, memory.c, video.c, sound.c)
        │
        │  renders GBA frame (240×160 RGB565)
        ▼
platform/rockbox/sys_rockbox_gba.c
        │
        ├── vid_update()     →  scale 240×160 → 320×240, rb->lcd_update()
        ├── input_poll()     →  rb->wheel_status() + rb->button_get()
        ├── pcm_callback()   →  rb->mixer_channel_play_data() ring buffer drain
        └── sys_malloc()     →  rb->plugin_get_audio_buffer() bump allocator
        │
        ▼
Rockbox / Rockpod on iPod Classic 6G
```

The original iPod Linux layer used `/dev/fb` for video, `/dev/dsp` for audio, and raw PP502x co-processor register writes for sound sync. All of that is replaced. The S5L8702 in the 6G is a different chip from the PP502x entirely — the COP code is not ported and is not needed.

---

## Status

| Phase | Description | Status |
|---|---|---|
| 1 | Scaffold — platform stubs, plugin entry point, build file | ✅ Complete |
| 2 | Video — nearest-neighbour scale 240×160 → 320×240, frameskip | ✅ Complete |
| 2b | Video — ARM assembly optimised inner blit loop | 🔲 Pending |
| 3 | Input — clickwheel d-pad zones, button binds, in-game menu | ✅ Complete |
| 4 | Audio — lock-free PCM ring buffer, Rockbox mixer callback | ✅ Complete |
| 5 | Core integration — malloc/POSIX shims, plugin_start() wired to igpSP loop | 🔧 In progress |
| 6 | Polish — save states, SRAM saves, frame limiter, ZIP support | 🔲 Pending |

---

## Project Structure

```
igsp_rockboy_plugin/
├── igpsp.c                      # Rockbox plugin_start() entry point
├── igpsp.make                   # Rockbox build system integration
├── platform/
│   └── rockbox/
│       ├── sys_rockbox_gba.h    # Platform abstraction API (public header)
│       └── sys_rockbox_gba.c    # All Rockbox API calls live here
├── src/
│   ├── rockbox_compat.h         # Shim: malloc/FILE*/POSIX/exit → sys_* (Phase 5)
│   │                            #   injected via -include into igpSP core files
│   ├── main.c                   # igpSP: emulator loop, update_gba(), quit()
│   ├── cpu.c                    # igpSP: ARM interpreter
│   ├── cpu_threaded.c           # igpSP: ARM JIT dynarec — MUST be compiled -marm
│   ├── memory.c                 # igpSP: GBA memory map, ROM/BIOS load
│   ├── video.c                  # igpSP: GBA PPU renderer
│   ├── sound.c                  # igpSP: GBA APU mixer
│   └── common.h / memory.h / … # igpSP headers (copy from upstream before building)
├── .gitignore
└── README.md
```

> **Note:** `src/*.c` and `src/*.h` (except `rockbox_compat.h`) are igpSP upstream source files and are not tracked in this repository. Copy them from https://github.com/iPodLinux-Community/igpSP before building.

**Invariant:** Nothing inside `src/` may include `plugin.h` or call `rb->` directly. All Rockbox API access is funnelled through `platform/rockbox/sys_rockbox_gba.{h,c}`.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                  Rockbox Firmware                        │
│  plugin_load()  →  fills rb*  →  calls plugin_start()   │
└──────────────────────┬──────────────────────────────────┘
                       │  rb (struct plugin_api *)
┌──────────────────────▼──────────────────────────────────┐
│                    igpsp.c                               │
│  plugin_start()  PLUGIN_HEADER  lifecycle management     │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│         platform/rockbox/sys_rockbox_gba.c               │
│                                                          │
│  Video     vid_init / vid_update / vid_exit              │
│  Input     input_init / input_poll / input_read_keys     │
│  Audio     sound_init / sound_write / sound_exit         │
│  CPU       sys_boost_cpu / sys_unboost_cpu               │
│  Timing    sys_sleep_ms / sys_get_ticks                  │
│  FS        sys_open/read/write/close/seek                │
│  Memory    sys_mem_init / sys_malloc (bump allocator)    │
└──────────────────────┬──────────────────────────────────┘
                       │  (no Rockbox includes below this line)
┌──────────────────────▼──────────────────────────────────┐
│                    src/                                  │
│                                                          │
│  cpu_threaded.c   — ARM JIT dynarec  (MUST be -marm)    │
│  memory.c         — GBA memory map                       │
│  video.c          — GBA PPU renderer                     │
│  sound.c          — GBA APU                              │
│  main.c           — emulator loop (update_gba)           │
└─────────────────────────────────────────────────────────┘
```

---

## API Mapping

| igpSP iPod Linux | Rockbox Plugin API | Notes |
|---|---|---|
| `open("/dev/fb0")` | `rb->lcd_get_framebuffer(&w, &stride)` | Returns live `fb16_t *` framebuffer |
| `write(fb_fd, pixels)` | Direct write to `fb16_t *` + `rb->lcd_update()` | Bypasses `lcd_bitmap()` overhead |
| `open("/dev/dsp")` | `rb->mixer_channel_play_data(CHAN, cb, …)` | PCM mixer callback replaces `/dev/dsp` |
| `write(dsp_fd, samples)` | Ring buffer → `pcm_callback()` drain | Callback runs from IRQ — must be lock-free |
| `open(rom_path, O_RDONLY)` | `rb->open(path, O_RDONLY, 0)` | Nearly 1:1 |
| `read(fd, buf, len)` | `rb->read(fd, buf, len)` | 1:1 |
| `write(fd, buf, len)` | `rb->write(fd, buf, len)` | 1:1 |
| `close(fd)` | `rb->close(fd)` | 1:1 |
| `lseek(fd, off, whence)` | `rb->lseek(fd, off, whence)` | 1:1 |
| COP register write | `rb->cpu_boost(true)` | Rockbox abstracts the PLL/PMU |
| `malloc(size)` | `sys_malloc()` bump allocator | Backed by `rb->plugin_get_audio_buffer()` |
| `usleep(us)` | `rb->sleep(ticks)` — `ticks = ms * HZ / 1000` | 10ms resolution |
| `gettimeofday()` | `*rb->current_tick` | Wraps ~497 days |
| `/dev/input` events | `rb->button_get()` + `rb->wheel_status()` | 0–95 absolute position → 4 d-pad zones |

---

## Input Mapping

| iPod Control | GBA Button |
|---|---|
| Wheel North (pos 84–95, 0–11) | D-pad Up |
| Wheel East (pos 12–35) | D-pad Right |
| Wheel South (pos 36–59) | D-pad Down |
| Wheel West (pos 60–83) | D-pad Left |
| Center (SELECT) | A |
| Play/Pause | B |
| Menu | Start |
| Rewind (held) | Select |
| Forward (held) | R trigger |
| Hold switch | In-game menu |

---

## Memory Layout

The plugin audio buffer (~60MB on the 6G) is claimed once via `rb->plugin_get_audio_buffer()` and used as a bump allocator. No `free()` is ever called.

| Region | Size |
|---|---|
| EWRAM | 256 KB |
| IWRAM | 32 KB |
| VRAM | 96 KB |
| OAM | 1 KB |
| Palette RAM | 1 KB |
| GBA framebuffer (240×160×2) | 75 KB |
| Sound ring buffer | 16 KB |
| ROM (capped at 16MB) | up to 16 MB |
| **Total without ROM** | **~477 KB** |
| **Total with ROM** | **~16.5 MB** |

---

## BIOS

igpSP requires a GBA BIOS dump (`gba_bios.bin`) from your own hardware. Place it at:

```
/.rockbox/igpsp/gba_bios.bin
```

The plugin also checks the same directory as the loaded ROM before falling back to the above path.

---

## Building

Once the port is complete, this plugin integrates into the standard Rockbox build system:

```bash
# Clone Rockpod (target firmware)
git clone https://github.com/nuxcodes/rockpod

# Copy this repo into the plugins directory
cp -r igsp_rockboy_plugin rockpod/apps/plugins/igpsp

# Copy igpSP upstream source files into src/
# (not tracked in this repo)
git clone https://github.com/iPodLinux-Community/igpSP /tmp/igpsp
cp /tmp/igpsp/*.c /tmp/igpsp/*.h rockpod/apps/plugins/igpsp/src/

# Register plugin for ipod6g target
echo 'igpsp' >> rockpod/apps/plugins/SOURCES

# Configure and build
cd rockpod && mkdir build && cd build
../tools/configure    # select: iPod Classic 6G
make
make install          # outputs .rockbox/rocks/games/igpsp.rock
```

---

## Known Limitations

- **ARM dynarec is not Thumb-safe** — `cpu_threaded.c` generates and executes ARM machine code at runtime. The `.make` file enforces `-marm` with a per-file override. Do not change this.
- **No COP support** — the PP502x co-processor code from igpSP is not ported. The 6G uses an S5L8702. Rockbox's PCM mixer callback replaces it entirely.
- **ROM cap at 16MB** — enforced as a bump allocator safety margin. Covers virtually the entire GBA commercial library.
- **No L shoulder button** — the iPod has no hardware equivalent. A button combo can be mapped in Phase 6.
- **igpSP sources not tracked** — copy upstream files into `src/` before building (see above).

---

## Reference Repositories

- [igpSP](https://github.com/iPodLinux-Community/igpSP) — emulator core and original iPod Linux platform layer
- [Rockbox](https://github.com/rockbox/rockbox) — firmware, plugin API, Rockboy reference implementation
- [Rockpod](https://github.com/nuxcodes/rockpod) — target firmware with Cover Flow and iFlash optimisations
- [iPodLoader2](https://github.com/crozone/ipodloader2) — bootloader reference
- [ProjectZeroSlackr](https://github.com/ProjectZeroSlackr) — original ZeroSlackr suite (iPod Linux, 5G/5.5G only)

---

## Notes for Claude Code

- Read this file and `platform/rockbox/sys_rockbox_gba.c` before writing anything
- Files in `src/` must not be modified — all shims go in `src/rockbox_compat.h` only, injected via `-include` in `igpsp.make`
- All `rb->` calls must stay inside `platform/rockbox/` — nothing in `src/` may touch the Rockbox API directly
- `cpu_threaded.c` contains hand-written ARM32 assembly and a JIT that writes raw opcodes at runtime — never compile as Thumb, never remove the `-marm` rule in `igpsp.make`
- The clickwheel reports absolute position 0–95 via `rb->wheel_status()` (0 = top, clockwise); divide into 4 zones of ~24 positions for d-pad
- Claim the audio buffer first in `plugin_start()` via `sys_mem_init()` before any core init
- `pcm_callback()` runs in IRQ context — no `rb->` calls, no malloc, no blocking of any kind
- Every TODO in the codebase references its phase number — use these as entry points
- Rockboy's `sys_rockbox.c` in the Rockbox repo is the closest structural analog — read it first

---

*Built for the people who still think the click wheel was peak UI.*
