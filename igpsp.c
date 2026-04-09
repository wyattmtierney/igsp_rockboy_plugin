/* igpSP-Rockbox — GBA emulator plugin for Rockbox
 * igpsp.c — plugin entry point (Phase 5: full igpSP core integration)
 *
 * Startup sequence mirrors the original igpSP iPod Linux main() exactly,
 * replacing platform-specific iPod Linux calls with Rockbox equivalents
 * provided by platform/rockbox/sys_rockbox_gba.c.
 *
 * Key design decisions (Phase 5):
 *   • sys_mem_init() claims the ~60 MB audio buffer as the plugin heap
 *     BEFORE any igpSP core function runs — all igpSP malloc() calls
 *     (redirected via src/rockbox_compat.h) draw from this arena.
 *   • igpSP's quit() eventually calls exit(0); the shim redirects exit()
 *     to longjmp(igpsp_exit_jmp, 1) so the plugin returns cleanly.
 *   • All igpSP platform hooks (ipod_init_hw, ipod_exit_sound, …) are
 *     implemented in platform/rockbox/sys_rockbox_gba.c.
 *   • This file does NOT include src/rockbox_compat.h — it uses the real
 *     platform API (sys_rockbox_gba.h) and declares igpSP symbols extern.
 *
 * Hardware target : iPod Classic 6G (Model A1238)
 *                   Samsung S5L8702, ARM926EJ-S ~216 MHz
 *                   Rockbox firmware, 320×240 RGB565 LCD
 */

#include "plugin.h"
#include "platform/rockbox/sys_rockbox_gba.h"
#include <setjmp.h>
#include <string.h>   /* strrchr, strncpy, strlen */

/* -------------------------------------------------------------------------
 * PLUGIN_HEADER — mandatory Rockbox plugin magic.
 *
 * Expands to:
 *   const struct plugin_api *rb DATA_ATTR;        ← filled by loader
 *   const struct plugin_header __header;           ← version / magic block
 *
 * sys_rockbox_gba.c declares rb as extern; the PLUGIN_HEADER definition
 * here satisfies that extern for the entire link unit.
 * ------------------------------------------------------------------------- */
PLUGIN_HEADER

/* -------------------------------------------------------------------------
 * igpsp_exit_jmp — longjmp target for igpSP's exit() calls.
 *
 * src/rockbox_compat.h redefines  exit(code) → longjmp(igpsp_exit_jmp, 1)
 * so igpSP's quit() (which calls exit(0)) returns control here instead of
 * terminating the process.  Declared extern in rockbox_compat.h; defined
 * here in the plugin entry translation unit.
 * ------------------------------------------------------------------------- */
jmp_buf igpsp_exit_jmp;

/* =========================================================================
 * igpSP core function declarations
 * =========================================================================
 * These symbols are defined in the igpSP src/ files.  We do NOT include
 * common.h or any igpSP header here — the shim (rockbox_compat.h) is only
 * injected into src/ files.  Bare extern declarations give the linker
 * enough information to resolve the symbols without dragging in the entire
 * igpSP header tree.
 * ========================================================================= */

/* memory.c / main.c */
extern void   init_gamepak_buffer(void);  /* allocates gamepak_rom from heap */
extern int    load_bios(char *name);       /* returns 0 on success, -1 on err */
extern void   init_main(void);             /* initialise main igpSP subsystem  */
extern int    load_gamepak(char *name);    /* returns 0 on success, -1 on err */
extern void   init_memory(void);           /* set up GBA memory map            */
extern char   main_path[512];              /* directory igpSP searches for BIOS*/
extern char   bios_rom[];                  /* loaded BIOS; [0] == 0x18 if OK   */

/* cpu.c / cpu_threaded.c */
extern void   init_cpu(void);
extern void   execute_arm_translate(unsigned int cycles);
extern void   execute_arm(unsigned int cycles);
extern unsigned int execute_cycles;        /* initial cycle count for JIT      */

/* sound.c  (renamed to igpsp_init_sound via shim in src/ files) */
extern void   igpsp_init_sound(void);      /* GBA APU init; calls ipod_init_sound() */

/* input.c  (IPOD_BUILD: init_input is a no-op stub) */
extern void   init_input(void);

/* video.c  (GBA PPU init; sets up 'screen' pointer) */
extern void   init_video(void);

/* main.c */
extern void   trigger_ext_event(void);     /* trigger any pending GBA events   */
extern void   load_config_file(void);      /* load global igpSP config         */

