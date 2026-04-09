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

/* Forward declaration — rb_file_pool_init() is defined in the FILE* section
 * below but called from sys_mem_init() which appears first in this file. */
void rb_file_pool_init(void);

void sys_mem_init(void)
{
    if (heap_base)
        return; /* idempotent — only first call has effect */

    heap_base = (uint8_t *)rb->plugin_get_audio_buffer(&heap_size);
    heap_ptr  = heap_base;

    /* Minimum heap: ROM(16MB) + caches(~2.5MB) + GBA regions(~1MB) = ~20 MB.
     * The iPod Classic 6G audio buffer is ~60 MB so this always passes.
     * Splash a fatal error on smaller/simulated targets to catch OOM early. */
#define IGPSP_MIN_HEAP  (20u * 1024u * 1024u)   /* 20 MB */
    if (heap_size < IGPSP_MIN_HEAP) {
        rb->splashf(HZ * 4,
                    "igpSP: heap too small (%u KB, need %u KB)",
                    (unsigned)(heap_size / 1024),
                    (unsigned)(IGPSP_MIN_HEAP / 1024));
        /* Leave heap_base set so callers can detect init ran; sys_malloc
         * will OOM-splash on the first overflowing allocation. */
    }
#ifdef IGPSP_DEBUG
    rb->splashf(HZ / 2, "igpSP heap: %u KB", (unsigned)(heap_size / 1024));
#endif

    /* Initialise the FILE* slot pool — must happen after heap is ready. */
    rb_file_pool_init();
}

