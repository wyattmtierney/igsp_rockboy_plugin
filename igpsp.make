#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# igpSP-Rockbox — Game Boy Advance emulator plugin
# igpsp.make — Rockbox build system integration
#
# INSTALLATION
# ─────────────────────────────────────────────────────────────────────────────
# 1. Copy the igpsp/ directory to $(APPSDIR)/plugins/igpsp/
# 2. Add this line to $(APPSDIR)/plugins/Makefile (near other .make includes):
#        include $(APPSDIR)/plugins/igpsp/igpsp.make
# 3. Populate $(APPSDIR)/plugins/igpsp/src/ with the igpSP source files from
#    https://github.com/iPodLinux-Community/igpSP (all .c and .h files from
#    the repo root).
# 4. Rebuild: make && make install
#    The plugin will appear at /.rockbox/rocks/games/igpsp.rock
#
# COMPILER FLAGS RATIONALE
# ─────────────────────────────────────────────────────────────────────────────
#   -marm
#       Compile the ENTIRE plugin as ARM32 (not Thumb).
#       CRITICAL: cpu_threaded.c's JIT dynarec emits raw 32-bit ARM opcodes
#       into a translation cache and then branches into that cache.  If the
#       dispatcher code itself is Thumb, every branch-to-cache target picks
#       up a +1 PC offset that corrupts all translated branch addresses.
#       Keeping everything ARM avoids the interworking complexity entirely.
#       The iPod 6G has 64 MB RAM so code-density savings from Thumb are
#       not worth the risk.
#
#   -O2 -fomit-frame-pointer
#       The GBA CPU emulation inner loop is extremely hot.  -O2 inlines
#       critical paths; -fomit-frame-pointer frees r11 for general use
#       (ARM926EJ-S has 16 registers; saving even one matters at 216 MHz).
#
#   -DROCKBOX   : platform discriminator used throughout igpSP source.
#   -DIPOD_BUILD: activates the igpSP iPod Linux code paths (no SDL, no PSP,
#                 no GP2X); ipod_* platform hooks provided by sys_rockbox_gba.c.
#   -DIPOD_6G   : target discriminator for iPod Classic 6G specifics.
#   -DLCD_WIDTH=320 -DLCD_HEIGHT=240 : host display dimensions.
#
# TWO FLAG SETS
# ─────────────────────────────────────────────────────────────────────────────
# IGPSP_CFLAGS      — used for igpsp.c and sys_rockbox_gba.c.
#                     These files use the real Rockbox API via plugin.h /
#                     sys_rockbox_gba.h and must NOT get the compatibility shim.
#
# IGPSP_CORE_CFLAGS — used for all igpSP src/ engine files.
#                     Adds -include rockbox_compat.h (the malloc/FILE*/exit
#                     shim) and -DIPOD_BUILD to steer igpSP's platform guards.
# ─────────────────────────────────────────────────────────────────────────────

IGPSP_SRCDIR := $(APPSDIR)/plugins/igpsp
IGPSP_OBJDIR := $(BUILDDIR)/apps/plugins/igpsp

# ─────────────────────────────────────────────────────────────────────────────
# Source files — plugin entry point and platform layer
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_SRC := \
	$(IGPSP_SRCDIR)/igpsp.c \
	$(IGPSP_SRCDIR)/platform/rockbox/sys_rockbox_gba.c

# ─────────────────────────────────────────────────────────────────────────────
# Source files — igpSP emulator core (Phase 5)
#
# These files are compiled with IGPSP_CORE_CFLAGS (includes the shim).
# All must exist in $(IGPSP_SRCDIR)/src/ — copy from the igpSP repo root:
#   https://github.com/iPodLinux-Community/igpSP
#
# input.c is intentionally omitted: our ipod_* hooks in sys_rockbox_gba.c
# provide all input functionality, replacing the iPod Linux input module.
#
# Phase 6 extras (optional — uncomment when ready):
#	$(IGPSP_SRCDIR)/src/cheats.c
#	$(IGPSP_SRCDIR)/src/zip.c
#	$(IGPSP_SRCDIR)/src/gui.c
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_CORE_SRC := \
	$(IGPSP_SRCDIR)/src/main.c \
	$(IGPSP_SRCDIR)/src/cpu.c \
	$(IGPSP_SRCDIR)/src/cpu_threaded.c \
	$(IGPSP_SRCDIR)/src/memory.c \
	$(IGPSP_SRCDIR)/src/video.c \
	$(IGPSP_SRCDIR)/src/sound.c

IGPSP_OBJ := \
	$(patsubst $(IGPSP_SRCDIR)/%.c, $(IGPSP_OBJDIR)/%.o, $(IGPSP_SRC)) \
	$(patsubst $(IGPSP_SRCDIR)/%.c, $(IGPSP_OBJDIR)/%.o, $(IGPSP_CORE_SRC))

OTHER_SRC += $(IGPSP_SRC) $(IGPSP_CORE_SRC)

# ─────────────────────────────────────────────────────────────────────────────
# Base compiler flags (shared between both flag sets)
#
# $(filter-out -mthumb -mthumb-interwork, ...) strips any Thumb flags that
# PLUGINFLAGS may inject for ARM targets, ensuring -marm always wins.
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_CFLAGS_BASE := \
	$(filter-out -mthumb -mthumb-interwork, $(PLUGINFLAGS)) \
	-marm \
	-O2 \
	-fomit-frame-pointer \
	-DROCKBOX \
	-DIPOD_6G \
	-DLCD_WIDTH=320 \
	-DLCD_HEIGHT=240 \
	-I$(IGPSP_SRCDIR) \
	-I$(IGPSP_SRCDIR)/platform/rockbox \
	-I$(APPSDIR)

