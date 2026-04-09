/* igpSP-Rockbox — compatibility shim for igpSP core source files
 * src/rockbox_compat.h
 *
 * Injected via  -include $(IGPSP_SRCDIR)/src/rockbox_compat.h
 * in IGPSP_CORE_CFLAGS so every igpSP src/ file compiles in a Rockbox
 * environment without direct modification of the igpSP source.
 *
 * This header is NOT included by igpsp.c or sys_rockbox_gba.c — those files
 * use the standard IGPSP_CFLAGS (no -include) and call the platform API
 * declared in sys_rockbox_gba.h directly.
 *
 * Processing order (guaranteed by -include):
 *   1. This file is fully preprocessed.
 *   2. All system header guards are claimed.
 *   3. Then the igpSP source file's own #includes are processed.
 *   Result: our type / macro definitions win any conflicts.
 *
 * Hardware target: iPod Classic 6G, Samsung S5L8702, ARM926EJ-S ~216 MHz
 *                  Rockbox firmware
 */

#ifndef ROCKBOX_COMPAT_H
#define ROCKBOX_COMPAT_H

/* =========================================================================
 * PLATFORM GUARDS
 * =========================================================================
 * IPOD_BUILD: tells igpSP's common.h to use the iPod Linux code paths.
 *   Effect: SDL.h is NOT included; file_open/read/write/close expand to
 *   FILE* ops (which we redirect); ipod_* platform hooks are referenced
 *   in input.c, video.c, sound.c instead of SDL / GP2X / PSP functions.
 *
 * ROCKBOX_BUILD: our own discriminator for any Rockbox-specific #ifdefs
 *   we introduce when populating src/ with adapted igpSP source.
 * ========================================================================= */
#ifndef IPOD_BUILD
#  define IPOD_BUILD
#endif
#ifndef ROCKBOX_BUILD
#  define ROCKBOX_BUILD
#endif

/* =========================================================================
 * BLOCK SYSTEM stdio.h
 * =========================================================================
 * We provide our own FILE* type and wrappers backed by Rockbox I/O.
 * Prevent the cross-compiler's stdio.h from defining a conflicting FILE
 * struct or function prototypes for fopen/fread/etc. that cannot be
 * satisfied at link time (no libc in a Rockbox plugin).
 *
 * Guard names vary by toolchain; define all common variants:
 *   newlib (arm-none-eabi-gcc)  → _STDIO_H_
 *   glibc / musl                → _STDIO_H
 *   some ARM embedded toolchains→ __STDIO_H__
 * ========================================================================= */
#ifndef _STDIO_H_
#  define _STDIO_H_
#endif
#ifndef _STDIO_H
#  define _STDIO_H
#endif
#ifndef __STDIO_H__
#  define __STDIO_H__
#endif
/* Also block stdint re-guard conflicts from plugin.h's sys includes */
/* (plugin.h is NOT included here; sys_rockbox_gba.h's forward decl is safe) */

/* Pull in only what we control: */
#include <stddef.h>    /* size_t, NULL, ptrdiff_t                          */
#include <stdint.h>    /* uint8_t … uint64_t                               */
#include <stdbool.h>   /* bool                                             */
#include <string.h>    /* memset, memcpy, strcmp, strncpy, strlen          */
#include <fcntl.h>     /* O_RDONLY, O_WRONLY, O_CREAT; SEEK_SET/CUR/END   */
#include <setjmp.h>    /* jmp_buf, setjmp, longjmp (for exit() redirect)  */

/* =========================================================================
 * FILE* COMPATIBILITY LAYER
 * =========================================================================
 * igpSP's common.h expands file_open() to:
 *   FILE *tag = fopen(name, stdio_file_open_read);
 * and uses fread(), fwrite(), fclose(), fseek(), ftell(), fgets().
 *
 * We provide a minimal FILE struct backed by a Rockbox file descriptor.
 * Functions are declared here; implemented in platform/rockbox/sys_rockbox_gba.c.
 * ========================================================================= */

