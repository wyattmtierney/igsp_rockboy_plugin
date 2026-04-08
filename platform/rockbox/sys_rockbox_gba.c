/* igpSP-Rockbox — GBA emulator Rockbox platform layer
 * platform/rockbox/sys_rockbox_gba.c
 *
 * Stub implementations of every igpSP platform call.
 * This is the ONE file in the project that includes plugin.h; all other
 * source files must access Rockbox services only through the functions
 * declared in sys_rockbox_gba.h.
 *
 * Hardware target : iPod Classic 6G (Model A1238)
 *                   Samsung S5L8702, ARM926EJ-S ~216 MHz
 *                   Rockbox firmware, 320×240 RGB565 LCD
 *
 * Phase 1 — stubs only.  Every stub has a "TODO Phase N:" comment that
 * describes precisely what the real implementation needs to do.
 */

/* plugin.h must come BEFORE sys_rockbox_gba.h so that the complete
 * definition of struct plugin_api is visible when the compiler processes
 * the extern declaration in the header. */
#include "plugin.h"
#include "sys_rockbox_gba.h"

/* =========================================================================
 * MEMORY SUBSYSTEM
 * ========================================================================= */

static uint8_t *heap_base = NULL;   /* start of audio-buffer arena          */
static uint8_t *heap_ptr  = NULL;   /* current bump pointer                 */
static size_t   heap_size = 0;      /* total bytes claimed from Rockbox     */

void sys_mem_init(void)
{
    if (heap_base)
        return; /* idempotent — only first call has effect */

    heap_base = (uint8_t *)rb->plugin_get_audio_buffer(&heap_size);
    heap_ptr  = heap_base;

    /* TODO Phase 2: enforce minimum heap requirement.
     *   GBA minimum layout:
     *     EWRAM  256 KB   = 0x040000
     *     IWRAM   32 KB   = 0x008000
     *     VRAM    96 KB   = 0x018000
     *     Palette  1 KB   = 0x000400
     *     OAM      1 KB   = 0x000400
     *     ROM (max 32 MB) = 0x2000000  — only if we pre-load entire ROM
     *     Translation cache (ROM+RAM+BIOS) ~2 MB typical
     *   Total minimum without pre-loaded ROM: ~8 MB
     *   If heap_size < 8 MB, splash an error and return an error code.
     */
    rb->splashf(HZ / 2, "igpSP heap: %u KB", (unsigned)(heap_size / 1024));
}

void *sys_malloc(size_t size)
{
    void *ptr;

    /* 4-byte align every allocation */
    size = (size + 3u) & ~3u;

    /* TODO Phase 2: OOM detection.
     *   if ((heap_ptr + size) > (heap_base + heap_size)) {
     *       rb->splash(HZ * 3, "igpSP: OUT OF MEMORY");
     *       rb->plugin_tsr(NULL);  // or longjmp to plugin_start cleanup
     *   }
     */
    ptr       = heap_ptr;
    heap_ptr += size;
    return ptr;
}

size_t sys_heap_remaining(void)
{
    if (!heap_base)
        return 0;
    return (size_t)(heap_base + heap_size - heap_ptr);
}

/* =========================================================================
 * VIDEO SUBSYSTEM
 * ========================================================================= */

/*
 * Nearest-neighbour scaling lookup tables.
 * Populated once in vid_init(); no division in the hot blit path.
 *
 *   scale_x[lcd_col] → GBA source column  (0 … GBA_LCD_WIDTH-1  = 0..239)
 *   scale_y[lcd_row] → GBA source row     (0 … GBA_LCD_HEIGHT-1 = 0..159)
 *
 * Both ranges fit in uint8_t, saving cache footprint vs int arrays.
 */
static uint8_t scale_x[LCD_WIDTH];    /* 320 bytes */
static uint8_t scale_y[LCD_HEIGHT];   /* 240 bytes */

/* Rockbox live framebuffer and row stride (in pixels, not bytes). */
static fb_data *lcd_framebuf = NULL;
static int      lcd_stride   = 0;

/*
 * Frameskip support.
 *
 *   frameskip == 0  → render every frame (default).
 *   frameskip == N  → render 1 in every N+1 frames; rb->lcd_update() is
 *                     skipped on the N intermediate frames.
 *
 * igpSP's main loop sets frameskip before the emulation loop; vid_update()
 * checks it each call without any further initialisation required.
 */
