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

/*
 * GBA KEYINPUT register is active-low: 0 = pressed, 1 = released, 10 bits.
 * GBA_KEYINPUT_ALL_RELEASED (0x3FF) means every button is up.
 * input_read_keys() returns this format directly so igpSP can write it
 * straight into REG_P1 without any further inversion.
 */
#define GBA_KEYINPUT_ALL_RELEASED   0x3FFu

/* Cardinal zone identifiers for the wheel debounce state machine. */
#define ZONE_NONE   (-1)
#define ZONE_UP       0
#define ZONE_RIGHT    1
#define ZONE_DOWN     2
#define ZONE_LEFT     3

/* Current KEYINPUT register value written by input_poll(), read by
 * input_read_keys().  Initialised to all-released. */
static uint32_t current_keys    = GBA_KEYINPUT_ALL_RELEASED;

/* Previous wheel zone for zone-change debounce (mirrors SCROLL_MOD). */
static int      prev_wheel_zone = ZONE_NONE;

/* Previous hold-switch state — used for rising-edge detection so the
 * in-game menu appears exactly once per hold-switch engagement, not on
 * every poll tick while the switch remains locked. */
static bool     prev_hold       = false;

/*
 * exit_requested — set true by show_ingame_menu() when the user chooses
 * "Quit".  The emulator main loop in igpsp.c checks this flag each frame
 * and breaks out of the frame loop, returning cleanly to plugin_start().
 * Declared extern in sys_rockbox_gba.h so igpsp.c can read it.
 */
bool exit_requested = false;

/* -------------------------------------------------------------------------
 * wheel_pos_to_zone() — map absolute clickwheel position (0–95) to one of
 * four cardinal zones, or ZONE_NONE if the wheel is not being touched.
 *
 * North wraps through position 0:
 *   [84..95] and [0..11]  →  ZONE_UP
 *   [12..35]              →  ZONE_RIGHT
 *   [36..59]              →  ZONE_DOWN
 *   [60..83]              →  ZONE_LEFT
 * ------------------------------------------------------------------------- */
static int wheel_pos_to_zone(int pos)
{
    if (pos < 0)
        return ZONE_NONE;
    if (pos >= (int)WHEEL_UP_A_MIN || pos <= (int)WHEEL_UP_B_MAX)
        return ZONE_UP;
    if (pos <= (int)WHEEL_RIGHT_MAX)
        return ZONE_RIGHT;
    if (pos <= (int)WHEEL_DOWN_MAX)
        return ZONE_DOWN;
    return ZONE_LEFT;
}

/* -------------------------------------------------------------------------
 * show_ingame_menu() — pause emulation and present the in-game menu.
 *
 * Uses Rockbox's MENUITEM_STRINGLIST / rb->do_menu() API (same approach
 * Rockboy uses via do_user_menu()).  On Quit, sets exit_requested = true.
 *
 * Wheel events are re-enabled for menu navigation and suppressed again
 * when control returns to the emulator loop.
 * ------------------------------------------------------------------------- */
MENUITEM_STRINGLIST(ingame_menu_ex, "igpSP Menu", NULL,
    "Resume", "Save State", "Load State", "Quit");

static void show_ingame_menu(void)
{
    int sel    = 0;
    int result;

#ifdef HAVE_WHEEL_POSITION
    /* Allow the wheel to generate button-queue events for menu navigation. */
    rb->wheel_send_events(true);
#endif

    result = rb->do_menu(&ingame_menu_ex, &sel, NULL, false);

#ifdef HAVE_WHEEL_POSITION
    /* Back to absolute-position polling; suppress queue events. */
    rb->wheel_send_events(false);
#endif

    switch (result) {
        case 0: /* Resume — return to emulator immediately */
            break;
        case 1: /* Save State — Phase 6 */
            rb->splash(HZ, "Save State: coming in Phase 6");
            break;
        case 2: /* Load State — Phase 6 */
            rb->splash(HZ, "Load State: coming in Phase 6");
            break;
        case 3: /* Quit — signal the emulator main loop to exit */
            exit_requested = true;
            break;
        default: /* BACK / MENU pressed — treat as Resume */
            break;
    }
}

/* -------------------------------------------------------------------------
 * input_init() — initialise the input subsystem.
 *
 * Disables scroll-wheel event-queue injection so delta BUTTON_SCROLL_FWD /
 * BUTTON_SCROLL_BACK events do not accumulate in the button queue during
 * emulation.  Absolute wheel position is read via rb->wheel_status() in
 * input_poll() instead.
 * ------------------------------------------------------------------------- */
void input_init(void)
{
    current_keys    = GBA_KEYINPUT_ALL_RELEASED;
    prev_wheel_zone = ZONE_NONE;
    prev_hold       = false;
    exit_requested  = false;

#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(false);
#endif
}

