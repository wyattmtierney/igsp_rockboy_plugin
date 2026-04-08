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
# 3. Rebuild: make && make install
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
#   -DIPOD_6G   : target discriminator for iPod Classic 6G specifics.
#   -DLCD_WIDTH=320 -DLCD_HEIGHT=240 : host display dimensions.
# ─────────────────────────────────────────────────────────────────────────────

IGPSP_SRCDIR := $(APPSDIR)/plugins/igpsp
IGPSP_OBJDIR := $(BUILDDIR)/apps/plugins/igpsp

# ─────────────────────────────────────────────────────────────────────────────
# Source files
#
# Platform layer (Phase 1 — implemented as stubs):
IGPSP_SRC := \
	$(IGPSP_SRCDIR)/igpsp.c \
	$(IGPSP_SRCDIR)/platform/rockbox/sys_rockbox_gba.c

# igpSP emulator core — uncomment each file as it is integrated.
# All src/ files must be compiled with -marm (see IGPSP_CFLAGS below).
#
# Phase 2 — memory map, PPU renderer:
#	$(IGPSP_SRCDIR)/src/memory.c
#	$(IGPSP_SRCDIR)/src/video.c
#
# Phase 4 — APU:
#	$(IGPSP_SRCDIR)/src/sound.c
#
# Phase 5 — CPU core + emulator loop:
#	$(IGPSP_SRCDIR)/src/main.c
#	$(IGPSP_SRCDIR)/src/cpu_threaded.c    # ← MUST stay -marm; see special rule
#	$(IGPSP_SRCDIR)/src/gba_memory.c
#	$(IGPSP_SRCDIR)/src/gba_io.c
#	$(IGPSP_SRCDIR)/src/gba_bios.c
#	$(IGPSP_SRCDIR)/src/thumb_handler.c
#
# Phase 6 — extras (optional):
#	$(IGPSP_SRCDIR)/src/zip.c
#	$(IGPSP_SRCDIR)/src/cheats.c
#	$(IGPSP_SRCDIR)/src/font.c
# ─────────────────────────────────────────────────────────────────────────────

IGPSP_OBJ := $(patsubst $(IGPSP_SRCDIR)/%.c, $(IGPSP_OBJDIR)/%.o, $(IGPSP_SRC))

OTHER_SRC += $(IGPSP_SRC)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler flags
#
# Start from Rockbox's standard PLUGINFLAGS (handles -fpic, -fno-builtin,
# include paths, etc.) and layer our additions on top.
#
# Note: $(filter-out -mthumb -mthumb-interwork, ...) strips any Thumb flags
# that PLUGINFLAGS may inject for ARM targets, ensuring -marm always wins.
# ─────────────────────────────────────────────────────────────────────────────
IGPSP_CFLAGS := \
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
# Build targets
# ─────────────────────────────────────────────────────────────────────────────

ROCKS += $(IGPSP_OBJDIR)/igpsp.rock

$(IGPSP_OBJDIR)/igpsp.rock: $(IGPSP_OBJ)
	$(call PRINTS,LD $(@F))$(CC) $(PLUGINFLAGS) \
		-o $(basename $@).elf $(IGPSP_OBJ) -lgcc
	$(call objcopy_plugin,$(basename $@).elf,$@)

# Generic pattern rule: compile any igpSP .c → .o with IGPSP_CFLAGS.
$(IGPSP_OBJDIR)/%.o: $(IGPSP_SRCDIR)/%.c
	@mkdir -p $(@D)
	$(call PRINTS,CC $<)$(CC) $(IGPSP_CFLAGS) -c $< -o $@

# ─────────────────────────────────────────────────────────────────────────────
# Special rule: cpu_threaded.c — ARM mode mandatory
#
# This rule OVERRIDES the generic pattern rule above for cpu_threaded.c.
# It strips -mthumb/-mthumb-interwork from IGPSP_CFLAGS (they should already
# be absent, but this is a belt-and-suspenders guarantee) and forces -marm.
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
		$(filter-out -mthumb -mthumb-interwork, $(IGPSP_CFLAGS)) \
		-marm \
		-c $< -o $@