/* =========================================================================
 * Helper: derive directory from a file path
 * =========================================================================
 * Given "/foo/bar/rom.gba", writes "/foo/bar" into dst (up to dst_size-1).
 * If no slash is found, writes "/".
 * ========================================================================= */
static void path_dirname(const char *path, char *dst, size_t dst_size)
{
    const char *last_slash;
    size_t      len;

    last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        /* Root or no directory component. */
        dst[0] = '/';
        dst[1] = '\0';
        return;
    }
    len = (size_t)(last_slash - path);
    if (len >= dst_size)
        len = dst_size - 1;
    strncpy(dst, path, len);
    dst[len] = '\0';
}

/* =========================================================================
 * Helper: build a BIOS path and test whether it exists
 * =========================================================================
 * Returns 1 if the file at 'path' is accessible (can be opened O_RDONLY),
 * 0 otherwise.
 * ========================================================================= */
static int file_exists(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd >= 0) {
        sys_close(fd);
        return 1;
    }
    return 0;
}

/* =========================================================================
 * plugin_start() — Rockbox plugin entry point
 * =========================================================================
 *
 * @param parameter  Absolute path to the selected .gba ROM file, as passed
 *                   by the Rockbox file browser.  NULL if launched without
 *                   a file association.
 *
 * @return  PLUGIN_OK    — emulation ran and exited cleanly.
 *          PLUGIN_ERROR — fatal setup error (no ROM, missing BIOS, etc.).
 *
 * Startup sequence (matches igpSP iPod Linux main() with Rockbox platform):
 *
 *   1.  Validate ROM path.
 *   2.  Boost CPU to ~216 MHz.
 *   3.  Claim audio buffer (~60 MB) as plugin heap via sys_mem_init().
 *   4.  init_gamepak_buffer() — allocate gamepak_rom from heap.
 *   5.  Set main_path to ROM's directory (igpSP uses it for BIOS lookup).
 *   6.  ipod_init_conf/hw/input/cop — platform hardware init
 *       (these call vid_init(), input_init() internally).
 *   7.  init_video() — GBA PPU init (sets up 'screen' pointer).
 *   8.  load_bios() — fail fast with a readable error if missing.
 *   9.  init_main() — igpSP main-subsystem init.
 *   10. igpsp_init_sound() — GBA APU init + calls ipod_init_sound()
 *       which starts our Rockbox PCM mixer at 22 050 Hz stereo.
 *   11. init_input() — GBA input init (IPOD_BUILD: no-op stub).
 *   12. load_gamepak() — load ROM; fail fast on error.
 *   13. init_cpu() — JIT dynarec init.
 *   14. init_memory() — GBA memory map init.
 *   15. trigger_ext_event() — trigger any pending GBA boot events.
 *   16. execute_arm_translate() — blocking JIT main loop.
 *       Exit path: user selects Quit → ipod_update_ingame_input() calls
 *       igpSP's quit() → ipod_exit_* hooks run → exit(0) hits our
 *       longjmp macro → setjmp() returns 1 → fall through to cleanup.
 *   17. Cleanup: sound_exit(), vid_exit(), sys_unboost_cpu().
 *   18. Return PLUGIN_OK.
 * ========================================================================= */
enum plugin_status plugin_start(const void *parameter)
{
    const char *rom_path = (const char *)parameter;

    /* Paths built during startup — fixed-size to avoid heap usage before
     * sys_mem_init().  512 chars matches igpSP's internal path buffers. */
    char bios_path[512];
    char rom_dir[512];

    /* ------------------------------------------------------------------
     * 1. Validate ROM path.
     * ------------------------------------------------------------------ */
    if (!rom_path || rom_path[0] == '\0') {
        rb->splash(HZ * 3,
                   "igpSP: No ROM selected — "
                   "launch from the file browser with a .gba file.");
        return PLUGIN_ERROR;
    }

    /* ------------------------------------------------------------------
     * 2. Boost CPU to maximum clock (~216 MHz on S5L8702).
     * Must happen before any heavy work; PLL stabilises in a few µs.
     * ------------------------------------------------------------------ */
    sys_boost_cpu();

    /* ------------------------------------------------------------------
     * 3. Claim the audio buffer (~60 MB on iPod Classic 6G) as the
     * plugin heap.  Must precede ALL sys_malloc() / malloc() calls,
     * including those inside igpSP core init functions.
     * ------------------------------------------------------------------ */
    sys_mem_init();

