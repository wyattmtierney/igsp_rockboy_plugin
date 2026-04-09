/* igpSP-Rockbox — GBA emulator Rockbox platform layer
 * platform/rockbox/sys_rockbox_gba.h
 *
 * This header is the ONLY interface between the igpSP emulator core (src/)
 * and the Rockbox firmware.  Nothing in src/ may include plugin.h directly;
 * all Rockbox API access must go through the functions declared here.
 *
 * Hardware target : iPod Classic 6G (Model A1238)
 *                   Samsung S5L8702, ARM926EJ-S ~216 MHz
 *                   Rockbox firmware, 320×240 RGB565 LCD
 *
 * Phase 1 — all function bodies are stubs with TODO Phase N: comments.
 */

#ifndef SYS_ROCKBOX_GBA_H
#define SYS_ROCKBOX_GBA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Rockbox API handle
 *
 * Declared extern here; DEFINED in igpsp.c via the PLUGIN_HEADER macro
 * (which expands to:  const struct plugin_api *rb DATA_ATTR; )
 * The firmware fills rb before calling plugin_start().
 *
 * sys_rockbox_gba.c includes plugin.h for the complete struct definition
 * before including this header, so the incomplete forward declaration below
 * is overridden by the full definition inside that translation unit.
 * ------------------------------------------------------------------------- */
struct plugin_api;                         /* incomplete forward declaration */
extern const struct plugin_api *rb;        /* defined by PLUGIN_HEADER      */

/* -------------------------------------------------------------------------
 * GBA display geometry
 * ------------------------------------------------------------------------- */
#define GBA_LCD_WIDTH    240
#define GBA_LCD_HEIGHT   160

/* iPod Classic 6G LCD — also set as -D flags in igpsp.make */
#ifndef LCD_WIDTH
#  define LCD_WIDTH  320
#endif
#ifndef LCD_HEIGHT
#  define LCD_HEIGHT 240
#endif

/* -------------------------------------------------------------------------
 * GBA keypad bitmask  (active-HIGH: 1 = pressed)
 *
 * Bit positions mirror the GBA KEYINPUT register (§4.1 ARM7TDMI GBA manual)
 * so igpSP's key_pad_update() can XOR directly without remapping.
 * ------------------------------------------------------------------------- */
#define GBA_KEY_A        (1u <<  0)
#define GBA_KEY_B        (1u <<  1)
#define GBA_KEY_SELECT   (1u <<  2)
#define GBA_KEY_START    (1u <<  3)
#define GBA_KEY_RIGHT    (1u <<  4)
#define GBA_KEY_LEFT     (1u <<  5)
#define GBA_KEY_UP       (1u <<  6)
#define GBA_KEY_DOWN     (1u <<  7)
#define GBA_KEY_R        (1u <<  8)
#define GBA_KEY_L        (1u <<  9)

/* =========================================================================
 * VIDEO SUBSYSTEM
 * ========================================================================= */

/**
 * vid_init() — initialise video subsystem.
 *
 * TODO Phase 2: configure Rockbox LCD for direct framebuffer access.
 *   - rb->lcd_set_backdrop(NULL) to prevent backdrop interference.
 *   - Obtain writable framebuffer via rb->lcd_get_framebuffer().
 *   - Clear to black; set up any viewport for the scaled output region.
 */
void vid_init(void);

/**
 * vid_update() — push one GBA frame to the iPod LCD.
 * @param src  240×160 RGB565 source buffer produced by igpSP's PPU renderer.
 *
 * TODO Phase 2: nearest-neighbour scale 240×160 → 320×240.
 *
 *   Scale factors:  x_scale = LCD_WIDTH  / GBA_LCD_WIDTH  = 4/3
 *                   y_scale = LCD_HEIGHT / GBA_LCD_HEIGHT = 3/2
 *
 *   Integer-only loop (no division in inner loop):
 *     dst_x = (src_x * LCD_WIDTH  + GBA_LCD_WIDTH/2)  / GBA_LCD_WIDTH
 *     dst_y = (src_y * LCD_HEIGHT + GBA_LCD_HEIGHT/2) / GBA_LCD_HEIGHT
 *
 *   Write directly to rb->lcd_framebuffer (fb16 *) to avoid the overhead
 *   of rb->lcd_bitmap_part(); then call rb->lcd_update() once per frame.
 *
 *   Phase 3+: consider bilinear filter or integer EPX/Scale2x for quality.
 */
void vid_update(const uint16_t *src);

