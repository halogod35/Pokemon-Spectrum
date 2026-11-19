# Pokémon Spectrum: the PC (SDL2) build of the pokeemerald decomp.
# The GBA ROM build (agbcc, devkitARM, `make compare`) was removed in PS-004; `make linux` and
# `make winwsl` are the only build targets. See INSTALL_PC.md.

KEEP_TEMPS  ?= 0
IS64BIT     ?= 1
TARGET_PLATFORM ?= PLATFORM_SDL2
TARGET_OS       ?= LINUX
TILE_RENDERER   ?= RENDERER_EASY_DRAW

# `File name` of the executable (the bit width and, on Windows, `.exe` are appended)
FILE_NAME := pokemon-spectrum
BUILD_DIR := build

# Always on. The PC build is the decomp's "portable" and "modern" (plain gcc) build; game code
# still tests both macros, so they keep their historical values.
PORTABLE := 1
MODERN   := 1

ifeq (winwsl,$(MAKECMDGOALS))
  TARGET_OS := WINDOWS
endif
ifeq (linux,$(MAKECMDGOALS))
  TARGET_OS := LINUX
endif

# Default make rule
all: rom

BIT_WIDTH := 32
OTHER_BIT_WIDTH := 64
ASFLAGS64 :=
CPPFLAGS64 :=

# 64 bit specific stuff goes here
ifeq ($(IS64BIT),1)
  BIT_WIDTH := 64
  OTHER_BIT_WIDTH := 32
  ASFLAGS64 :=
  CPPFLAGS64 := -D VER_64BIT
endif

# Toolchain selection: mingw-w64 cross compilers for Windows, the host gcc for Linux
ifeq ($(TARGET_OS),WINDOWS)
  ifeq ($(IS64BIT),1)
    PREFIX := x86_64-w64-mingw32-
  else
    PREFIX := i686-w64-mingw32-
  endif # IS64BIT
else # LINUX
  PREFIX :=
endif # TARGET_OS

OBJCOPY := $(PREFIX)objcopy
WINDRES := $(PREFIX)windres
AS := $(PREFIX)as

EXE :=
ifeq ($(OS),Windows_NT)
  EXE := .exe
endif

# SDL2 mingw development libraries, extracted to ./SDL2 (INSTALL_PC.md). Linux uses the system SDL2.
ifeq ($(TARGET_OS),WINDOWS)
  ifeq ($(IS64BIT),1)
    SDL_DIR := ./SDL2/x86_64-w64-mingw32
  else
    SDL_DIR := ./SDL2/i686-w64-mingw32
  endif
endif

ASM_PSEUDO_OP_CONV := sed -e 's/\.4byte/\.int/g;s/\.2byte/\.short/g'
#FIX_UNDERSCORE is required for 32 bit windows
ifeq ($(TARGET_OS)$(IS64BIT),WINDOWS0)
  FIX_UNDERSCORE := $(OBJCOPY) --prefix-symbol _
  LEADING_UNDERSCORE_FLAG := -fleading-underscore
else
  FIX_UNDERSCORE := $(OBJCOPY)
  LEADING_UNDERSCORE_FLAG :=
endif

PLATFORM_LFLAGS :=
PLATFORM_INCLUDES :=
PLATFORM_CFLAGS :=
BUILD_FEXTENSION :=
OS_CFLAGS :=
OS_LFLAGS :=

ifeq ($(TARGET_OS),WINDOWS)
  OS_CFLAGS :=
  # PS-083: no -lxinput. XInput is loaded at run time (sdl2.c) so the exe starts on Windows XP / without XINPUT1_3.dll.
  OS_LFLAGS := -lwinmm -Wl,-Map,pokemon-spectrum.map # MP: map file for the crash reporter (BUG-003)
  BUILD_FEXTENSION := .exe
else
  OS_CFLAGS :=
  OS_LFLAGS := -no-pie
endif

#Windows only
ifeq ($(TARGET_OS),WINDOWS)
  ifneq ($(NO_STD_LIB),1)
    PLATFORM_LFLAGS += -lmingw32
  endif
endif