    /* ------------------------------------------------------------------
     * 4. Allocate gamepak_rom from the heap.
     * init_gamepak_buffer() calls malloc(gamepak_ram_buffer_size) which
     * the shim redirects to sys_malloc().  gamepak_ram_buffer_size is
     * typically 16–32 MB in igpSP; our OOM guard caps it safely.
     * ------------------------------------------------------------------ */
    init_gamepak_buffer();

    /* ------------------------------------------------------------------
     * 5. Set main_path to the ROM's directory.
     * igpSP uses main_path as the base for relative BIOS lookup and
     * config file loading.  On Rockbox all paths are absolute, so we
     * extract the directory component of rom_path.
     * ------------------------------------------------------------------ */
    path_dirname(rom_path, rom_dir, sizeof(rom_dir));
    strncpy(main_path, rom_dir, sizeof(main_path) - 1);
    main_path[sizeof(main_path) - 1] = '\0';

    /* ------------------------------------------------------------------
     * 5b. Derive BIOS path.
     * Search order:
     *   (a) <rom_directory>/gba_bios.bin   — alongside the ROM
     *   (b) /.rockbox/igpsp/gba_bios.bin   — plugin config directory
     * ------------------------------------------------------------------ */
    rb->snprintf(bios_path, sizeof(bios_path),
                 "%s/gba_bios.bin", rom_dir);
    if (!file_exists(bios_path)) {
        rb->snprintf(bios_path, sizeof(bios_path),
                     "/.rockbox/igpsp/gba_bios.bin");
    }

    /* ------------------------------------------------------------------
     * 5c. Load the igpSP global config file (non-fatal if missing).
     * load_config_file() reads <main_path>/gpsp.cfg for settings like
     * frameskip mode, screen scale, and sound volume.
     * ------------------------------------------------------------------ */
    load_config_file();

    /* ------------------------------------------------------------------
     * 6. Platform hardware init — matches igpSP iPod Linux main() order.
     *
     *   ipod_init_conf()  — load stored settings (Phase 6 stub)
     *   ipod_init_hw()    → vid_init()     — Rockbox LCD + scale tables
     *   ipod_init_input() → input_init()   — Rockbox button / wheel
     *   ipod_init_cop()   — no-op (no PP502x COP on S5L8702)
     *
     * These are implemented in platform/rockbox/sys_rockbox_gba.c.
     * ------------------------------------------------------------------ */
    ipod_init_conf();
    ipod_init_hw();       /* calls vid_init() internally */
    ipod_init_input();    /* calls input_init() internally */
    ipod_init_cop();      /* no-op on S5L8702 */

    /* ------------------------------------------------------------------
     * 7. GBA PPU init.
     * init_video() sets up igpSP's internal renderer state (pixel format,
     * scanline buffers, palette tables).  It also allocates the 'screen'
     * pointer that vid_update() reads during update_screen() calls.
     * ------------------------------------------------------------------ */
    init_video();

    /* ------------------------------------------------------------------
     * 8. Load BIOS — fail fast with a readable error splash.
     * load_bios() returns 0 on success, -1 if the file cannot be opened
     * or is the wrong size / bad checksum.
     * ------------------------------------------------------------------ */
    if (load_bios(bios_path) != 0) {
        rb->splashf(HZ * 4,
                    "igpSP: GBA BIOS not found.\n"
                    "Place gba_bios.bin next to the ROM\n"
                    "or at /.rockbox/igpsp/gba_bios.bin\n"
                    "(MD5: a860e8c0b6d573d191e4ec7db1b1e4f6)");
        goto err_cleanup;
    }

    /* Warn on incorrect BIOS image (non-fatal — many games still run). */
    if ((unsigned char)bios_rom[0] != 0x18) {
        rb->splash(HZ * 2,
                   "igpSP: BIOS checksum mismatch — "
                   "some games may not work correctly.");
        /* Continue anyway. */
    }

    /* ------------------------------------------------------------------
     * 9. Main igpSP subsystem init.
     * init_main() initialises global state: timer structures, cpu_ticks,
     * execute_cycles, frame timing, frameskip counters, etc.
     * ------------------------------------------------------------------ */
    init_main();

    /* ------------------------------------------------------------------
     * 10. GBA APU init.
     * igpsp_init_sound() is igpSP's init_sound() renamed by the shim to
     * avoid a link conflict with our platform sound_init().  It:
     *   • initialises igpSP's internal APU mixing state
     *   • calls ipod_init_sound() → our sound_init(22050, 2) to start
     *     the Rockbox PCM mixer at 22 050 Hz stereo.
     * ------------------------------------------------------------------ */
    igpsp_init_sound();

