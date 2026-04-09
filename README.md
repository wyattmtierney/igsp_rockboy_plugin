# igpSP-Rockbox

A native Rockbox plugin that ports the **igpSP** (iPod gameplaySP) GBA emulator to the iPod Classic 6th generation — no iPod Linux, no ZeroSlackr, no Apple firmware decryption required.

---

## Project Goal

Run Game Boy Advance games inside Rockbox on the iPod Classic 6G.

The iPod 6G is the last iPod model with no community-maintained alternative OS (iPod Linux / ZeroSlackr only support 5G and earlier because Apple introduced encrypted firmware signing on the 6G).  Rockbox runs natively on the 6G.  This plugin brings the igpSP dynarec-based GBA emulator into the Rockbox plugin environment, replacing the iPod Linux-specific platform layer with a Rockbox-native one.

---

## Hardware Target

| Property          | Value                                              |
|-------------------|----------------------------------------------------|
| Device            | iPod Classic 6th Generation (Model A1238)          |
| SoC               | Samsung S5L8702                                    |
| CPU               | ARM926EJ-S @ ~216 MHz (boosted) / ~54 MHz (idle)  |
| RAM               | 64 MB SDRAM                                        |
| LCD               | 320 × 240, RGB565                                  |
| Audio DAC         | Wolfson WM8758                                     |
| Firmware          | Rockbox (open source)                              |
| Plugin API ver.   | PLUGIN_API_VERSION 281 (Rockbox ~2023+)            |
| GBA screen        | 240 × 160, RGB565 source                           |
| Scale factor      | 4:3 horizontal, 3:2 vertical → full 320 × 240 fit |

---

## Repository Layout

```
igsp_rockboy_plugin/
├── igpsp.c                         # Rockbox plugin_start() entry point
├── igpsp.make                      # Rockbox build system integration
├── platform/
│   └── rockbox/
│       ├── sys_rockbox_gba.h       # Platform abstraction API (public header)
│       └── sys_rockbox_gba.c       # Full implementations (all Rockbox calls here)
├── src/
│   ├── rockbox_compat.h            # Phase 5: malloc/FILE*/POSIX/exit shim
│   │                               #   injected via -include into igpSP core files
│   ├── main.c                      # igpSP: emulator loop, update_gba(), quit()
│   ├── cpu.c                       # igpSP: ARM interpreter
│   ├── cpu_threaded.c              # igpSP: ARM JIT dynarec (MUST be -marm)
│   ├── memory.c                    # igpSP: GBA memory map, ROM/BIOS load
│   ├── video.c                     # igpSP: GBA PPU renderer
│   ├── sound.c                     # igpSP: GBA APU mixer
│   ├── common.h / memory.h / …     # igpSP headers (copy from upstream)
│   └── .gitkeep
├── .gitignore
└── README.md
```
> **Note:** `src/*.c` and `src/*.h` (except `rockbox_compat.h`) are igpSP upstream
> source files and are not tracked in this repository.  Copy them from
> https://github.com/iPodLinux-Community/igpSP before building.