int        frameskip          = 0;   /* externally settable; 0 = no skip    */
static int frame_skip_counter = 0;   /* private countdown; reset each frame */

void vid_init(void)
{
    int w, s;
    int i;

    /* Remove any backdrop that might bleed through direct framebuffer writes. */
    rb->lcd_set_backdrop(NULL);

    /* Obtain a direct pointer to the live Rockbox LCD framebuffer.
     * lcd_stride is the row pitch in pixels (320 on the iPod Classic 6G). */
    lcd_framebuf = rb->lcd_get_framebuffer(&w, &s);
    lcd_stride   = s;

    /* Full-screen viewport so rb->lcd_update() flushes the whole display. */
    {
        struct viewport vp;
        rb->viewport_set_defaults(&vp, SCREEN_MAIN);
        rb->lcd_set_viewport(&vp);
    }

    /* Pre-compute nearest-neighbour source-pixel lookup tables.
     *
     *   scale_x[dx] = dx * GBA_LCD_WIDTH  / LCD_WIDTH   (integer floor)
     *   scale_y[dy] = dy * GBA_LCD_HEIGHT / LCD_HEIGHT  (integer floor)
     *
     * All integer division happens here at init time — zero division in
     * the hot blit path.
     */
    for (i = 0; i < LCD_WIDTH; i++)
        scale_x[i] = (uint8_t)((i * GBA_LCD_WIDTH) / LCD_WIDTH);

    for (i = 0; i < LCD_HEIGHT; i++)
        scale_y[i] = (uint8_t)((i * GBA_LCD_HEIGHT) / LCD_HEIGHT);

    frame_skip_counter = 0;

    /* Clear to black and flush once to give a clean starting state. */
    rb->lcd_clear_display();
    rb->lcd_update();
}

/*
 * blit_frame_c() — nearest-neighbour scale 240×160 GBA → 320×240 LCD, C impl.
 *
 * *** Phase 2b ARM assembly replacement target ***
 *
 * This function contains the entire inner blit loop.  When replacing with
 * hand-written ARM asm, keep the signature and calling convention identical:
 *   - src: pointer to 240×160 RGB565 pixels, row-major, stride=GBA_LCD_WIDTH
 *   - dst: pointer to LCD framebuffer, row-major, stride=lcd_stride pixels
 *   - scale_x[] and scale_y[] are module-level statics visible to the asm
 *
 * Constraints (must be preserved in the asm version):
 *   - No floating point.
 *   - No division.
 *   - No branches inside the pixel-level inner loop.
 *   - Both src and dst are 16-bit aligned; word/dword loads are safe.
 */
static void blit_frame_c(const uint16_t * restrict src,
                          fb_data        * restrict dst)
{
    int dy, dx;

    for (dy = 0; dy < LCD_HEIGHT; dy++) {
        const uint16_t *src_row = src + (int)scale_y[dy] * GBA_LCD_WIDTH;
        fb_data        *dst_row = dst + dy * lcd_stride;

        for (dx = 0; dx < LCD_WIDTH; dx++)
            dst_row[dx] = (fb_data)src_row[(int)scale_x[dx]];
    }
}

void vid_update(const uint16_t *src)
{
    /* Frameskip: skip blit + lcd_update on intermediate frames.
     * The emulator core calls vid_update() every frame regardless. */
    if (frameskip > 0) {
        if (frame_skip_counter < frameskip) {
            frame_skip_counter++;
            return;
        }
        frame_skip_counter = 0;
    }

    blit_frame_c(src, lcd_framebuf);
    rb->lcd_update();
}