# ─────────────────────────────────────────────────────────────────────────────
# IGPSP_CFLAGS — for igpsp.c and sys_rockbox_gba.c
#   No -include shim; these files call the real Rockbox API.
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_CFLAGS := $(IGPSP_CFLAGS_BASE)

# ─────────────────────────────────────────────────────────────────────────────
# IGPSP_CORE_CFLAGS — for igpSP src/ engine files
#
#   -include src/rockbox_compat.h
#       Injects the compatibility shim before any igpSP source file.
#       Redirects malloc→sys_malloc, fopen→rb_fopen, open→sys_open,
#       exit→longjmp, renames conflicting symbols, etc.
#       The shim is invisible to the core: igpSP source files compile
#       without knowing they are in a Rockbox environment.
#
#   -DIPOD_BUILD
#       Activates igpSP's iPod Linux code paths:
#         • SDL.h is NOT included (no SDL on Rockbox).
#         • file_open/read/write/close/seek macros expand to FILE* calls.
#         • ipod_* platform hooks are referenced from input.c, video.c,
#           sound.c instead of SDL / PSP / GP2X functions.
#
#   -fno-strict-aliasing
#       igpSP's memory access helpers cast u8* ↔ u16* ↔ u32* extensively.
#       Strict aliasing optimisation would break these casts.
#
#   -I$(IGPSP_SRCDIR)/src
#       Allows igpSP headers to include each other without path prefixes
#       (e.g. #include "memory.h" from within cpu_threaded.c).
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_CORE_CFLAGS := \
	$(IGPSP_CFLAGS_BASE) \
	-include $(IGPSP_SRCDIR)/src/rockbox_compat.h \
	-DIPOD_BUILD \
	-fno-strict-aliasing \
	-I$(IGPSP_SRCDIR)/src

# ─────────────────────────────────────────────────────────────────────────────
# Build targets
# ─────────────────────────────────────────────────────────────────────────────

ROCKS += $(IGPSP_OBJDIR)/igpsp.rock

$(IGPSP_OBJDIR)/igpsp.rock: $(IGPSP_OBJ)
	$(call PRINTS,LD $(@F))$(CC) $(PLUGINFLAGS) \
		-o $(basename $@).elf $(IGPSP_OBJ) -lgcc
	$(call objcopy_plugin,$(basename $@).elf,$@)

# ─────────────────────────────────────────────────────────────────────────────
# Pattern rule: plugin entry point and platform layer → IGPSP_CFLAGS
# (no shim, full Rockbox API access via plugin.h)
# ─────────────────────────────────────────────────────────────────────────────
$(IGPSP_OBJDIR)/igpsp.o: $(IGPSP_SRCDIR)/igpsp.c
	@mkdir -p $(@D)
	$(call PRINTS,CC $<)$(CC) $(IGPSP_CFLAGS) -c $< -o $@

$(IGPSP_OBJDIR)/platform/rockbox/sys_rockbox_gba.o: \
		$(IGPSP_SRCDIR)/platform/rockbox/sys_rockbox_gba.c
	@mkdir -p $(@D)
	$(call PRINTS,CC $<)$(CC) $(IGPSP_CFLAGS) -c $< -o $@

# ─────────────────────────────────────────────────────────────────────────────
# Pattern rule: igpSP core src/ files → IGPSP_CORE_CFLAGS (with shim)
# ─────────────────────────────────────────────────────────────────────────────
$(IGPSP_OBJDIR)/src/%.o: $(IGPSP_SRCDIR)/src/%.c
	@mkdir -p $(@D)
	$(call PRINTS,CC $<)$(CC) $(IGPSP_CORE_CFLAGS) -c $< -o $@

# ─────────────────────────────────────────────────────────────────────────────
# Special rule: cpu_threaded.c — ARM mode mandatory, fno-strict-aliasing kept
#
# This rule OVERRIDES the generic src/ pattern rule above for cpu_threaded.c.
# It strips -mthumb/-mthumb-interwork (belt-and-suspenders: they are already
# absent from IGPSP_CORE_CFLAGS, but this rule guarantees it unconditionally)
# and forces -marm -fno-strict-aliasing.
#
# WHY THIS MATTERS:
#   cpu_threaded.c contains the igpSP ARM JIT dynarec.  At runtime it:
#     1. Translates GBA ARM/THUMB opcodes into native ARM32 machine code.
#     2. Writes that code into a translation cache (rom_translation_cache[]).
#     3. Branches directly into the cache via a function pointer.
#   If the dispatcher itself is compiled as Thumb, the CPU bit-0 PC convention
#   causes every direct branch into the ARM translation cache to be
#   misinterpreted as a Thumb-mode entry, corrupting execution immediately.
# ─────────────────────────────────────────────────────────────────────────────
$(IGPSP_OBJDIR)/src/cpu_threaded.o: $(IGPSP_SRCDIR)/src/cpu_threaded.c
	@mkdir -p $(@D)
	$(call PRINTS,CC [ARM-forced] $<)$(CC) \
		$(filter-out -mthumb -mthumb-interwork, $(IGPSP_CORE_CFLAGS)) \
		-marm \
		-fno-strict-aliasing \
		-c $< -o $@