**Invariant:** Nothing inside `src/` may include `plugin.h` or call `rb->` directly.  All Rockbox API access is funnelled through `platform/rockbox/sys_rockbox_gba.{h,c}`.

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
│                    src/  (Phase 2+)                      │
│                                                          │
│  cpu_threaded.c   — ARM JIT dynarec  (MUST be -marm)    │
│  memory.c         — GBA memory map                       │
│  video.c          — GBA PPU renderer                     │
│  sound.c          — GBA APU                              │
│  main.c           — emulator loop (update_gba)           │
│  gba_memory.c     — cartridge / BIOS loading             │
│  gba_io.c         — I/O register handling                │
│  gba_bios.c       — BIOS HLE                             │
└─────────────────────────────────────────────────────────┘
```

---

## Phase Checklist

- [x] **Phase 1 — Scaffold**
  - [x] Directory structure and build system (`igpsp.make`)
  - [x] Platform abstraction header (`sys_rockbox_gba.h`) with all subsystem stubs
  - [x] `sys_rockbox_gba.c` — stub implementations, all `rb->` calls isolated here
  - [x] `igpsp.c` — `plugin_start()` entry point: boost CPU, init subsystems, placeholder screen, clean teardown
  - [x] `.gitignore` (ROMs, build artefacts)
  - [x] `README.md` with port plan

- [ ] **Phase 2 — Video**
  - [x] Obtain Rockbox framebuffer pointer via `rb->lcd_get_framebuffer()`
  - [x] Implement `vid_update()`: nearest-neighbour scale 240×160 → 320×240
  - [x] Direct framebuffer writes (bypass `lcd_bitmap()` overhead)
  - [x] `rb->lcd_update()` once per frame
  - [x] Pre-compute sx[]/sy[] lookup tables in `vid_init()`
  - [x] `blit_frame_c()` hot loop isolated — marked as Phase 2b ARM asm target
  - [x] Frameskip support (`frameskip` + `frame_skip_counter`)
  - [ ] Phase 2b: Replace `blit_frame_c()` with ARM asm inner loop
  - [ ] (Optional) Integer EPX / bilinear filter

- [x] **Phase 3 — Input**
  - [x] Read absolute clickwheel position (0–95) via `rb->wheel_status()`
  - [x] Map 4 cardinal zones to GBA D-pad (zone-change debounce, mirrors SCROLL_MOD)
  - [x] Map physical click buttons: SELECT→A, PLAY→B, MENU→Start, LEFT→Select, RIGHT→L
  - [x] Hold switch (rising-edge) → in-game menu: Resume / Save State / Load State / Quit
  - [x] Quit sets `exit_requested = true`; emulator main loop exits cleanly
  - [x] `input_read_keys()` returns active-low GBA KEYINPUT format (0x3FF = all released)

- [x] **Phase 4 — Audio**
  - [x] Allocate lock-free SPSC ring buffer from plugin heap (`sys_malloc`)
  - [x] `pcm_callback()` drains ring buffer; returns silence on underrun
  - [x] `sound_write()` enqueues stereo frames; drops oldest on overrun (non-blocking)
  - [x] `sound_init()` sets 22050 Hz via `rb->mixer_set_frequency()`, starts `PCM_MIXER_CHAN_PLAYBACK`
  - [x] `sound_set_volume()` maps 0–100 → `SOUND_VOLUME` range via `rb->sound_set()`
  - [x] `sound_exit()` stops mixer channel, restores `HW_SAMPR_DEFAULT`
  - [x] COP replacement documented — PP502x registers absent on S5L8702; Rockbox DMA IRQ covers the same role
  - [x] Ring buffer depth: 4096 stereo frames (≈ 11 video frames @ 22050 Hz / 60 fps)

- [x] **Phase 5 — Memory, Filesystem, Core Integration**
  - [x] `src/rockbox_compat.h` shim: `-include` injected via `IGPSP_CORE_CFLAGS`
    - [x] `malloc` → `sys_malloc`, `calloc` → `rb_calloc`, `free` → no-op
    - [x] `fopen/fread/fwrite/fclose/fseek/ftell/fgets` → `rb_f*()` wrappers
    - [x] `open/read/write/close/lseek` → `sys_open/read/write/close/seek`
    - [x] `exit(code)` → `longjmp(igpsp_exit_jmp, 1)` for clean Rockbox exit
    - [x] `IPOD_BUILD` defined to select igpSP's iPod code paths (no SDL)
    - [x] `sound_exit` → `igpsp_sound_exit`, `sound_callback` → `igpsp_sound_callback` (avoid link conflicts)
    - [x] `usleep/sleep` → `sys_sleep_ms`, `getcwd` → stub returning "/"
    - [x] `printf/fprintf` → `DEBUGF` (compiled out in release)
    - [x] `IGPSP_DEBUG` build: `sys_malloc_debug()` logs each allocation to LCD
    - [x] Memory layout documentation: ~20.7 MB total (ROM cap 16 MB), ~40 MB headroom on 6G
  - [x] `sys_rockbox_gba.c` additions:
    - [x] `FILE*` slot pool (8 slots, bump-allocated): `rb_fopen/fclose/fread/fwrite/fseek/ftell/fgets/feof`
    - [x] `rb_file_length()` — seek-to-end, report size, seek back
    - [x] `rb_file_pool_init()` called from `sys_mem_init()` after heap is ready
    - [x] `sys_malloc()` OOM guard: splash + degenerate fallback pointer
    - [x] `sys_mem_init()` minimum heap check (20 MB threshold with error splash)
    - [x] `ipod_init_hw/exit_hw/exit_video` → `vid_init/exit`
    - [x] `ipod_init_input/exit_input` → `input_init/exit`
    - [x] `ipod_init_sound/exit_sound` → `sound_init(22050,2)/sound_exit`
    - [x] `ipod_init_cop/exit_cop` — no-op (no PP502x COP on S5L8702)
    - [x] `ipod_init_conf/exit_conf` — Phase 6 stub (settings persistence)
    - [x] `ipod_update_ingame_input()` → `input_poll()` + exit-check + active-HIGH key return
    - [x] `update_screen()` → `vid_update(screen)` (bridges igpSP PPU output to Rockbox LCD)
    - [x] `get_ticks_us()` / `delay_us()` — timing shims for igpSP's `synchronize()`
    - [x] `print_string()` — silenced (Phase 6: optional FPS overlay)
    - [x] `sys_malloc_debug()` — IGPSP_DEBUG allocation logger
  - [x] `igpsp.c` rewritten — full Phase 5 startup sequence:
    - [x] BIOS path search: ROM directory first, then `/.rockbox/igpsp/gba_bios.bin`
    - [x] `main_path` set to ROM's directory before igpSP config/BIOS lookup
    - [x] `init_gamepak_buffer()` → `load_bios()` → `init_main()` → `igpsp_init_sound()` → `init_input()` → `load_gamepak()` → `init_cpu()` → `init_memory()` — matches original `main()` order
    - [x] `setjmp(igpsp_exit_jmp)` guard around `execute_arm_translate()` for clean exit
    - [x] All error paths: splash readable message → cleanup → `PLUGIN_ERROR`
    - [x] Teardown in reverse init order; idempotent cleanup safe after longjmp
  - [x] `igpsp.make` updated:
    - [x] `IGPSP_CORE_CFLAGS` = base flags + `-include rockbox_compat.h -DIPOD_BUILD -fno-strict-aliasing -I src/`
    - [x] Explicit rules for `igpsp.o` and `sys_rockbox_gba.o` use `IGPSP_CFLAGS` (no shim)
    - [x] `src/` engine files compiled with `IGPSP_CORE_CFLAGS` via pattern rule
    - [x] `cpu_threaded.c` special rule preserved: `-marm -fno-strict-aliasing` forced
    - [x] Enabled: `main.c cpu.c cpu_threaded.c memory.c video.c sound.c`
  - [ ] Populate `src/` with igpSP source files (copy from upstream repo — not tracked in git)
  - [ ] Integration-test: run first frame of a ROM without hang/crash

- [ ] **Phase 6 — Polish**
  - [ ] In-emulator menu (load ROM, save/load state, volume, exit)
  - [ ] Save-state serialisation to `/gba/<rom>.sgm`
  - [ ] SRAM battery-save to `/gba/<rom>.sav`
  - [ ] Frame-rate limiter + optional frame-skip for slow sections
  - [ ] ZIP ROM support (optional)
  - [ ] Cheat code engine (optional)

---

## Key API Mapping Table

| igpSP iPod Linux call                  | Rockbox equivalent                              | Notes                                         |
|----------------------------------------|-------------------------------------------------|-----------------------------------------------|
| `open("/dev/fb0", O_WRONLY)`           | `rb->lcd_get_framebuffer(&w, &stride)`          | Returns `fb16_t *` to the live framebuffer    |
| `write(fb_fd, pixels, size)`           | Direct write to `fb16_t *` + `rb->lcd_update()` | Skip `lcd_bitmap()` overhead for full frames  |
| `open("/dev/dsp", O_WRONLY)`           | `rb->mixer_channel_play_data(CHAN, cb, …)`      | PCM mixer callback replaces `/dev/dsp` writes |
| `write(dsp_fd, samples, len)`          | Ring buffer → `pcm_callback()` drain            | Callback runs from IRQ; must be lock-free      |
| `open(rom_path, O_RDONLY)`             | `rb->open(path, O_RDONLY, 0)`                   | Nearly 1:1; same POSIX flags                  |
| `read(fd, buf, len)`                   | `rb->read(fd, buf, len)`                        | 1:1                                           |
| `write(fd, buf, len)`                  | `rb->write(fd, buf, len)`                       | 1:1                                           |
| `close(fd)`                            | `rb->close(fd)`                                 | 1:1                                           |
| `lseek(fd, off, whence)`               | `rb->lseek(fd, off, whence)`                    | 1:1                                           |
| COP register write (CPU boost)         | `rb->cpu_boost(true)`                           | Rockbox abstracts the PLL/PMU                 |
| `malloc(size)` / no `free()`           | `sys_malloc()` bump allocator                   | Backed by `rb->plugin_get_audio_buffer()`     |
| `usleep(us)` / `sleep(s)`             | `rb->sleep(ticks)` where tick = 10 ms           | `ticks = ms * HZ / 1000`                      |
| `gettimeofday()` / `clock()`          | `*rb->current_tick`                             | 10 ms resolution; uint32_t wraps ~497 days    |
| `/dev/input` clickwheel events         | `rb->button_get()` + wheel position API         | Map 0–95 absolute position to 4 D-pad zones  |
| iPod button matrix                     | `BUTTON_SELECT/PLAY/MENU/LEFT/RIGHT` constants  | Defined by Rockbox for each keypad variant    |

---

## Notes for Claude Code

Everything a future session needs to pick up Phase 2+ without this conversation:

### Build System Integration

- The plugin lives at `$(APPSDIR)/plugins/igpsp/` inside a Rockbox source tree.
- `igpsp.make` is included by `$(APPSDIR)/plugins/Makefile` (add one include line).
- `PLUGINFLAGS` from the Rockbox build system provides: `-fpic`, `-fno-builtin`, correct sysroot, and device-specific defines.  `IGPSP_CFLAGS` strips `-mthumb`/`-mthumb-interwork` and adds `-marm`.
- The output is `$(BUILDDIR)/apps/plugins/igpsp/igpsp.rock` → installed to `/.rockbox/rocks/games/igpsp.rock`.

### rb Pointer Flow

1. `PLUGIN_HEADER` in `igpsp.c` creates `const struct plugin_api *rb DATA_ATTR;`.
2. The Rockbox loader fills `rb` before calling `plugin_start()`.
3. `platform/rockbox/sys_rockbox_gba.c` declares `rb` as `extern` (via the header).
4. `src/` files never touch `rb` — they call `sys_*()` / `vid_*()` / `sound_*()` etc.

### Memory Layout

- `sys_mem_init()` calls `rb->plugin_get_audio_buffer(&size)` once.  This surrenders the PCM DMA buffer (~60 MB on 6G) to the plugin.
- `sys_malloc()` is a bump allocator: `ptr = heap_ptr; heap_ptr += aligned_size;`.  No `free()` ever.
- All igpSP static arrays (translation caches, EWRAM, IWRAM, VRAM, OAM, palette) should be allocated via `sys_malloc()` instead of being static globals, to keep BSS/data segment small (Rockbox plugins have limited overlay space on some targets, though 6G is not overlay-limited).

### Video Scaling

- GBA: 240 × 160 RGB565.  iPod 6G: 320 × 240 RGB565.
- Scale: 4/3 horizontal, 3/2 vertical — both are exact integer ratios, which means a pre-computed lookup table approach (sx[320] and sy[240]) eliminates all division from the inner loop.
- The framebuffer is row-major, stride = 320 pixels = 640 bytes.
- `rb->lcd_get_framebuffer(&width, &stride)` returns the live framebuffer pointer; write directly, then call `rb->lcd_update()` once per frame.

### Input — Clickwheel Specifics

- The iPod Classic scroll wheel fires `BUTTON_SCROLL_FWD` / `BUTTON_SCROLL_BACK` as delta events OR exposes an absolute position 0–95 (0 = top, increasing clockwise).
- Rockbox's `rb->button_status()` gives a snapshot of currently-held physical buttons.
- Zone mapping (see `sys_rockbox_gba.c` for the `WHEEL_*` constants):
  ```
  [84–95, 0–11]  = UP       [12–35]  = RIGHT
  [36–59]        = DOWN     [60–83]  = LEFT
  ```
- Physical buttons: `BUTTON_SELECT`→A, `BUTTON_PLAY`→B, `BUTTON_MENU`→Start, `BUTTON_LEFT`→Select, `BUTTON_RIGHT`→R.
- No L shoulder button on iPod hardware; Phase 3 should map a combo (e.g. scroll-click + MENU) to `GBA_KEY_L`.

### Audio Architecture

- igpSP produces 44100 Hz, 16-bit stereo at ~735 frames per GBA video frame.
- Rockbox mixer: `rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, cb, start, size)`.
- The callback `cb` runs in IRQ context — no malloc, no `rb->` calls, no blocking.
- Use a lock-free ring buffer (power-of-2 size, atomic head/tail) allocated via `sys_malloc()`.
- `sound_write()` (emulator thread) pushes; `pcm_callback()` (IRQ) pops.

### ARM Dynarec Safety Rule

**Never compile `src/cpu_threaded.c` as Thumb.**

The JIT writes raw ARM32 opcodes to `rom_translation_cache[]` / `ram_translation_cache[]` and branches into them.  If the code surrounding the branch is Thumb, the `BX` instruction sets bit 0 which the CPU interprets as a Thumb entry point, breaking every single translated instruction.

`igpsp.make` has a belt-and-suspenders special rule for `cpu_threaded.o` that explicitly removes `-mthumb` and adds `-marm`.  Do not remove this rule.

### igpSP Source References

- `src/cpu_threaded.c` — ARM JIT dynarec; translation caches defined here.
- `src/memory.c` — GBA memory map; `read_memory_*` / `write_memory_*` dispatch tables.
- `src/video.c` — GBA PPU; calls `update_scanline()` 160 times per frame; output is `psp_gu_vram_base` (pointer to 240×160 RGB565 buffer).
- `src/sound.c` — GBA APU; calls `sound_callback(length)` which is the platform hook.
- `src/main.c` — top-level `update_gba()` function; frame loop lives here.
- `common.h` — includes all other headers; `#ifdef ROCKBOX` guards needed throughout.