void vid_exit(void)
{
    /* Restore default viewport, clear display. */
    rb->lcd_set_viewport(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
    lcd_framebuf = NULL;
}

/* =========================================================================
 * INPUT SUBSYSTEM
 * ========================================================================= */

/*
 * Clickwheel zone boundaries (absolute position 0–95, 0 = top, CW):
 *
 *         0 (top)
 *      84 │ 11
 *   ───────┼───────
 *  LEFT    │    RIGHT
 *   (60-83)│  (12-35)
 *   ───────┼───────
 *      59 │ 36
 *        48 (bottom / 36-59)
 *
 * Zone [84–95] + [0–11] = UP  (wraps through 0; checked with two ranges)
 * Zone [12–35]           = RIGHT
 * Zone [36–59]           = DOWN
 * Zone [60–83]           = LEFT
 */
#define WHEEL_POSITIONS     96u      /* full revolution = 96 clicks          */
#define WHEEL_UP_A_MIN      84u      /* upper arc wraps through 0            */
#define WHEEL_UP_A_MAX      95u
#define WHEEL_UP_B_MIN       0u
#define WHEEL_UP_B_MAX      11u
#define WHEEL_RIGHT_MIN     12u
#define WHEEL_RIGHT_MAX     35u
#define WHEEL_DOWN_MIN      36u
#define WHEEL_DOWN_MAX      59u
#define WHEEL_LEFT_MIN      60u
#define WHEEL_LEFT_MAX      83u

static uint32_t current_keys = 0;   /* bitmask of GBA_KEY_* flags           */

void input_init(void)
{
    /* TODO Phase 3: enable hardware scroll-wheel interrupt for lower latency.
     *   On iPod Classic, wheel position is read from the PortC GPIO block.
     *   Rockbox may expose rb->scroll_wheel_enable_int() — check plugin API
     *   version before calling.
     *
     *   If the API has no such call, rely on BUTTON_SCROLL_FWD /
     *   BUTTON_SCROLL_BACK delta events and maintain a running accumulator:
     *     int wheel_pos = 0;
     *     // On SCROLL_FWD event: wheel_pos = (wheel_pos + 1) % 96;
     *     // On SCROLL_BACK event: wheel_pos = (wheel_pos + 95) % 96;
     */
    current_keys = 0;
}

void input_poll(void)
{
    /* TODO Phase 3: full implementation.
     *
     *  uint32_t keys = 0;
     *
     *  // --- Physical click buttons ---
     *  int btn = rb->button_status();  // non-blocking snapshot
     *
     *  if (btn & BUTTON_SELECT) keys |= GBA_KEY_A;
     *  if (btn & BUTTON_PLAY)   keys |= GBA_KEY_B;
     *  if (btn & BUTTON_MENU)   keys |= GBA_KEY_START;
     *  if (btn & BUTTON_LEFT)   keys |= GBA_KEY_SELECT;
     *  if (btn & BUTTON_RIGHT)  keys |= GBA_KEY_R;
     *
     *  // --- Scroll wheel → D-pad ---
     *  //  Option A: absolute position (preferred)
     *  //    int pos = rb->wheel_status();  // 0-95
     *  //
     *  //  Option B: accumulate delta events from the button queue.
     *  //    Drain queue with rb->button_get(false) in a loop until 0.
     *
     *  //  Map position to cardinal direction:
     *  //    if (pos >= WHEEL_UP_A_MIN || pos <= WHEEL_UP_B_MAX)
     *  //        keys |= GBA_KEY_UP;
     *  //    else if (pos <= WHEEL_RIGHT_MAX) keys |= GBA_KEY_RIGHT;
     *  //    else if (pos <= WHEEL_DOWN_MAX)  keys |= GBA_KEY_DOWN;
     *  //    else                             keys |= GBA_KEY_LEFT;
     *
     *  // --- Long MENU press → emulator exit ---
     *  //    Track a press-duration counter here; if MENU held > 1 s,
     *  //    set a global exit_requested flag checked by the main loop.
     *
     *  current_keys = keys;
     */
    current_keys = 0; /* stub: no keys pressed — placeholder screen stays up */
}

uint32_t input_read_keys(void)
{
    return current_keys;
}

void input_exit(void)
{
    /* TODO Phase 3: disable scroll-wheel interrupt if we enabled it. */
    current_keys = 0;
}

/* =========================================================================
 * AUDIO SUBSYSTEM
 * ========================================================================= */

/*
 * PCM ring buffer — filled by sound_write() (emulator thread) and
 * drained by pcm_callback() (Rockbox mixer IRQ context).
 *
 * TODO Phase 4: allocate ring buffer from sys_malloc() in sound_init(),
 *   use atomic head/tail indices for lock-free operation.  The GBA APU
 *   produces ~735 stereo frames per video frame @ 44100 Hz / 60 fps.
 *   A 4 × that = ~2940-frame (~11760-byte) ring buffer prevents starvation.
 */

/* Called by the Rockbox PCM mixer when it needs another chunk of audio.
 * Runs in interrupt context — must be fast and lock-free.
 *
 * TODO Phase 4: dequeue next contiguous chunk from the ring buffer.
 *   *start = ring_read_ptr;
 *   *size  = contiguous_bytes_available;
 *   advance ring_read_ptr by *size after the mixer consumes it.
 *
 *   If the ring buffer is empty (underrun): set *start/*size to a small
 *   silence buffer to avoid glitches; log the underrun count.
 */
static void pcm_callback(const void **start, size_t *size)
{
    /* TODO Phase 4: replace with real ring buffer drain. */
    *start = NULL;
    *size  = 0;
}

void sound_init(int sample_rate, int channels)
{
    /* TODO Phase 4: initialise audio subsystem.
     *
     *   1. Allocate ring buffer from sys_malloc():
     *        size_t buf_frames = (sample_rate / 60) * 4;
     *        size_t buf_bytes  = buf_frames * channels * sizeof(int16_t);
     *        ring_buf = sys_malloc(buf_bytes);
     *
     *   2. Start the Rockbox PCM mixer channel:
     *        rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK,
     *                                    pcm_callback, NULL, 0);
     *      The mixer will immediately call pcm_callback() for the first
     *      chunk; it will return silence until sound_write() populates the
     *      ring buffer.
     *
     *   3. If rb->mixer_channel_play_data() is not available in this plugin
     *      API version, fall back to rb->pcm_play_data().
     *
     * Note: igpSP hardcodes 44100 Hz stereo.  The S5L8702 DAC supports this
     * natively so no resampling is needed.
     */
    (void)sample_rate;
    (void)channels;
    (void)pcm_callback; /* suppress -Wunused-function for Phase 1 stub */
}

void sound_write(const int16_t *buf, int len)
{
    /* TODO Phase 4: enqueue @len stereo frames into the ring buffer.
     *   Bytes = len * 2 channels * sizeof(int16_t) = len * 4.
     *
     *   Strategy if ring buffer is full:
     *     Spin-wait (busy loop) for a small number of cycles — the mixer
     *     IRQ will drain space shortly.  Cap spin at ~2 ms; if still full
     *     after cap, drop the oldest data (prefer low latency over no drops).
     */
    (void)buf;
    (void)len;
}

void sound_set_volume(int vol)
{
    /* TODO Phase 4: map 0–100 percentage to Rockbox volume scale.
     *   int rb_vol = vol * rb->sound_max(SOUND_VOLUME) / 100;
     *   rb->sound_set(SOUND_VOLUME, rb_vol);
     *   Or, if using mixer amplitude:
     *   rb->mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK,
     *       MIX_AMP_UNITY * vol / 100);
     */
    (void)vol;
}

void sound_exit(void)
{
    /* TODO Phase 4: stop PCM output cleanly.
     *   rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
     *   or rb->pcm_play_stop() for older API.
     *   No need to free ring buffer — sys_malloc() arena is dropped with
     *   the plugin anyway.
     */
}

/* =========================================================================
 * CPU / TIMING UTILITIES
 * ========================================================================= */

void sys_boost_cpu(void)
{
    /* S5L8702: ~54 MHz unboosted → ~216 MHz boosted.
     * Rockbox's cpu_boost() adjusts the PLL divider via the SoC PMU. */
    rb->cpu_boost(true);
}

void sys_unboost_cpu(void)
{
    rb->cpu_boost(false);
}

void sys_sleep_ms(int ms)
{
    /* HZ = 100 ticks/second in Rockbox (10 ms per tick).
     * rb->sleep(0) yields; rb->sleep(1) waits at least one tick (10 ms). */
    rb->sleep((ms * HZ + 999) / 1000); /* round up to avoid sleeping 0 */
}

uint32_t sys_get_ticks(void)
{
    return (uint32_t)(*rb->current_tick);
}

/* =========================================================================
 * FILESYSTEM WRAPPERS
 * ========================================================================= */

int sys_open(const char *path, int flags)
{
    /* Rockbox open() takes (path, oflag, mode); mode is only used for
     * O_CREAT.  Pass 0644 as a safe default. */
    return rb->open(path, flags, 0644);
}

int sys_read(int fd, void *buf, size_t len)
{
    return (int)rb->read(fd, buf, (size_t)len);
}

int sys_write(int fd, const void *buf, size_t len)
{
    return (int)rb->write(fd, buf, (size_t)len);
}

int sys_close(int fd)
{
    return rb->close(fd);
}

int sys_seek(int fd, int offset, int whence)
{
    return (int)rb->lseek(fd, (off_t)offset, whence);
}