/** Minimal FILE backed by a Rockbox open() descriptor. */
typedef struct {
    int  fd;    /**< Rockbox file descriptor (rb->open() result).          */
    int  eof;   /**< Non-zero when EOF has been reached.                   */
} FILE;

/** EOF sentinel — mirrors stdio convention. */
#ifndef EOF
#  define EOF  (-1)
#endif

/** NULL guard (may already be defined by stddef.h). */
#ifndef NULL
#  define NULL  ((void *)0)
#endif

/**
 * stdio_file_open_read / _write
 * igpSP's file_open() macro appends the mode token with ##:
 *   file_open(tag, name, read)  →  FILE *tag = fopen(name, stdio_file_open_read)
 */
#define stdio_file_open_read   "rb"
#define stdio_file_open_write  "wb"

/* Forward declarations — bodies in platform/rockbox/sys_rockbox_gba.c */
FILE   *rb_fopen(const char *path, const char *mode);
int     rb_fclose(FILE *fp);
size_t  rb_fread(void *buf, size_t size, size_t count, FILE *fp);
size_t  rb_fwrite(const void *buf, size_t size, size_t count, FILE *fp);
int     rb_fseek(FILE *fp, long offset, int whence);
long    rb_ftell(FILE *fp);
char   *rb_fgets(char *buf, int n, FILE *fp);
int     rb_feof(FILE *fp);

/**
 * file_length() — return file size in bytes.
 * igpSP prototype (non-PSP): extern u32 file_length(u8 *dummy, FILE *fp);
 * Seeks to end, reads position, seeks back.
 * Implemented in platform/rockbox/sys_rockbox_gba.c.
 */
uint32_t rb_file_length(const char *dummy, FILE *fp);

#define fopen(p,m)           rb_fopen(p, m)
#define fclose(fp)           rb_fclose(fp)
#define fread(b,s,n,fp)      rb_fread(b, s, n, fp)
#define fwrite(b,s,n,fp)     rb_fwrite(b, s, n, fp)
#define fseek(fp,off,w)      rb_fseek(fp, off, w)
#define ftell(fp)            rb_ftell(fp)
#define fgets(b,n,fp)        rb_fgets(b, n, fp)
#define feof(fp)             rb_feof(fp)
#define file_length(dum,fp)  rb_file_length(dum, fp)

/* =========================================================================
 * MEMORY ALLOCATION
 * =========================================================================
 * All dynamic memory comes from the sys_malloc() bump allocator, which is
 * backed by rb->plugin_get_audio_buffer() (~60 MB on iPod Classic 6G).
 * free() is a no-op: all GBA regions live for the plugin lifetime.
 *
 * Memory layout (approximate byte totals — see full breakdown below):
 *
 *   Region                           igpSP array size      Bytes
 *   ─────────────────────────────────────────────────────────────
 *   EWRAM  (extern, ×2 for mirror)   u8[1024*256*2]        524 288
 *   IWRAM  (extern, ×2 for mirror)   u8[1024*32*2]          65 536
 *   VRAM   (extern, ×2 for mirror)   u8[1024*96*2]         196 608
 *   Palette RAM                       u16[512]                1 024
 *   OAM RAM                           u16[512]                1 024
 *   BIOS ROM                          u8[1024*32]            32 768
 *   Gamepak backup/SRAM               u8[1024*128]          131 072
 *   ROM translation cache (JIT)       u8[1024*512*4]      2 097 152
 *   RAM translation cache             u8[1024*384]          393 216
 *   BIOS translation cache            u8[1024*128]          131 072
 *   ROM branch hash (64K × ptr)       u32*[1024*64]         262 144
 *   ─────────────────────────────────────────────────────────────
 *   Static BSS total (approx)                             ~3.8 MB
 *
 *   gamepak_rom  (dynamic, via malloc)   capped at 16 MB = 16 777 216
 *   GBA framebuffer 240×160×2                                 76 800
 *   Sound ring buf  4096 × 2 × 2                              16 384
 *   PCM DMA buf     1024 × 2                                   2 048
 *   ─────────────────────────────────────────────────────────────
 *   Grand total (with 16 MB ROM cap)                       ~20.7 MB
 *
 *   iPod Classic 6G audio buffer: ~60 MB → ~40 MB headroom ✓
 *
 *   If the BSS static arrays (3.8 MB) exceed the Rockbox plugin buffer
 *   on some future target, convert them to sys_malloc() pointers here:
 *     extern u8 *ewram;   (allocated in sys_mem_init via sys_malloc)
 *     #define ewram (*((u8(*)[...])_ewram_ptr))   ← array-access shim
 *   For the iPod Classic 6G this is not needed (plugin buffer is adequate).
 *
 *   Trim order if total memory is tight:
 *     1. ROM cap: reduce 16 MB → 8 MB  (only affects very large ROMs)
 *     2. ROM translation cache: reduce by half (impacts JIT cache hit rate)
 *     3. Gamepak backup: most games need ≤ 64 KB SRAM
 * ========================================================================= */