ifeq ($(TARGET_PLATFORM), PLATFORM_SDL2)
  PLATFORM_LFLAGS += -lSDL2main -lSDL2 -lm
  # SDL headers pull in x86 intrinsics headers, which break under -fno-builtin with GCC 15.
  # We never use SDL intrinsics, so disable them for every SDL2 build (was 32-bit only).
  PLATFORM_INCLUDES += -DSDL_DISABLE_IMMINTRIN_H -DSDL_DISABLE_MMINTRIN_H -DSDL_DISABLE_XMMINTRIN_H -DSDL_DISABLE_EMMINTRIN_H -DSDL_DISABLE_PMMINTRIN_H
endif

ifeq ($(TARGET_PLATFORM), PLATFORM_WIN32)
  ifeq ($(NO_STD_LIB),1)
    PLATFORM_LFLAGS += -Wl,-e__main -nostdlib
    CPPFLAGS += -D NO_STD_LIB_ENABLED
  endif
  PLATFORM_LFLAGS += -lkernel32 -luser32 -lgdi32
  PLATFORM_INCLUDES += -D SOUND_DISABLED
endif

ROM := $(FILE_NAME)$(BIT_WIDTH)$(BUILD_FEXTENSION)
OBJ_DIR := $(BUILD_DIR)/pc$(BIT_WIDTH)
ROM_OTHER := $(FILE_NAME)$(OTHER_BIT_WIDTH)$(BUILD_FEXTENSION)
OBJ_DIR_OTHER := $(BUILD_DIR)/pc$(OTHER_BIT_WIDTH)
ASSETS_DIR_NAME := $(BUILD_DIR)/assets

# Commonly used directories
C_SUBDIR = src
ASM_SUBDIR = asm
DATA_SRC_SUBDIR = src/data
DATA_ASM_SUBDIR = data
MID_SUBDIR = sound/songs/midi

C_BUILDDIR = $(OBJ_DIR)/$(C_SUBDIR)
ASM_BUILDDIR = $(OBJ_DIR)/$(ASM_SUBDIR)
DATA_ASM_BUILDDIR = $(OBJ_DIR)/$(DATA_ASM_SUBDIR)
MID_BUILDDIR = $(OBJ_DIR)/$(MID_SUBDIR)

SHELL := bash -o pipefail

# Set flags for tools
ASFLAGS := --$(BIT_WIDTH) $(ASFLAGS64) --defsym VER_64BIT=$(IS64BIT) --defsym MODERN=$(MODERN) --defsym PORTABLE=1 --defsym UBFIX=1

INCLUDE_DIRS := include
INCLUDE_CPP_ARGS := $(INCLUDE_DIRS:%=-iquote %)
INCLUDE_SCANINC_ARGS := $(INCLUDE_DIRS:%=-I %)

CPPFLAGS := $(INCLUDE_CPP_ARGS) -Wno-trigraphs -DMODERN=$(MODERN)
CPPFLAGS += -std=gnu99 -D NONMATCHING -D PORTABLE -D $(TARGET_PLATFORM) -D $(TILE_RENDERER) -D UBFIX $(CPPFLAGS64) $(PLATFORM_INCLUDES) -I$(SDL_DIR)/include -L$(SDL_DIR)/lib
MODERNCC := $(PREFIX)gcc
CPP := $(PREFIX)cpp -m$(BIT_WIDTH)
CC1 	:= $(shell $(PREFIX)gcc --print-prog-name=cc1) -quiet
override CFLAGS += $(OS_CFLAGS) $(PLATFORM_CFLAGS) -Werror=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-trigraphs -Wimplicit -Wparentheses -Wunused -m$(BIT_WIDTH) -std=gnu99 $(LEADING_UNDERSCORE_FLAG) -fno-dce -fno-builtin -Wno-unused-function -DPORTABLE -DNONMATCHING -D UBFIX -DMODERN=$(MODERN)

# Enable debug info if set
ifeq ($(DINFO),1)
  override CFLAGS += -g -O0
else
  override CFLAGS += -O3
endif

# Variable filled out in other make files
AUTO_GEN_TARGETS :=
include make_tools.mk
# Tool executables
GFX       := $(TOOLS_DIR)/gbagfx/gbagfx$(EXE)
WAV2AGB   := $(TOOLS_DIR)/wav2agb/wav2agb$(EXE)
MID       := $(TOOLS_DIR)/mid2agb/mid2agb$(EXE)
SCANINC   := $(TOOLS_DIR)/scaninc/scaninc$(EXE)
PREPROC   := $(TOOLS_DIR)/preproc/preproc$(EXE)
MAPJSON   := $(TOOLS_DIR)/mapjson/mapjson$(EXE)
JSONPROC  := $(TOOLS_DIR)/jsonproc/jsonproc$(EXE)