void *sys_malloc(size_t size)
{
    void *ptr;

    /* 4-byte align every allocation */
    size = (size + 3u) & ~3u;

    if ((heap_ptr + size) > (heap_base + heap_size)) {
        rb->splashf(HZ * 4,
                    "igpSP OOM: need %u KB, have %u KB",
                    (unsigned)(size / 1024),
                    (unsigned)(sys_heap_remaining() / 1024));
        /* Crash-safe: return a valid pointer to the last valid byte so the
         * emulator corrupts its own heap rather than the Rockbox stack.
         * The corrupted state will manifest as a garbled screen or hang,
         * which is recoverable by holding MENU to exit via the in-game menu. */
        return heap_base;   /* degenerate fallback — do not return NULL */
    }
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
 * AUDIO SUBSYSTEM — Phase 4
 * =========================================================================
 *
 * Architecture
 * ─────────────
 *   Emulator thread (sound_write) → ring buffer → pcm_callback (IRQ) → DAC
 *
 * The GBA APU (src/sound.c) produces 16-bit stereo PCM at SOUND_SAMPLE_RATE
 * Hz.  At 22050 Hz / 60 fps the APU delivers ≈ 368 stereo frames per GBA
 * video frame.  sound_write() pushes these into a lock-free ring buffer;
 * the Rockbox PCM mixer drains the buffer via pcm_callback() each time the
 * DMA engine needs its next audio chunk.
 *
 * Ring buffer is SPSC (Single-Producer Single-Consumer):
 *   Producer — sound_write()  : emulator thread, the ONLY writer of ring_head
 *   Consumer — pcm_callback() : IRQ context,     the ONLY writer of ring_tail
 *
 * Indices are volatile uint32_t and grow without bound.  Buffer position is
 * obtained by masking: ring_buf[index & RING_BUF_MASK].  Unsigned subtraction
 * handles the inevitable 32-bit wrap-around transparently.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * COP Replacement — why no ipod_cop.c equivalent is needed
 * ─────────────────────────────────────────────────────────────────────────
 * igpSP's ipod_cop.c targeted the PortalPlayer PP502x dual-core SoC present
 * in iPods up to the 5th generation.  It accessed three memory-mapped
 * registers at fixed addresses (COP_HANDLER 0x4001501C, COP_STATUS 0x40015020,
 * COP_CONTROL 0x60007004) to schedule screen-synchronisation work on the
 * PP502x's second ARM core, freeing the main CPU for emulation.
 *
 * The iPod Classic 6G uses the Samsung S5L8702, an entirely different SoC.
 * The PP502x COP registers do not exist on the S5L8702; writing to those
 * addresses would corrupt peripheral registers or hang the device.  The COP
 * code must NOT be ported.
 *
 * On the PP502x, audio went through the OSS /dev/dsp kernel driver via
 * blocking write() calls on the MAIN core — the COP handled VIDEO only.
 * That blocking write model is replaced wholesale here: Rockbox's DMA-driven
 * PCM subsystem fires an interrupt when the DMA engine needs the next buffer.
 * pcm_callback() runs in that IRQ context, achieving the same asynchronous
 * audio delivery without any second-core management.  The emulator runs
 * uninterrupted on the single S5L8702 ARM926EJ-S core while audio is handled
 * by the hardware DMA controller and the Wolfson WM8758 DAC.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Sample rate — 22050 Hz, matching igpSP's original iPod Linux default.
 *
 * The iPodLinux port ran the APU at 22050 Hz MONO because the OSS /dev/dsp
 * driver had high per-write overhead and the PP502x COP pipeline stalled under
 * stereo load at higher rates.  None of those constraints apply here:
 *   • Rockbox uses DMA-driven PCM — the CPU is not involved in sample transfer.
 *   • The Wolfson WM8758 DAC on the 6G supports 22050 Hz natively (no SRC).
 *   • The GBA APU's highest-frequency content is ≈ 18 kHz, well within the
 *     22050 Hz Nyquist limit of 11025 Hz.
 *   • Stereo callback overhead is identical to mono on the S5L8702 (DMA width
 *     is the same; the CPU copies nothing during normal playback).
 *
 * Performance tuning knob: if the emulator cannot sustain 60 fps due to
 * audio overhead, reduce SOUND_SAMPLE_RATE to 11025 (SAMPR_11).  The ring
 * buffer will drain 2× more slowly, giving the emulator more headroom.
 * Change the value here AND the rb->mixer_set_frequency() call in sound_init().
 */
#define SOUND_SAMPLE_RATE    22050

/*
 * Ring buffer depth — SOUND_BUFFER_FRAMES stereo frames.
 *
 * At 22050 Hz / 60 fps the APU produces ≈ 368 stereo frames per video frame.
 * 4096 frames ≈ 11 video frames of headroom, which prevents underruns across
 * brief emulation hiccups without introducing perceptible latency.
 * Memory cost: 4096 × 2 × sizeof(int16_t) = 16384 bytes.
 *
 * MUST be a power of two — the mask trick (& RING_BUF_MASK) replaces modulo.
 */
#define SOUND_BUFFER_FRAMES  4096
#define RING_BUF_SAMPLES     (SOUND_BUFFER_FRAMES * 2)       /* × 2 = stereo */
#define RING_BUF_MASK        ((uint32_t)(RING_BUF_SAMPLES - 1))

/*
 * DMA output chunk — int16_t samples provided to the mixer per callback.
 *
 * 1024 samples = 512 stereo frames ≈ 23 ms at 22050 Hz.  Small enough for
 * low latency; large enough to amortise callback overhead across many frames.
 * Rockbox requires the DMA buffer size to be a multiple of 4 bytes;
 * 1024 × sizeof(int16_t) = 2048 bytes satisfies that requirement.
 */
#define PCM_CHUNK_SAMPLES    1024
#define PCM_CHUNK_BYTES      ((int)(PCM_CHUNK_SAMPLES * sizeof(int16_t)))

/* Ring buffer — allocated from the plugin heap in sound_init(). */
static int16_t          *ring_buf    = NULL;

/*
 * SPSC ring buffer indices.
 *
 * ring_head — written ONLY by sound_write() (emulator thread).
 * ring_tail — written ONLY by pcm_callback() (IRQ context).
 *
 * Both are read by both sides; volatile prevents the compiler from caching
 * either in a register across the producer/consumer boundary.
 *
 * Indices grow without bound (uint32_t).  Unsigned subtraction gives the
 * correct available/free count even after a 32-bit wrap-around, as long as
 * the instantaneous distance never exceeds 2^31 — guaranteed here since the
 * buffer is only 8192 samples deep.
 */
static volatile uint32_t ring_head   = 0;    /* write index — emulator thread */
static volatile uint32_t ring_tail   = 0;    /* read  index — pcm_callback    */

/*
 * DMA output buffer — pcm_callback() copies ring data here, then hands the
 * pointer to the Rockbox mixer via *start.  The buffer must remain valid
 * between callback invocations because the DMA engine reads it asynchronously
 * after the callback returns.
 * Allocated from the plugin heap in sound_init().
 */
static int16_t          *pcm_dma_buf = NULL;

/*
 * Silence fallback — returned by pcm_callback() on underrun so the DAC keeps
 * running without glitches.  Zeroed by sound_init(); small enough to live in
 * the BSS segment (Rockbox zeroes plugin BSS at load time).
 */
static int16_t           pcm_silence[PCM_CHUNK_SAMPLES];

/* Diagnostic counter — incremented on each underrun for debugging. */
static volatile uint32_t pcm_underruns = 0;

/* -------------------------------------------------------------------------
 * pcm_callback() — Rockbox PCM mixer get_more callback.
 *
 * Invoked from interrupt context by the Rockbox DMA / mixer subsystem
 * whenever the DMA engine has consumed the previous buffer and needs another
 * chunk.  Sets *start and *size to point at the next PCM_CHUNK_BYTES of
 * audio for the DMA engine to transfer to the WM8758 DAC.
 *
 * Hard constraints (enforced by the Rockbox mixer contract):
 *   • MUST NOT block, sleep, yield, or call rb->sleep() / rb->yield().
 *   • MUST NOT allocate or free memory.
 *   • MUST NOT acquire any mutex or semaphore held by the main thread.
 *   • MUST complete quickly — runs inside the audio IRQ handler.
 *
 * The function reads ring_head (producer index) and writes ring_tail
 * (consumer index).  No other execution context writes ring_tail, so no
 * lock is needed for the normal drain path.
 * ------------------------------------------------------------------------- */
static void pcm_callback(const void **start, size_t *size)
{
    uint32_t head  = ring_head;     /* snapshot — producer writes this        */
    uint32_t tail  = ring_tail;     /* our own index — only we write this     */
    uint32_t avail = head - tail;   /* unsigned wrap gives correct count      */
    uint32_t to_copy;
    uint32_t idx;

    if (avail == 0) {
        /* Ring buffer empty — underrun.  Return silence so the DAC stays fed
         * and does not click.  The emulator will refill within one frame. */
        pcm_underruns++;
        *start = pcm_silence;
        *size  = (size_t)PCM_CHUNK_BYTES;
        return;
    }

    /* Drain up to one full DMA chunk worth of samples. */
    to_copy = (avail < (uint32_t)PCM_CHUNK_SAMPLES)
              ? avail
              : (uint32_t)PCM_CHUNK_SAMPLES;

    idx = tail & RING_BUF_MASK;     /* buffer position of first sample to read */

    if (idx + to_copy <= (uint32_t)RING_BUF_SAMPLES) {
        /* Contiguous region — single copy. */
        rb->memcpy(pcm_dma_buf, ring_buf + idx,
                   to_copy * sizeof(int16_t));
    } else {
        /* Data wraps around the end of the ring buffer — two copies. */
        uint32_t first = (uint32_t)RING_BUF_SAMPLES - idx;
        rb->memcpy(pcm_dma_buf,         ring_buf + idx,
                   first * sizeof(int16_t));
        rb->memcpy(pcm_dma_buf + first, ring_buf,
                   (to_copy - first) * sizeof(int16_t));
    }

    /* Silence-pad the tail of the DMA buffer if to_copy < PCM_CHUNK_SAMPLES
     * (partial chunk at the very end of a session or after an underrun). */
    if (to_copy < (uint32_t)PCM_CHUNK_SAMPLES) {
        rb->memset(pcm_dma_buf + to_copy, 0,
                   (PCM_CHUNK_SAMPLES - to_copy) * sizeof(int16_t));
    }

    /* Advance the consumer index.  This is the only write to ring_tail;
     * the store is visible to sound_write() on the next sample check. */
    ring_tail = tail + to_copy;

    *start = pcm_dma_buf;
    *size  = (size_t)PCM_CHUNK_BYTES;
}

void sound_init(int sample_rate, int channels)
{
    (void)sample_rate;  /* fixed at SOUND_SAMPLE_RATE — see comment above     */
    (void)channels;     /* always stereo — RING_BUF_SAMPLES already accounts for it */

    /* Allocate ring buffer and DMA output buffer from the plugin heap.
     * These allocations are permanent for the plugin's lifetime. */
    ring_buf    = (int16_t *)sys_malloc(RING_BUF_SAMPLES  * sizeof(int16_t));
    pcm_dma_buf = (int16_t *)sys_malloc(PCM_CHUNK_SAMPLES * sizeof(int16_t));

    /* Initialise indices and diagnostic counters. */
    ring_head    = 0;
    ring_tail    = 0;
    pcm_underruns = 0;

    /* Zero both buffers so the first callback invocation returns clean silence
     * even before sound_write() has enqueued any data. */
    rb->memset(pcm_silence,  0, sizeof(pcm_silence));
    if (pcm_dma_buf)
        rb->memset(pcm_dma_buf, 0, PCM_CHUNK_SAMPLES * sizeof(int16_t));

    /* Stop Rockbox's own audio playback so we can take over the PCM mixer. */
    rb->audio_stop();

#if INPUT_SRC_CAPS != 0
    /* Route audio output to the playback path (required on targets with
     * configurable I/O routing; harmless no-op on the iPod Classic 6G). */
    rb->audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    rb->audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif

    /* Set the PCM sample rate.  22050 Hz matches igpSP's iPod Linux default
     * and is natively supported by the Wolfson WM8758 DAC on the 6G.
     * See SOUND_SAMPLE_RATE comment above if you need to change this. */
    rb->mixer_set_frequency(SOUND_SAMPLE_RATE);

    /* Register our callback and start the PCM mixer channel.  The mixer will
     * call pcm_callback() immediately for the first chunk; it returns silence
     * until sound_write() begins populating the ring buffer. */
    rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK,
                                pcm_callback, NULL, 0);
}