/* -------------------------------------------------------------------------
 * input_poll() — snapshot all input sources and build the GBA KEYINPUT word.
 *
 * Called once per emulated frame (~60 Hz) by the emulator main loop.
 * Writes to current_keys in active-low KEYINPUT format (0 = pressed).
 *
 * Sources polled:
 *   1. rb->button_hold()  — hold switch → in-game menu (rising-edge only)
 *   2. rb->button_status() — physical click buttons → GBA face/shoulder keys
 *   3. rb->wheel_status() — absolute wheel position → GBA D-pad
 *
 * Button mapping:
 *   BUTTON_SELECT (centre click)  →  GBA A
 *   BUTTON_PLAY   (play/pause)    →  GBA B
 *   BUTTON_MENU   (menu)          →  GBA Start
 *   BUTTON_LEFT   (rewind)        →  GBA Select
 *   BUTTON_RIGHT  (forward)       →  GBA L trigger
 *
 * D-pad mapping (zone-change debounced):
 *   Wheel North [84–95,0–11]  →  GBA Up
 *   Wheel East  [12–35]       →  GBA Right
 *   Wheel South [36–59]       →  GBA Down
 *   Wheel West  [60–83]       →  GBA Left
 * ------------------------------------------------------------------------- */
void input_poll(void)
{
    uint32_t keys = GBA_KEYINPUT_ALL_RELEASED;   /* 0x3FF = all up          */
    bool     hold = rb->button_hold();

    /* -----------------------------------------------------------------------
     * Hold switch → in-game menu.
     *
     * Rising-edge detection: show the menu exactly once per hold engagement.
     * While the switch remains locked, keep all GBA keys released so the game
     * does not receive stray inputs.
     * --------------------------------------------------------------------- */
    if (hold) {
        if (!prev_hold) {
            /* First frame the switch is on — show the menu. */
            prev_hold = true;
            show_ingame_menu();
            prev_wheel_zone = ZONE_NONE;   /* clear stale wheel state        */
        }
        current_keys = GBA_KEYINPUT_ALL_RELEASED;
        return;
    }
    prev_hold = false;

    /* -----------------------------------------------------------------------
     * Physical click buttons → GBA keys.
     * rb->button_status() returns a non-blocking snapshot of currently-held
     * buttons (same as Rockboy's ev_poll() approach).
     * Clearing a bit in `keys` means that GBA key is pressed (active-low).
     * --------------------------------------------------------------------- */
    {
        int btn = rb->button_status();

        if (btn & BUTTON_SELECT) keys &= ~GBA_KEY_A;      /* centre → A     */
        if (btn & BUTTON_PLAY)   keys &= ~GBA_KEY_B;      /* play   → B     */
        if (btn & BUTTON_MENU)   keys &= ~GBA_KEY_START;  /* menu   → Start */
        if (btn & BUTTON_LEFT)   keys &= ~GBA_KEY_SELECT; /* rewind → Sel   */
        if (btn & BUTTON_RIGHT)  keys &= ~GBA_KEY_L;      /* fwd    → L     */
    }

    /* -----------------------------------------------------------------------
     * Clickwheel → GBA D-pad (absolute position, zone-change debounce).
     *
     * rb->wheel_status() returns the current absolute position 0–95 or -1.
     * We map to four cardinal zones and apply the direction every frame.
     *
     * Zone-change debounce (mirrors SCROLL_MOD from igpSP's ipod_input.h):
     * prev_wheel_zone is updated only when the zone changes.  This suppresses
     * jitter when a finger rests near a zone boundary and prevents a new D-pad
     * press being registered on every poll tick while the wheel is stationary.
     * --------------------------------------------------------------------- */
#ifdef HAVE_WHEEL_POSITION
    {
        int pos  = rb->wheel_status();        /* -1 or 0..95                */
        int zone = wheel_pos_to_zone(pos);

        if (zone != prev_wheel_zone)
            prev_wheel_zone = zone;           /* latch new zone on change   */

        switch (prev_wheel_zone) {
            case ZONE_UP:    keys &= ~GBA_KEY_UP;    break;
            case ZONE_RIGHT: keys &= ~GBA_KEY_RIGHT; break;
            case ZONE_DOWN:  keys &= ~GBA_KEY_DOWN;  break;
            case ZONE_LEFT:  keys &= ~GBA_KEY_LEFT;  break;
            default: break;   /* ZONE_NONE: finger lifted, no D-pad active  */
        }
    }
#endif /* HAVE_WHEEL_POSITION */

    current_keys = keys;
}

uint32_t input_read_keys(void)
{
    return current_keys;
}

/* -------------------------------------------------------------------------
 * input_exit() — release input resources.
 *
 * Restores scroll-wheel event delivery so Rockbox can use the wheel normally
 * after the plugin exits.
 * ------------------------------------------------------------------------- */
void input_exit(void)
{
#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(true);
#endif
    current_keys    = GBA_KEYINPUT_ALL_RELEASED;
    prev_wheel_zone = ZONE_NONE;
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