/* sys_malloc() declaration — defined in platform/rockbox/sys_rockbox_gba.c */
void *sys_malloc(size_t size);

/**
 * rb_calloc() — sys_malloc + zero-fill.
 * Inline so the compiler can elide the call when size is constant-zero.
 */
static inline void *rb_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void  *p     = sys_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

#define malloc(sz)      sys_malloc(sz)
#define calloc(n, sz)   rb_calloc((n), (sz))
#define free(p)         ((void)(p))   /* bump allocator — no free */
#define realloc(p, sz)  sys_malloc(sz) /* best-effort: alloc fresh block  */

/* =========================================================================
 * POSIX FILE DESCRIPTOR I/O
 * =========================================================================
 * igpSP uses open()/read()/write()/close()/lseek() in some code paths
 * (e.g. direct /dev/dsp writes on iPod Linux, backup save to raw fd).
 * Redirect all POSIX descriptor calls to our Rockbox wrappers.
 *
 * The variadic open() macro handles both 2-arg (O_RDONLY) and 3-arg
 * (O_CREAT, mode) forms safely — extra args are ignored by sys_open().
 * ========================================================================= */

int sys_open(const char *path, int flags);
int sys_read(int fd, void *buf, size_t len);
int sys_write(int fd, const void *buf, size_t len);
int sys_close(int fd);
int sys_seek(int fd, int offset, int whence);

/* open() takes 2 or 3 args; the variadic macro swallows the optional mode. */
#define open(path, flags, ...)   sys_open((path), (flags))
#define read(fd, buf, len)       sys_read((fd), (buf), (len))
#define write(fd, buf, len)      sys_write((fd), (buf), (len))
#define close(fd)                sys_close(fd)
#define lseek(fd, off, whence)   sys_seek((fd), (int)(off), (whence))

/* =========================================================================
 * TIMING
 * ========================================================================= */

void     sys_sleep_ms(int ms);
uint32_t sys_get_ticks(void);

/* igpSP uses delay_us() and get_ticks_us(); provided as platform functions
 * in sys_rockbox_gba.c (IPOD_BUILD path calls these directly from main.c). */

/* usleep / sleep — used in a few igpSP error paths */
#define usleep(us)   sys_sleep_ms((int)((unsigned long)(us) / 1000u))
#define sleep(s)     sys_sleep_ms((int)((s) * 1000))

/* =========================================================================
 * EXIT REDIRECTION
 * =========================================================================
 * igpSP's quit() calls exit(0) after platform teardown.  Rockbox plugins
 * must return from plugin_start(); calling the libc exit() (if it even
 * links) would bypass Rockbox cleanup and hang or corrupt the firmware.
 *
 * We redirect exit() to longjmp() back to the setjmp() guard in
 * plugin_start().  igpsp_exit_jmp is defined in igpsp.c and declared
 * extern here so all src/ translation units share the same jump target.
 * ========================================================================= */