/**
 * vid_exit() — release video resources.
 *
 * TODO Phase 2: restore Rockbox's default LCD viewport/backdrop if changed.
 */
void vid_exit(void);

/* =========================================================================
 * INPUT SUBSYSTEM
 * ========================================================================= */

/**
 * input_init() — initialise input subsystem.
 *
 * TODO Phase 3: enable scroll-wheel absolute-position reporting via
 *   rb->scroll_wheel_enable_int(true) if present in the plugin API version,
 *   or rely on BUTTON_SCROLL_FWD/BACK deltas with a running accumulator.
 *   Configure button repeat interval if needed.
 */
void input_init(void);

/**
 * input_poll() — drain the Rockbox button queue and update internal state.
 * Must be called exactly once per emulated frame (≈60 Hz) before
 * input_read_keys().
 *
 * TODO Phase 3: full implementation.
 *
 * Clickwheel zone map  (absolute position 0–95, 0 = top, clockwise):
 * ┌──────────────────────────────────────────────┐
 * │ Positions  84–95 + 0–11  →  GBA_KEY_UP       │ (wraps through 0)
 * │ Positions  12–35          →  GBA_KEY_RIGHT    │
 * │ Positions  36–59          →  GBA_KEY_DOWN     │
 * │ Positions  60–83          →  GBA_KEY_LEFT     │
 * └──────────────────────────────────────────────┘
 * Modelled on Rockboy's wheelmap[8] array but collapsed to 4 cardinal zones.
 *
 * Physical click buttons → GBA keys:
 *   BUTTON_SELECT  →  GBA_KEY_A
 *   BUTTON_PLAY    →  GBA_KEY_B
 *   BUTTON_MENU    →  GBA_KEY_START       (short press)
 *   BUTTON_LEFT    →  GBA_KEY_SELECT
 *   BUTTON_RIGHT   →  GBA_KEY_R
 *   BUTTON_MENU (long press) → request emulator exit / open in-emu menu
 *
 * Note: no physical L shoulder button on iPod; Phase 3 may map a scroll
 * click + MENU combo to GBA_KEY_L.
 */
void input_poll(void);

/**
 * input_read_keys() — return current GBA key state.
 * @return  OR-combination of GBA_KEY_* flags; 1 = pressed, 0 = released.
 *          Result is valid until next call to input_poll().
 */
uint32_t input_read_keys(void);

/**
 * input_exit() — release input resources.
 * Re-enables scroll-wheel event delivery for normal Rockbox operation.
 */
void input_exit(void);

/**
 * exit_requested — set true by the in-game "Quit" menu option.
 *
 * The emulator main loop in igpsp.c checks this flag once per frame and
 * breaks out when it is true, allowing plugin_start() to tear down cleanly.
 * Reset to false by input_init().
 */
extern bool exit_requested;

/* =========================================================================
 * AUDIO SUBSYSTEM
 * ========================================================================= */

/**
 * sound_init() — configure and start PCM output.
 * @param sample_rate  Desired rate in Hz; igpSP uses 44100.
 * @param channels     Channel count; igpSP produces 2 (stereo).
 *
 * TODO Phase 4: set up a lock-free ring buffer (size ≥ 2 × audio chunk),
 *   then kick off the Rockbox PCM mixer:
 *
 *     rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK,
 *                                 pcm_callback, NULL, 0);
 *
 *   The mixer calls pcm_callback() from IRQ context each time it needs more
 *   data; the callback should dequeue from the ring buffer filled by
 *   sound_write().
 *
 *   igpSP generates chunks of (sample_rate / 60) stereo frames ≈ 735 frames
 *   per video frame at 44100 Hz.
 */
void sound_init(int sample_rate, int channels);

/**
 * sound_write() — enqueue a buffer of mixed PCM samples.
 * @param buf  Interleaved signed 16-bit stereo samples (L0,R0,L1,R1,…).
 * @param len  Number of stereo *frames* (NOT bytes; bytes = len × 4).
 *
 * TODO Phase 4: push into the ring buffer.  If the buffer is full, either
 *   spin-wait (preferred — keeps audio/video in sync) or drop and log a
 *   buffer-underrun counter.
 */
void sound_write(const int16_t *buf, int len);

/**
 * sound_set_volume() — adjust playback volume.
 * @param vol  0 (silent) … 100 (maximum).
 *
 * TODO Phase 4:
 *   rb->sound_set(SOUND_VOLUME, vol * global_settings.volume / 100);
 *   or use rb->mixer_channel_set_amplitude().
 */
void sound_set_volume(int vol);