### Key igpSP → Rockbox Symbol Remapping

When integrating `src/` files, search for these iPod-specific symbols and replace with the Rockbox equivalents declared in `sys_rockbox_gba.h`:

| igpSP iPod symbol     | Replace with             |
|-----------------------|--------------------------|
| `open()`              | `sys_open()`             |
| `read()`              | `sys_read()`             |
| `write()`             | `sys_write()`            |
| `close()`             | `sys_close()`            |
| `lseek()`             | `sys_seek()`             |
| `malloc()`            | `sys_malloc()`           |
| `free()`              | (no-op — bump allocator) |
| `sound_callback(len)` | `sound_write(buf, len)`  |
| `usleep(us)`          | `sys_sleep_ms(us/1000)`  |
| `iopl()`, COP regs    | `sys_boost_cpu()`        |
| `SDL_*`               | (not used on iPod build) |

### Testing Without a Physical Device

`igpsp.rock` can be loaded in the Rockbox simulator (`tools/configure` → choose a simulator target).  The simulator will not have the iPod LCD or clickwheel, but it validates:
- Plugin links without undefined symbols.
- `plugin_start()` runs to the splash screen without crashing.
- Memory allocation reports a reasonable heap size.

For hardware testing, copy `igpsp.rock` to `/.rockbox/rocks/games/` on the iPod's disk.