extern jmp_buf igpsp_exit_jmp;
/* exit(code) → longjmp value 1 (0 is reserved — setjmp returns 0 on first call) */
#define exit(code)   longjmp(igpsp_exit_jmp, 1)

/* =========================================================================
 * SYMBOL RENAMING — avoid link conflicts with platform API
 * =========================================================================
 * igpSP's sound.c defines sound_exit() and sound_callback() which conflict
 * with our platform API symbols of the same name in sys_rockbox_gba.c.
 * The shim renames them inside all src/ files so both can coexist.
 *
 *   igpSP src/  defines:  igpsp_sound_exit()     ← renamed via macro
 *                          igpsp_sound_callback() ← renamed via macro
 *   Platform defines:      sound_exit()           ← unchanged, in sys_rockbox_gba.c
 *
 * igpsp.c (compiled WITHOUT this shim) calls platform sound_exit() directly.
 * igpSP's quit() (compiled WITH this shim) calls igpsp_sound_exit() which
 * handles GBA APU teardown and then calls ipod_exit_sound() — our hook.
 * ========================================================================= */
#define sound_exit      igpsp_sound_exit
#define init_sound      igpsp_init_sound
#define sound_callback  igpsp_sound_callback

/* =========================================================================
 * GETCWD STUB
 * =========================================================================
 * igpSP's main() calls getcwd(main_path, 512) to find the directory where
 * the binary lives (for relative BIOS path lookup).  In Rockbox, all paths
 * are absolute and the ROM path is passed directly; we set main_path to the
 * ROM's directory in igpsp.c before igpSP's init sequence runs.
 * Return a harmless "/" so the call doesn't crash if it still executes.
 * ========================================================================= */
static inline char *rb_getcwd(char *buf, size_t size)
{
    if (buf && size > 1) { buf[0] = '/'; buf[1] = '\0'; }
    return buf;
}
#define getcwd(buf, sz)  rb_getcwd((buf), (sz))

/* =========================================================================
 * PRINTF / DEBUG OUTPUT
 * =========================================================================
 * Rockbox plugins have no stdout/stderr.  Map printf family to DEBUGF which
 * is compiled out in release builds.  fprintf with a FILE* first arg is
 * silenced entirely (igpSP uses it for error messages to stderr).
 * ========================================================================= */
#ifndef DEBUGF
#  define DEBUGF(fmt, ...)  /* compiled out in release */
#endif
/* Map printf → DEBUGF.  Leave sprintf/snprintf alone — Rockbox's embedded
 * libc provides these and igpSP uses them for string formatting. */
#define printf(fmt, ...)          DEBUGF(fmt, ##__VA_ARGS__)
#define fprintf(fp, fmt, ...)     DEBUGF(fmt, ##__VA_ARGS__)

/* =========================================================================
 * DEBUG ALLOCATION LOGGING
 * =========================================================================
 * If IGPSP_DEBUG is defined at build time, route malloc/calloc/sys_malloc
 * through sys_malloc_debug() which logs each allocation and the remaining
 * heap via rb->splashf().  Useful during bringup to verify all GBA regions
 * fit within the audio buffer.
 * ========================================================================= */
#ifdef IGPSP_DEBUG
void *sys_malloc_debug(size_t size, const char *tag);

/* Override the earlier #defines with the debug version: */
#undef  malloc
#undef  calloc
#undef  sys_malloc
#define malloc(sz)      sys_malloc_debug((sz), __func__)
#define calloc(n, sz)   sys_malloc_debug((n) * (sz), __func__)
#define sys_malloc(sz)  sys_malloc_debug((sz), __func__)
#endif /* IGPSP_DEBUG */

#endif /* ROCKBOX_COMPAT_H */