/**
 * sound_exit() — stop PCM playback and free audio resources.
 *
 * TODO Phase 4:
 *   rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
 *   Flush and destroy ring buffer.
 */
void sound_exit(void);

/* =========================================================================
 * CPU / TIMING UTILITIES
 * ========================================================================= */

/**
 * sys_boost_cpu() — request maximum CPU clock.
 * Wraps rb->cpu_boost(true).  Must be paired with sys_unboost_cpu().
 * The S5L8702 can run at ~216 MHz boosted vs ~54 MHz unboosted.
 */
void sys_boost_cpu(void);

/**
 * sys_unboost_cpu() — release CPU boost.
 * Wraps rb->cpu_boost(false).  Always call before returning from
 * plugin_start() to avoid leaving the SoC stuck at high clock.
 */
void sys_unboost_cpu(void);

/**
 * sys_sleep_ms() — sleep for approximately N milliseconds.
 * @param ms  Duration in milliseconds (Rockbox tick resolution = 10 ms).
 * Uses rb->sleep(ms * HZ / 1000) where HZ = 100.
 */
void sys_sleep_ms(int ms);

/**
 * sys_get_ticks() — monotonic tick counter (10 ms resolution).
 * @return  Value of *rb->current_tick cast to uint32_t.
 *          Wraps every ~497 days; igpSP frame limiter handles the wrap.
 */
uint32_t sys_get_ticks(void);

/* =========================================================================
 * FILESYSTEM WRAPPERS
 *
 * Thin shims over rb->open / rb->read / rb->write / rb->close / rb->lseek.
 * The Rockbox file API is nearly POSIX-compatible; flags (O_RDONLY etc.) are
 * the same.  These wrappers exist so src/ files never include plugin.h.
 * ========================================================================= */

/**
 * sys_open() — open a file.
 * @param path   Absolute path on the Rockbox VFS (e.g. "/gba/rom.gba").
 * @param flags  POSIX open flags: O_RDONLY, O_WRONLY|O_CREAT, etc.
 * @return  File descriptor ≥ 0 on success, negative on error.
 */
int  sys_open(const char *path, int flags);

/**
 * sys_read() — read from file descriptor.
 * @return  Bytes read, 0 = EOF, negative = error.
 */
int  sys_read(int fd, void *buf, size_t len);

/**
 * sys_write() — write to file descriptor.
 * @return  Bytes written, negative = error.
 */
int  sys_write(int fd, const void *buf, size_t len);

/** sys_close() — close file descriptor. */
int  sys_close(int fd);

/**
 * sys_seek() — reposition file offset.
 * @param whence  SEEK_SET / SEEK_CUR / SEEK_END (POSIX values).
 * @return  New absolute offset, negative = error.
 */
int  sys_seek(int fd, int offset, int whence);

/* =========================================================================
 * MEMORY SUBSYSTEM
 *
 * Rockbox plugins cannot use the system heap (malloc/free) unless the
 * platform exports them — the iPod firmware does not.  Instead, we claim
 * the entire Rockbox audio buffer as a bump-allocator arena.  The GBA
 * emulator core needs ~6 MB minimum (EWRAM 256 KB, IWRAM 32 KB, VRAM 96 KB,
 * ROM up to 32 MB, translation caches, etc.) so the 64 MB iPod RAM is
 * plenty when audio is surrendered to us.
 *
 * There is NO free().  All allocations live for the plugin's lifetime.
 * ========================================================================= */

/**
 * sys_mem_init() — claim the audio buffer as the plugin heap.
 * Must be the FIRST platform call after plugin_start() stores rb.
 * Subsequent calls are idempotent (only the first has any effect).
 *
 * TODO Phase 2: add minimum-size check; rb->splash() + return error code
 *   if the audio buffer is smaller than the emulator's minimum requirement
 *   (~8 MB for a typical GBA ROM + caches).
 */
void  sys_mem_init(void);

/**
 * sys_malloc() — bump-allocate @size bytes from the plugin heap.
 * Alignment is always 4 bytes.
 * @return  Pointer to allocated region.  Never NULL within the plugin —
 *          out-of-memory causes a fatal rb->splash() + plugin exit.
 *
 * TODO Phase 2: add OOM detection; currently silently overflows.
 */
void *sys_malloc(size_t size);

/**
 * sys_heap_remaining() — bytes still available in the bump allocator.
 * Useful for diagnostic splashes during early integration.
 */
size_t sys_heap_remaining(void);

#endif /* SYS_ROCKBOX_GBA_H */