MAKEFLAGS += --no-print-directory

# Clear the default suffixes
.SUFFIXES:
# Don't delete intermediate files
.SECONDARY:
# Delete files that weren't built properly
.DELETE_ON_ERROR:

RULES_NO_SCAN += clean clean-assets tidy tidyportable clean-platform generated clean-generated checkspecials
.PHONY: all rom winwsl linux
.PHONY: $(RULES_NO_SCAN)

infoshell = $(foreach line, $(shell $1 | sed "s/ /__SPACE__/g"), $(info $(subst __SPACE__, ,$(line))))

# Check if we need to scan dependencies based on the chosen rule OR user preference
NODEP ?= 0
# Check if we need to pre-build tools and generate assets based on the chosen rule.
SETUP_PREREQS ?= 1
# Disable dependency scanning for rules that don't need it.
ifneq (,$(MAKECMDGOALS))
  ifeq (,$(filter-out $(RULES_NO_SCAN),$(MAKECMDGOALS)))
    NODEP := 1
    SETUP_PREREQS := 0
  endif
endif

.SHELLSTATUS ?= 0

ifeq ($(SETUP_PREREQS),1)
  # If set on: Default target or a rule requiring a scan
  # Forcibly execute `make tools` since we need them for what we are doing.
  $(foreach line, $(shell $(MAKE) -f make_tools.mk | sed "s/ /__SPACE__/g"), $(info $(subst __SPACE__, ,$(line))))
  ifneq ($(.SHELLSTATUS),0)
    $(error Errors occurred while building tools. See error messages above for more details)
  endif
  # Oh and also generate mapjson sources before we use `SCANINC`.
  $(foreach line, $(shell $(MAKE) generated | sed "s/ /__SPACE__/g"), $(info $(subst __SPACE__, ,$(line))))
  ifneq ($(.SHELLSTATUS),0)
    $(error Errors occurred while generating map-related sources. See error messages above for more details)
  endif
endif

# Collect sources
C_SRCS_IN := $(wildcard $(C_SUBDIR)/*.c $(C_SUBDIR)/*/*.c $(C_SUBDIR)/*/*/*.c)
C_SRCS := $(foreach src,$(C_SRCS_IN),$(if $(findstring .inc.c,$(src)),,$(src)))
C_OBJS := $(patsubst $(C_SUBDIR)/%.c,$(C_BUILDDIR)/%.o,$(C_SRCS))

