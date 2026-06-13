# psxport runtime ("wide60rt"): compiles the mednafen/Beetle PSX core sources
# (vendor/beetle-psx, GPL-2) directly into our own binary — no prebuilt core,
# no .so. Source lists come from the vendored Makefile.common; flags mirror
# the upstream unix build minus the dynarec (interpreter-only: HAVE_LIGHTREC=0,
# we hook/override interpreted code by PC).
# Beetle's libretro.cpp currently serves as init glue; it will be replaced by
# native glue as the override layer grows.

BEETLE   := ../vendor/beetle-psx
CORE_DIR := $(BEETLE)
# Overridable so parallel builds don't clobber each other: e.g. two concurrent
# agents can `make OBJDIR=obj_a EXE=wide60rt_a` and `make OBJDIR=obj_b EXE=wide60rt_b`
# in the same checkout (no git worktree needed).
OBJDIR   ?= obj
EXE      ?= wide60rt

HAVE_LIGHTREC = 0
HAVE_CDROM    = 0
NEED_CD       = 1
NEED_TREMOR   = 1
NEED_DEINTERLACER = 1
HAVE_CHD      = 1
HAVE_PBP      = 1
WANT_32BPP    = 1
THREADED      = 1

include $(BEETLE)/Makefile.common

DEFINES := -DNDEBUG -DHAVE_PBP -DWANT_THREADING -DHAVE_THREADS \
           -DNEED_DEINTERLACER -DWANT_32BPP -DNEED_CD -DHAVE_CHD \
           -DCHDR_SYSTEM_ZLIB -DNEED_TREMOR -DLOG_LEVEL=3 \
           -DENABLE_DISASSEMBLER=0 -DMEDNAFEN_VERSION_NUMERIC=9386 \
           -DSTDC_HEADERS -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS \
           -D__LIBRETRO__ -D_LOW_ACCURACY_ -D_FILE_OFFSET_BITS=64 \
           -DPSXPORT_HOOKS

WARNINGS := -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function \
            -Wno-uninitialized -Wno-strict-aliasing

CFLAGS_COMMON := -O2 -g -fwrapv -fsigned-char $(INCFLAGS) -I$(BEETLE)/include -I. \
                 -include $(BEETLE)/include/debug.h $(DEFINES) $(WARNINGS)
CFLAGS   := $(CFLAGS_COMMON)
CXXFLAGS := $(CFLAGS_COMMON) -std=c++14
# SDL2: prefer pkg-config, fall back to sdl2-config (Homebrew/macOS ships both).
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS  := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)

# Platform link flags. librt/libdl are Linux-only; on macOS clock_gettime and
# dlopen live in libc, so linking them fails ("library not found for -lrt").
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  PLATFORM_LIBS := -lpthread
else
  PLATFORM_LIBS := -lpthread -ldl -lrt
endif
LDFLAGS  := $(PLATFORM_LIBS) $(SDL_LIBS)

# frontend-side libretro-common implementations the core expects from the host
LRC := $(BEETLE)/libretro-common
HOST_SRCS_C := $(LRC)/streams/file_stream.c \
               $(LRC)/streams/file_stream_transforms.c \
               $(LRC)/vfs/vfs_implementation.c \
               $(LRC)/string/stdstring.c \
               $(LRC)/time/rtime.c \
               $(LRC)/file/file_path.c \
               $(LRC)/compat/compat_strl.c \
               $(LRC)/compat/compat_strcasestr.c \
               $(LRC)/file/retro_dirent.c \
               $(LRC)/compat/compat_posix_string.c \
               $(LRC)/compat/fopen_utf8.c \
               $(LRC)/encodings/encoding_utf.c \
               $(LRC)/rthreads/rthreads.c

ALL_C   := $(sort $(SOURCES_C) $(HOST_SRCS_C))
ALL_CXX := $(sort $(SOURCES_CXX))

# core sources live under ../vendor/beetle-psx; map them into obj/core/ so
# no object ever lands outside obj/ (".." in an obj path escapes the tree)
OBJS := $(patsubst $(BEETLE)/%.c,$(OBJDIR)/core/%.o,$(ALL_C)) \
        $(patsubst $(BEETLE)/%.cpp,$(OBJDIR)/core/%.o,$(ALL_CXX)) \
        $(OBJDIR)/main.o $(OBJDIR)/psxport_hooks.o $(OBJDIR)/wide60.o $(OBJDIR)/games/tomba2.o \
        $(OBJDIR)/hle_iso.o $(OBJDIR)/hle_syscalls.o $(OBJDIR)/hle_kernel.o \
        $(OBJDIR)/hle_irq.o

$(EXE): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(OBJDIR)/main.o: main.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $(SDL_CFLAGS) -o $@ $<

$(OBJDIR)/wide60.o: wide60.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/psxport_hooks.o: psxport_hooks.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/games/tomba2.o: games/tomba2.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/hle_iso.o: hle_iso.cpp hle_bios.h
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/hle_syscalls.o: hle_syscalls.cpp hle_bios.h
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/hle_kernel.o: hle_kernel.cpp hle_bios.h
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/hle_irq.o: hle_irq.cpp hle_bios.h psxport_hooks.h
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/core/%.o: $(BEETLE)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(OBJDIR)/core/%.o: $(BEETLE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(OBJDIR) $(EXE)

.PHONY: clean