    /* ------------------------------------------------------------------
     * 11. GBA input init.
     * init_input() in input.c (IPOD_BUILD path) is an empty stub:
     *   void init_input() {}
     * Our Rockbox input layer was already started by ipod_init_input()
     * in step 6.  This call is kept for correctness with igpSP's sequence.
     * ------------------------------------------------------------------ */
    init_input();

    /* ------------------------------------------------------------------
     * 12. Load ROM — fail fast on error.
     * load_gamepak() reads the ROM header, detects save type, loads
     * cheats if a .cht file exists, and fills gamepak_rom[].
     * Returns 0 on success, non-zero (-1) on failure.
     * ------------------------------------------------------------------ */
    if (load_gamepak((char *)rom_path) != 0) {
        rb->splashf(HZ * 3,
                    "igpSP: Failed to load ROM:\n%s", rom_path);
        goto err_cleanup;
    }

    /* ------------------------------------------------------------------
     * 13–14. CPU and memory map init.
     * init_cpu()    — clear JIT translation caches, set up branch hash.
     * init_memory() — reset GBA memory map, apply wait-state table,
     *                 initialise I/O registers to power-on defaults.
     * ------------------------------------------------------------------ */
    init_cpu();
    init_memory();

    /* ------------------------------------------------------------------
     * 15. Trigger any pending external GBA events before starting the JIT.
     * ------------------------------------------------------------------ */
    trigger_ext_event();

    /* ------------------------------------------------------------------
     * 16. Main emulation loop.
     *
     * execute_arm_translate() is the igpSP ARM JIT dynarec entry point.
     * It runs indefinitely, translating GBA ARM/Thumb opcodes to native
     * ARM32 code and dispatching into the translation cache.
     *
     * At each GBA frame boundary, igpSP's update_gba() is invoked which:
     *   • calls update_input() → ipod_update_ingame_input() (our hook)
     *     to poll Rockbox buttons/wheel and update io_registers[REG_P1].
     *   • calls update_screen() (our hook) → vid_update() to scale and
     *     blit the 240×160 GBA framebuffer to the 320×240 Rockbox LCD.
     *   • calls synchronize() → delay_us() / get_ticks_us() for frame
     *     rate limiting.
     *
     * Exit path:
     *   User long-presses MENU (hold switch) → show_ingame_menu() runs →
     *   user selects "Quit" → exit_requested = true →
     *   ipod_update_ingame_input() detects flag → calls igpSP's quit() →
     *   quit() calls ipod_exit_{sound,input,video}() hooks + exit(0) →
     *   exit() macro → longjmp(igpsp_exit_jmp, 1) → setjmp() returns 1
     *   → fall through to cleanup below.
     *
     * execute_arm() is the interpreter fallback for code paths the JIT
     * cannot handle (e.g. self-modifying code after an SMC invalidation).
     * Calling both after the setjmp matches the original igpSP main().
     * ------------------------------------------------------------------ */
    if (setjmp(igpsp_exit_jmp) == 0) {
        /* First entry: start the JIT.  Will not return until exit(). */
        execute_arm_translate(execute_cycles);
        /* Fallback interpreter — reached only if JIT is unavailable or
         * after a cache flush forces interpreter mode for one block. */
        execute_arm(execute_cycles);
    }
    /* setjmp() returns here with value 1 after longjmp(igpsp_exit_jmp,1). */

    /* ------------------------------------------------------------------
     * 17. Cleanup — reverse init order, always reached on normal exit.
     *
     * Note: igpSP's quit() has already called:
     *   ipod_exit_sound() → sound_exit()    (stops Rockbox PCM)
     *   ipod_exit_input() → input_exit()    (re-enables wheel events)
     *   ipod_exit_video() → vid_exit()      (restores LCD viewport)
     * Calling our cleanup functions again is safe — all are idempotent.
     * We call them here to handle the rare case where quit() was not
     * reached (e.g. if execute_arm_translate returned without going through
     * the normal quit path).
     * ------------------------------------------------------------------ */
    sound_exit();
    input_exit();
    vid_exit();
    sys_unboost_cpu();

    return PLUGIN_OK;

    /* ------------------------------------------------------------------
     * Error cleanup — reached via goto when BIOS or ROM load fails.
     * Platform subsystems were partially initialised; tear down whatever
     * was started to leave Rockbox in a clean state.
     * ------------------------------------------------------------------ */
err_cleanup:
    sound_exit();
    input_exit();
    vid_exit();
    sys_unboost_cpu();
    return PLUGIN_ERROR;
}