void sound_write(const int16_t *buf, int len)
{
    /* len is stereo *frames*; each frame is 2 int16_t samples (L + R). */
    int      samples = len * 2;
    int      i;

    if (!ring_buf || !buf || samples <= 0)
        return;

    for (i = 0; i < samples; i++) {
        uint32_t head = ring_head;

        if ((head - ring_tail) >= (uint32_t)RING_BUF_SAMPLES) {
            /*
             * Ring buffer full (overrun).  Drop the oldest PCM_CHUNK_SAMPLES
             * by advancing ring_tail forward, keeping the most recent audio in
             * the buffer.  rb->pcm_play_lock() briefly masks the PCM IRQ so
             * the write to ring_tail is atomic with respect to pcm_callback().
             *
             * This never blocks — lock/unlock is a short IRQ-mask pair.
             */
            rb->pcm_play_lock();
            ring_tail += (uint32_t)PCM_CHUNK_SAMPLES;
            rb->pcm_play_unlock();
        }

        ring_buf[head & RING_BUF_MASK] = buf[i];
        ring_head = head + 1;
    }
}

void sound_set_volume(int vol)
{
    /*
     * Map the 0–100 percentage to the Rockbox SOUND_VOLUME range.
     *
     * rb->sound_min(SOUND_VOLUME) / rb->sound_max(SOUND_VOLUME) return the
     * hardware DAC's usable range in Rockbox units (typically -74 to +6 dB on
     * the WM8758, but queried at runtime to remain target-agnostic).
     *
     * A linear mapping from the percentage to [min, max] approximates the
     * perceptual volume curve well enough for in-game volume control.
     */
    int min_vol = rb->sound_min(SOUND_VOLUME);
    int max_vol = rb->sound_max(SOUND_VOLUME);
    int rb_vol  = min_vol + (vol * (max_vol - min_vol) + 50) / 100;
    rb->sound_set(SOUND_VOLUME, rb_vol);
}

