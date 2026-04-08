/* igpSP-Rockbox — GBA emulator plugin for Rockbox
 * igpsp.c — plugin entry point
 *
 * Phase 1 scaffold: initialises the platform layer, shows a confirmation
 * splash, and tears down cleanly.  No igpSP core calls yet.
 *
 * To build: copy the igpsp/ directory into $(APPSDIR)/plugins/ inside a
 * Rockbox source tree, then add igpsp.make to the plugins Makefile.
 *
 * Hardware target : iPod Classic 6G (Model A1238)
 *                   Samsung S5L8702, ARM926EJ-S ~216 MHz
 *                   Rockbox firmware, 320×240 RGB565 LCD
 */

#include "plugin.h"
#include "platform/rockbox/sys_rockbox_gba.h"

/* PLUGIN_HEADER must appear at file scope (not inside a function).
 * It expands to two definitions:
 *   1. const struct plugin_api *rb DATA_ATTR;
 *      — the global Rockbox API handle filled by the firmware before
 *        plugin_start() is called.  sys_rockbox_gba.c declares it extern.
 *   2. const struct plugin_header __header
 *      — the magic/version structure the Rockbox plugin loader checks.
 */
PLUGIN_HEADER

/* -------------------------------------------------------------------------
 * plugin_start() — Rockbox plugin entry point.
 *
 * @param parameter  For plugins launched from the file browser, this is a
 *                   NUL-terminated absolute path to the selected file
 *                   (e.g. "/gba/pokemon.gba").  NULL if launched from a
 *                   menu without a file association.
 *
 * @return  PLUGIN_OK      — exited cleanly (user pressed exit).
 *          PLUGIN_ERROR   — fatal error (no ROM path, init failure, etc.).
 *
 * Phase 1 behaviour:
 *   1. Validate ROM path parameter.
 *   2. Boost CPU clock.
 *   3. Claim audio buffer as plugin heap.
 *   4. Stub-initialise video, input, audio.
 *   5. Display a confirmation screen showing the plugin loaded and the
 *      ROM path that would be loaded.
 *   6. Wait for SELECT button press, then tear down cleanly.
 *
 * TODO Phase 5: replace step 5 with the igpSP emulator main loop:
 *   load_gba_bios(bios_path);
 *   load_gamepak(rom_path);
 *   for (;;) {
 *       update_gba();              // runs one GBA frame
 *       vid_update(gba_framebuf);  // scale + blit
 *       input_poll();
 *       gba_key_state = input_read_keys();
 *       if (exit_requested) break;
 *   }
 * ------------------------------------------------------------------------- */
enum plugin_status plugin_start(const void *parameter)
{
    const char *rom_path = (const char *)parameter;

    /* ------------------------------------------------------------------
     * 1. Validate ROM path.
     * The Rockbox file browser passes the selected file's absolute path.
     * A NULL or empty path means the plugin was launched without a file
     * association — show a helpful error and exit.
     * ------------------------------------------------------------------ */
    if (!rom_path || rom_path[0] == '\0') {
        rb->splash(HZ * 3,
                   "igpSP: No ROM selected. "
                   "Launch from the file browser with a .gba file.");
        return PLUGIN_ERROR;
    }

    /* ------------------------------------------------------------------
     * 2. Boost CPU to maximum clock.
     * Must happen before any heavy initialisation; the S5L8702 PLL takes
     * a few microseconds to stabilise after the boost request.
     * ------------------------------------------------------------------ */
    sys_boost_cpu();

    /* ------------------------------------------------------------------
     * 3. Claim audio buffer as plugin heap.
     * rb->plugin_get_audio_buffer() surrenders the PCM buffer (~60 MB on
     * 6G) to us.  This call is idempotent and must precede sys_malloc().
     * ------------------------------------------------------------------ */
    sys_mem_init();

    /* ------------------------------------------------------------------
     * 4. Initialise subsystems (all stubs in Phase 1).
     * ------------------------------------------------------------------ */
    vid_init();
    input_init();
    sound_init(44100, 2); /* 44100 Hz stereo — igpSP's fixed output format */

    /* ------------------------------------------------------------------
     * 5. Phase 1 placeholder screen.
     *
     * Shows a confirmation that:
     *   - The plugin loaded and linked correctly.
     *   - Platform init functions ran without crashing.
     *   - The ROM path parameter arrived intact.
     *
     * TODO Phase 2: replace with a loading bar + ROM info screen while
     *   load_gamepak() reads the cartridge header and allocates EWRAM/IWRAM.
     * ------------------------------------------------------------------ */
    rb->lcd_clear_display();

    /* Title bar */
    rb->lcd_puts(0, 0, "igpSP-Rockbox  v0.1-phase1");
    rb->lcd_puts(0, 1, "--------------------------");

    /* Status */
    rb->lcd_puts(0, 2, "Platform layer: OK (stubs)");
    rb->lcd_puts(0, 3, "CPU boost:      ON");

    /* Show heap remaining so we can verify audio buffer was claimed */
    {
        char heap_str[48];
        rb->snprintf(heap_str, sizeof(heap_str),
                     "Heap avail:     %u KB",
                     (unsigned)(sys_heap_remaining() / 1024));
        rb->lcd_puts(0, 4, heap_str);
    }

    /* ROM path — may be longer than the screen; truncate if needed */
    rb->lcd_puts(0, 5, "ROM path:");
    rb->lcd_puts(0, 6, rom_path);

    rb->lcd_puts(0, 8, "Press SELECT to exit.");
    rb->lcd_update();

    /* ------------------------------------------------------------------
     * 6. Wait for SELECT to exit.
     *
     * rb->button_get_w_tmo(HZ/10) polls with a 100 ms timeout so we do
     * not burn all 216 MHz spinning in a tight button loop.
     *
     * We mask out BUTTON_REL (release events) so a SELECT release after
     * plugin launch does not immediately exit before the user reads the
     * screen.  We require a fresh SELECT press.
     *
     * TODO Phase 3: replace this loop with the emulator frame loop.
     *   The frame loop calls input_poll() each iteration and checks a
     *   global exit_requested flag set by input_poll() on long-MENU press.
     * ------------------------------------------------------------------ */
    {
        bool seen_release = false; /* ignore SELECT held from file browser */
        for (;;) {
            int btn = rb->button_get_w_tmo(HZ / 10);

            /* Discard the initial SELECT-release from the file browser
             * launch gesture — wait until SELECT is freshly pressed. */
            if (!seen_release) {
                if (btn & BUTTON_REL)
                    seen_release = true;
                continue;
            }

            if ((btn & ~BUTTON_REL) == BUTTON_SELECT)
                break;
        }
    }

    /* ------------------------------------------------------------------
     * 7. Tear down cleanly — always undo in reverse order of init.
     * sys_unboost_cpu() MUST be last so the SoC is not left at high clock
     * after the plugin exits.
     * ------------------------------------------------------------------ */
    sound_exit();
    input_exit();
    vid_exit();
    sys_unboost_cpu();

    return PLUGIN_OK;
}