ASM_SRCS := $(wildcard $(ASM_SUBDIR)/*.s)
ASM_OBJS := $(patsubst $(ASM_SUBDIR)/%.s,$(ASM_BUILDDIR)/%.o,$(ASM_SRCS))

DATA_ASM_SRCS := $(wildcard $(DATA_ASM_SUBDIR)/*.s)
DATA_ASM_OBJS := $(patsubst $(DATA_ASM_SUBDIR)/%.s,$(DATA_ASM_BUILDDIR)/%.o,$(DATA_ASM_SRCS))

MID_SRCS := $(wildcard $(MID_SUBDIR)/*.mid)
MID_OBJS := $(patsubst $(MID_SUBDIR)/%.mid,$(MID_BUILDDIR)/%.o,$(MID_SRCS))

OBJS     := $(C_OBJS) $(ASM_OBJS) $(DATA_ASM_OBJS) $(MID_OBJS)

ifeq ($(TARGET_OS),WINDOWS)
  OBJS += $(OBJ_DIR)/res.o
endif

# MP: multiplayer (PS-011). src/mp/*.c is already collected by C_SRCS_IN above.
# ENet (libs/enet, vendored 1.3.18, MIT) is plain C: it gets its own rule below,
# without preproc and without the game's warning flags.
ENET_DIR    := libs/enet
ENET_SRCS   := $(addprefix $(ENET_DIR)/,callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c)
ENET_OBJS   := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ENET_SRCS))
ENET_CFLAGS := -O2 -std=gnu99 -m$(BIT_WIDTH) $(LEADING_UNDERSCORE_FLAG) -I $(ENET_DIR)/include -w
ifeq ($(TARGET_OS),WINDOWS)
  OS_LFLAGS   += -lws2_32
else
  # Replaces ENet's configure step (unix.c only defaults these on __APPLE__).
  ENET_CFLAGS += -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 \
                 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1
endif
OBJS += $(ENET_OBJS)

# Build id for the multiplayer handshake (docs/multiplayer/protocol.md section 9).
# Only src/mp/mp_build.c sees it; a stamp file rebuilds that one file when it changes.
MP_GIT_ID      := $(shell git rev-parse HEAD 2>/dev/null | cut -c1-8)
MP_BUILD_ID    ?= $(if $(MP_GIT_ID),$(MP_GIT_ID),nogit)
MP_BUILD_DIRTY := $(if $(shell git status --porcelain --untracked-files=no 2>/dev/null),1,0)
MP_BUILD_STAMP := $(OBJ_DIR)/mp_build_id.txt
$(shell mkdir -p $(OBJ_DIR); echo '$(MP_BUILD_ID) $(MP_BUILD_DIRTY)' | cmp -s - $(MP_BUILD_STAMP) || echo '$(MP_BUILD_ID) $(MP_BUILD_DIRTY)' > $(MP_BUILD_STAMP))

SUBDIRS  := $(sort $(dir $(OBJS)))
$(shell mkdir -p $(SUBDIRS))

# Pretend rules that are actually flags defer to `make all`
winwsl: all
linux: all

# Other rules
rom: $(ROM)

clean: tidy clean-tools clean-generated clean-assets

clean-assets:
	rm -rf $(ASSETS_DIR_NAME)
	rm -f $(MID_SUBDIR)/*.s
	rm -f $(DATA_ASM_SUBDIR)/layouts/layouts.inc $(DATA_ASM_SUBDIR)/layouts/layouts_table.inc
	rm -f $(DATA_ASM_SUBDIR)/maps/connections.inc $(DATA_ASM_SUBDIR)/maps/events.inc $(DATA_ASM_SUBDIR)/maps/groups.inc $(DATA_ASM_SUBDIR)/maps/headers.inc
	find sound -iname '*.bin' -exec rm {} +
	find . \( -iname '*.1bpp' -o -iname '*.4bpp' -o -iname '*.8bpp' -o -iname '*.gbapal' -o -iname '*.lz' -o -iname '*.rl' -o -iname '*.latfont' -o -iname '*.hwjpnfont' -o -iname '*.fwjpnfont' \) -exec rm {} +
	find $(DATA_ASM_SUBDIR)/maps \( -iname 'connections.inc' -o -iname 'events.inc' -o -iname 'header.inc' \) -exec rm {} +

tidy: tidyportable

tidyportable:
	rm -f $(ROM)
	rm -f $(ROM).exe
	rm -rf $(OBJ_DIR)
	rm -f $(ROM_OTHER)
	rm -f $(ROM_OTHER).exe
	rm -rf $(OBJ_DIR_OTHER)

clean-platform:
	rm -f $(ROM)
	rm -rf $(OBJ_DIR)/src/platform

# PS-090: fail if any script special returns bool8/u8/s8 (specialvar reads 16 bits; x86 leaves the upper byte undefined). Also a CI step.
checkspecials:
	bash tools/check_specials.sh

# Other rules
include graphics_file_rules.mk
include map_data_rules.mk
include json_data_rules.mk
include audio_rules.mk

# NOTE: Tools must have been built prior (FIXME)
# so you can't really call this rule directly
generated: $(AUTO_GEN_TARGETS)
	@: # Silence the "Nothing to be done for `generated'" message, which some people were confusing for an error.


%.s:   ;
%.png: ;
%.pal: ;
%.wav: ;

%.1bpp:   %.png  ; $(GFX) $< $@
%.4bpp:   %.png  ; $(GFX) $< $@
%.8bpp:   %.png  ; $(GFX) $< $@
%.gbapal: %.pal  ; $(GFX) $< $@
%.gbapal: %.png  ; $(GFX) $< $@
%.lz:     %      ; $(GFX) $< $@
%.rl:     %      ; $(GFX) $< $@

clean-generated:
	@rm -f $(AUTO_GEN_TARGETS)
	@echo "rm -f <AUTO_GEN_TARGETS>"

# Dependency rules (for the *.c & *.s sources to .o files)
# Have to be explicit or else missing files won't be reported.

# As a side effect, they're evaluated immediately instead of when the rule is invoked.
# It doesn't look like $(shell) can be deferred so there might not be a better way (Icedude_907: there is soon).

$(C_BUILDDIR)/%.o: $(C_SUBDIR)/%.c
ifneq ($(KEEP_TEMPS),1)
	@echo "$(CC1) <flags> -o $@ $<"
	@$(CPP) $(CPPFLAGS) $< | $(PREPROC) -i -g $(ASSETS_DIR_NAME) $< charmap.txt | $(CC1) $(CFLAGS) -o - - | cat - <(echo -e ".text\n\t.align\t2, 0") | $(AS) $(ASFLAGS) -o $@ -
else
	@$(CPP) $(CPPFLAGS) $< -o $(C_BUILDDIR)/$*.i
	@$(PREPROC) -g $(ASSETS_DIR_NAME) $(C_BUILDDIR)/$*.i charmap.txt | $(CC1) $(CFLAGS) -o $(C_BUILDDIR)/$*.s
	@echo -e ".text\n\t.align\t2, 0\n" >> $(C_BUILDDIR)/$*.s
	$(AS) $(ASFLAGS) -o $@ $(C_BUILDDIR)/$*.s
endif

$(C_BUILDDIR)/%.d: $(C_SUBDIR)/%.c
	$(SCANINC) -M $@ -g $(ASSETS_DIR_NAME) $(INCLUDE_SCANINC_ARGS) $<

# MP: multiplayer build rules (PS-011)
$(OBJ_DIR)/$(ENET_DIR)/%.o: $(ENET_DIR)/%.c $(wildcard $(ENET_DIR)/include/enet/*.h)
	@echo "$(MODERNCC) <enet flags> -o $@ $<"
	@$(MODERNCC) $(ENET_CFLAGS) -c -o $@ $<

# Only the transport sees the ENet headers.
$(C_BUILDDIR)/mp/mp_transport.o: CPPFLAGS += -I $(ENET_DIR)/include
$(C_BUILDDIR)/mp/mp_transport.o: $(wildcard $(ENET_DIR)/include/enet/*.h)

$(C_BUILDDIR)/mp/mp_build.o: CPPFLAGS += -DMP_BUILD_ID=\"$(MP_BUILD_ID)\" -DMP_BUILD_DIRTY=$(MP_BUILD_DIRTY)
$(C_BUILDDIR)/mp/mp_build.o: $(MP_BUILD_STAMP)

ifneq ($(NODEP),1)
-include $(addprefix $(OBJ_DIR)/,$(C_SRCS:.c=.d))
endif

$(ASM_BUILDDIR)/%.o: $(ASM_SUBDIR)/%.s
	$(AS) $(ASFLAGS) -o $@ $<
	$(FIX_UNDERSCORE) $@

$(ASM_BUILDDIR)/%.d: $(ASM_SUBDIR)/%.s
	$(SCANINC) -M $@ -g $(ASSETS_DIR_NAME) $(INCLUDE_SCANINC_ARGS) -I "" $<

ifneq ($(NODEP),1)
-include $(addprefix $(OBJ_DIR)/,$(ASM_SRCS:.s=.d))
endif

$(DATA_ASM_BUILDDIR)/%.o: $(DATA_ASM_SUBDIR)/%.s
	$(PREPROC) $< charmap.txt | $(CPP) $(INCLUDE_SCANINC_ARGS) - | $(PREPROC) -ie $< charmap.txt | $(ASM_PSEUDO_OP_CONV) | $(AS) $(ASFLAGS) -o $@
	$(FIX_UNDERSCORE) $@

$(DATA_ASM_BUILDDIR)/%.d: $(DATA_ASM_SUBDIR)/%.s
	$(SCANINC) -M $@ -g $(ASSETS_DIR_NAME) $(INCLUDE_SCANINC_ARGS) -I "" $<

ifneq ($(NODEP),1)
-include $(addprefix $(OBJ_DIR)/,$(DATA_ASM_SRCS:.s=.d))
endif

$(OBJ_DIR)/res.o: $(C_SUBDIR)/platform/win32res/res.rc $(C_SUBDIR)/platform/win32res/icon.ico
	$(WINDRES) $< -o $@

# Final rule: link the executable
$(ROM): $(OBJS)
	$(MODERNCC) $(CFLAGS) -Wl,--demangle $^ -static-libgcc -L$(SDL_DIR)/lib $(PLATFORM_LFLAGS) $(OS_LFLAGS) -o $@