void sound_exit(void)
{
    /* Stop the mixer channel and restore the default sample rate so Rockbox's
     * own audio playback works normally after the plugin exits. */
    rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    rb->mixer_set_frequency(HW_SAMPR_DEFAULT);

    /* No need to free ring_buf or pcm_dma_buf — the sys_malloc() bump arena
     * is discarded in its entirety when the plugin returns to Rockbox. */
    ring_buf    = NULL;
    pcm_dma_buf = NULL;
    ring_head   = 0;
    ring_tail   = 0;
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

/* =========================================================================
 * FILE* COMPATIBILITY WRAPPERS  (Phase 5)
 * =========================================================================
 *
 * igpSP's common.h expands the file_open/read/write/close/seek macros to
 * standard C FILE* calls (fopen, fread, fwrite, fclose, fseek, ftell, fgets).
 * We back these with Rockbox file descriptors so no libc stdio is needed.
 *
 * The FILE typedef and these function declarations are exposed to igpSP
 * source files via src/rockbox_compat.h (injected with -include).
 *
 * Implementation notes:
 *   - FILE structs are bump-allocated from sys_malloc() so they live for the
 *     plugin lifetime.  We never free them — consistent with igpSP's pattern
 *     of one open → read/write → close per data region.
 *   - We allocate a pool of FILE slots to avoid per-call sys_malloc overhead
 *     and to support the small number of simultaneously open files igpSP uses
 *     (BIOS, ROM, config, save state — at most ~4 at once).
 * ========================================================================= */

#define RB_FILE_POOL_SIZE  8        /* max simultaneously open FILE*s       */

/* File slot pool — allocated once from the bump arena in file_pool_init(). */
typedef struct {
    int  fd;
    int  eof;
    bool in_use;
} rb_file_slot_t;

static rb_file_slot_t *file_pool = NULL;

/** Initialise the FILE slot pool.  Called from sys_mem_init() after the heap
 *  is set up.  Idempotent — safe to call multiple times. */
void rb_file_pool_init(void)
{
    if (file_pool)
        return;
    file_pool = (rb_file_slot_t *)sys_malloc(
                    RB_FILE_POOL_SIZE * sizeof(rb_file_slot_t));
    if (file_pool)
        rb->memset(file_pool, 0, RB_FILE_POOL_SIZE * sizeof(rb_file_slot_t));
}

/** Acquire a free slot from the pool. Returns NULL if pool is exhausted. */
static rb_file_slot_t *slot_alloc(void)
{
    int i;
    if (!file_pool)
        rb_file_pool_init();
    for (i = 0; i < RB_FILE_POOL_SIZE; i++) {
        if (!file_pool[i].in_use) {
            file_pool[i].in_use = true;
            file_pool[i].eof    = 0;
            file_pool[i].fd     = -1;
            return &file_pool[i];
        }
    }
    return NULL;  /* pool exhausted — should not happen in normal igpSP use */
}

/** Release a slot back to the pool. */
static void slot_free(rb_file_slot_t *s)
{
    if (s) s->in_use = false;
}

/* -------------------------------------------------------------------------
 * rb_fopen() — open a file, return a FILE* backed by a Rockbox fd.
 *
 * Supported modes (only what igpSP actually uses):
 *   "r" / "rb"  → O_RDONLY
 *   "w" / "wb"  → O_WRONLY | O_CREAT | O_TRUNC
 *   "a" / "ab"  → O_WRONLY | O_CREAT | O_APPEND
 * ------------------------------------------------------------------------- */
FILE *rb_fopen(const char *path, const char *mode)
{
    rb_file_slot_t *s;
    int             flags;

    if (!path || !mode)
        return NULL;

    /* Decode mode string into Rockbox open flags. */
    if (mode[0] == 'r')
        flags = O_RDONLY;
    else if (mode[0] == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else
        flags = O_RDONLY;  /* safe default */

    s = slot_alloc();
    if (!s)
        return NULL;

    s->fd = rb->open(path, flags, 0644);
    if (s->fd < 0) {
        slot_free(s);
        return NULL;
    }

    return (FILE *)s;
}

/* -------------------------------------------------------------------------
 * rb_fclose() — close and release.
 * ------------------------------------------------------------------------- */
int rb_fclose(FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    int ret;
    if (!s || !s->in_use || s->fd < 0)
        return EOF;
    ret = rb->close(s->fd);
    slot_free(s);
    return (ret < 0) ? EOF : 0;
}

/* -------------------------------------------------------------------------
 * rb_fread() — read count items of size bytes each.
 * Returns number of complete items read (matches fread semantics).
 * ------------------------------------------------------------------------- */
size_t rb_fread(void *buf, size_t size, size_t count, FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    ssize_t         n;

    if (!s || !s->in_use || s->fd < 0 || s->eof || !buf || size == 0)
        return 0;

    n = rb->read(s->fd, buf, size * count);
    if (n <= 0) {
        s->eof = (n == 0) ? 1 : 0;
        return 0;
    }
    if ((size_t)n < size * count)
        s->eof = 1;
    return (size_t)n / size;
}

/* -------------------------------------------------------------------------
 * rb_fwrite() — write count items of size bytes each.
 * Returns number of complete items written.
 * ------------------------------------------------------------------------- */
size_t rb_fwrite(const void *buf, size_t size, size_t count, FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    ssize_t         n;

    if (!s || !s->in_use || s->fd < 0 || !buf || size == 0)
        return 0;

    n = rb->write(s->fd, buf, size * count);
    if (n <= 0)
        return 0;
    return (size_t)n / size;
}

/* -------------------------------------------------------------------------
 * rb_fseek() — reposition file offset.
 * ------------------------------------------------------------------------- */
int rb_fseek(FILE *fp, long offset, int whence)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    off_t           pos;

    if (!s || !s->in_use || s->fd < 0)
        return -1;

    pos = rb->lseek(s->fd, (off_t)offset, whence);
    if (pos < 0)
        return -1;

    s->eof = 0;   /* a successful seek clears EOF */
    return 0;
}

/* -------------------------------------------------------------------------
 * rb_ftell() — return current file position.
 * ------------------------------------------------------------------------- */
long rb_ftell(FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    if (!s || !s->in_use || s->fd < 0)
        return -1L;
    return (long)rb->lseek(s->fd, 0, SEEK_CUR);
}

/* -------------------------------------------------------------------------
 * rb_fgets() — read a line (up to n-1 chars or newline, NUL-terminate).
 * igpSP uses this in load_game_config_file() to read .cfg line-by-line.
 * ------------------------------------------------------------------------- */
char *rb_fgets(char *buf, int n, FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    int i;
    char c;

    if (!s || !s->in_use || s->fd < 0 || s->eof || !buf || n <= 1)
        return NULL;

    for (i = 0; i < n - 1; i++) {
        ssize_t r = rb->read(s->fd, &c, 1);
        if (r <= 0) {
            if (r == 0) s->eof = 1;
            if (i == 0) return NULL;   /* nothing read at all */
            break;
        }
        buf[i] = c;
        if (c == '\n') { i++; break; }
    }
    buf[i] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * rb_feof() — return non-zero if the last read hit end-of-file.
 * ------------------------------------------------------------------------- */
int rb_feof(FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    return (!s || !s->in_use) ? 1 : s->eof;
}

/* -------------------------------------------------------------------------
 * rb_file_length() — return file size without disturbing the position.
 * igpSP prototype: u32 file_length(u8 *dummy, FILE *fp)
 * ------------------------------------------------------------------------- */
uint32_t rb_file_length(const char *dummy, FILE *fp)
{
    rb_file_slot_t *s = (rb_file_slot_t *)fp;
    off_t cur, end;
    (void)dummy;

    if (!s || !s->in_use || s->fd < 0)
        return 0;

    cur = rb->lseek(s->fd, 0, SEEK_CUR);
    end = rb->lseek(s->fd, 0, SEEK_END);
    rb->lseek(s->fd, cur, SEEK_SET);

    return (end > 0) ? (uint32_t)end : 0u;
}

/* =========================================================================
 * IPOD LINUX PLATFORM HOOKS  (Phase 5)
 * =========================================================================
 *
 * igpSP's IPOD_BUILD source calls these functions instead of SDL / GP2X /
 * PSP equivalents.  We implement them here as thin wrappers over our
 * Rockbox platform layer.
 *
 * Call sites in igpSP source (matching the original iPod Linux main.c):
 *   main.c:     ipod_init_conf(), ipod_init_hw(), ipod_init_input(),
 *               ipod_init_cop(), ipod_update_settings()
 *   main.c quit(): ipod_exit_conf(), ipod_exit_cop(), ipod_exit_input(),
 *                  ipod_exit_hw(), ipod_exit_video()
 *   sound.c:    ipod_init_sound(), ipod_exit_sound()
 *   input.c:    ipod_update_ingame_input(), ipod_update_menu_input()
 *   main.c:     get_ticks_us(), delay_us()
 *
 * COP note: The iPod Linux COP (PP502x second core) registers do NOT exist
 * on the S5L8702.  ipod_init_cop / ipod_exit_cop are intentional no-ops.
 * The Rockbox DMA-driven PCM system replaces the COP audio pipeline.
 * ========================================================================= */

/* ── Hardware init / exit ─────────────────────────────────────────────── */

/** ipod_init_conf() — load persistent configuration.  igpSP calls this to
 *  read stored settings.  Phase 5 stub: settings live in /.rockbox/igpsp/. */
void ipod_init_conf(void)
{
    /* Phase 6: load saved settings from /.rockbox/igpsp/igpsp.cfg */
}

/** ipod_exit_conf() — save persistent configuration on exit. */
void ipod_exit_conf(void)
{
    /* Phase 6: write settings to /.rockbox/igpsp/igpsp.cfg */
}

/** ipod_init_hw() — hardware video initialisation.
 *  Called before igpSP's init_video() in the original iPod Linux main(). */
void ipod_init_hw(void)
{
    vid_init();   /* set up Rockbox LCD, scaling tables, framebuffer pointer */
}

/** ipod_exit_hw() — hardware video teardown (alias of ipod_exit_video). */
void ipod_exit_hw(void)
{
    vid_exit();   /* restore Rockbox LCD viewport and clear display */
}

/** ipod_exit_video() — video teardown called from igpSP's quit(). */
void ipod_exit_video(void)
{
    vid_exit();
}

/** ipod_init_input() — initialise input subsystem.
 *  Called before the emulation loop in the original iPod Linux main(). */
void ipod_init_input(void)
{
    input_init();  /* configure Rockbox button/wheel layer for GBA input */
}

/** ipod_exit_input() — release input resources; called from igpSP's quit(). */
void ipod_exit_input(void)
{
    input_exit();  /* re-enable wheel events for normal Rockbox operation */
}

/** ipod_init_cop() — start the PP502x second core.  NO-OP on S5L8702. */
void ipod_init_cop(void)
{
    /* S5L8702 has no second core accessible via PP502x COP registers.
     * The Rockbox DMA/IRQ audio system replaces the COP audio pipeline. */
}

/** ipod_exit_cop() — shut down the COP.  NO-OP on S5L8702. */
void ipod_exit_cop(void)
{
    /* no-op — see ipod_init_cop() comment */
}

/** ipod_update_settings() — apply any changed settings mid-session.
 *  igpSP calls this after ipod_init_hw() in the original main(). */
void ipod_update_settings(void)
{
    /* Phase 6: apply screen_scale, sound_volume, etc. from global settings. */
}

/* ── Sound init / exit ────────────────────────────────────────────────── */

/** ipod_init_sound() — start Rockbox PCM output.
 *  Called from igpSP's init_sound() (renamed igpsp_init_sound() via shim)
 *  to initialise the platform audio backend. */
void ipod_init_sound(void)
{
    /* 22 050 Hz stereo — matches igpSP's iPod Linux default.
     * The Wolfson WM8758 DAC on the 6G supports this rate natively. */
    sound_init(22050, 2);
}

/** ipod_exit_sound() — stop Rockbox PCM output.
 *  Called from igpSP's sound_exit() (renamed igpsp_sound_exit() via shim)
 *  during quit() to stop the mixer before the plugin returns. */
void ipod_exit_sound(void)
{
    sound_exit();
}

/* ── Input polling ────────────────────────────────────────────────────── */

/**
 * ipod_update_ingame_input() — poll hardware and return the current GBA key
 * state in active-HIGH format (bit SET = button pressed).
 *
 * igpSP's input.c (IPOD_BUILD) calls this from update_input():
 *   key = ipod_update_ingame_input();
 *   trigger_key(key);                       ← raise GBA keypad IRQ if changed
 *   io_registers[REG_P1] = (~key) & 0x3FF; ← write active-LOW KEYINPUT
 *
 * We call input_poll() to update our internal state, check for the hold-
 * switch exit trigger, and return (~current_keys & 0x3FF) to invert our
 * active-LOW KEYINPUT value back to the active-HIGH format igpSP expects.
 *
 * Exit path: if exit_requested is set (user chose "Quit" from in-game menu),
 * we call igpSP's quit() which calls ipod_exit_* hooks and then reaches our
 * exit() → longjmp() macro, returning control to plugin_start().
 */
uint32_t ipod_update_ingame_input(void)
{
    /* Declared in igpSP's main.h; resolves at link time from main.c. */
    extern void quit(void);

    input_poll();   /* update current_keys and check hold switch / menu */

    if (exit_requested) {
        /* quit() calls ipod_exit_* → our cleanup hooks, then exit(0)
         * which longjmps back to plugin_start()'s setjmp guard. */
        quit();
        /* NOT REACHED — longjmp unwinds the stack */
    }

    /* Convert our active-LOW KEYINPUT (0=pressed) to active-HIGH (1=pressed)
     * so igpSP's input.c can correctly re-invert it into REG_P1. */
    return (~input_read_keys()) & 0x3FFu;
}

/**
 * ipod_update_menu_input() — return a GUI menu action.
 * igpSP's input.c calls this when navigating the built-in igpSP file browser.
 * We redirect all menu navigation to our Rockbox in-game menu (Phase 6 will
 * wire the full igpSP file browser).
 */
int ipod_update_menu_input(void)
{
    /* gui_action_type: CURSOR_NONE = 0.  Return no-action for now.
     * Phase 6: map Rockbox button events to igpSP CURSOR_* constants. */
    return 0;  /* CURSOR_NONE */
}

/* ── Screen output ────────────────────────────────────────────────────── */

/**
 * update_screen() — push the completed GBA frame to the Rockbox LCD.
 *
 * igpSP's synchronize() (IPOD_BUILD path in main.c) calls update_screen()
 * once per rendered frame.  igpSP's video.c (IPOD_BUILD) sets:
 *   u16 *screen;   ← points to the 240×160 RGB565 output buffer
 * and defines:
 *   #define get_screen_pixels() ((u16 *)screen)
 *
 * We scale and blit this to the 320×240 Rockbox framebuffer via vid_update().
 */
void update_screen(void)
{
    /* 'screen' is declared in igpSP's video.c (IPOD_BUILD block).
     * We extern it here; it is set up by igpSP's init_video(). */
    extern uint16_t *screen;
    vid_update(screen);
}

/* ── Timing ───────────────────────────────────────────────────────────── */

/**
 * get_ticks_us() — return a microsecond timestamp.
 *
 * igpSP uses this for frame rate limiting in synchronize().
 * Rockbox ticks are 10 ms resolution; we scale to microseconds.
 * Note: this gives 10 000 µs resolution (sufficient for ~100 Hz frame timing).
 */
void get_ticks_us(uint64_t *tick_return)
{
    if (tick_return)
        *tick_return = (uint64_t)(*rb->current_tick) * 10000ULL;
}

/**
 * delay_us() — sleep for approximately us_count microseconds.
 * Used by igpSP's synchronize() to cap frame rate.
 * Rockbox minimum sleep granularity is one tick (10 ms).
 */
void delay_us(uint32_t us_count)
{
    int ms = (int)(us_count / 1000u);
    if (ms > 0)
        sys_sleep_ms(ms);
    else
        rb->yield();   /* sub-millisecond: at least yield to other threads */
}

/**
 * print_string() — igpSP debug text overlay.  Silenced in Rockbox builds.
 * igpSP calls this in synchronize() to display FPS counters.
 */
void print_string(const char *str, uint32_t fg, uint32_t bg,
                  int x, int y)
{
    (void)str; (void)fg; (void)bg; (void)x; (void)y;
    /* Phase 6: optionally render debug overlay via rb->lcd_putsxy() */
}

/* =========================================================================
 * sys_malloc_debug() — IGPSP_DEBUG allocation logger  (Phase 5)
 * =========================================================================
 * When -DIGPSP_DEBUG is passed at build time, rockbox_compat.h reroutes
 * malloc/calloc/sys_malloc through this function.  It splashes the
 * allocation size and remaining heap to the LCD so we can verify that all
 * GBA memory regions fit within the audio buffer before enabling the JIT.
 *
 * The splash is brief (HZ/8 = 125 ms) so startup is not excessively slow.
 * Disable IGPSP_DEBUG for normal builds.
 * ========================================================================= */
#ifdef IGPSP_DEBUG
void *sys_malloc_debug(size_t size, const char *tag)
{
    void  *p   = sys_malloc(size);
    size_t rem = sys_heap_remaining();

    rb->splashf(HZ / 8, "%s: %u B  rem %u KB",
                tag ? tag : "?",
                (unsigned)size,
                (unsigned)(rem / 1024));
    return p;
}
#endif /* IGPSP_DEBUG */
