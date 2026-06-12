# psxport runtime ("wide60rt"): compiles the mednafen/Beetle PSX core sources
# (vendor/beetle-psx, GPL-2) directly into our own binary — no prebuilt core,
# no .so. Source lists come from the vendored Makefile.common; flags mirror
# the upstream unix build minus the dynarec (interpreter-only: HAVE_LIGHTREC=0,
# we hook/override interpreted code by PC).
# Beetle's libretro.cpp currently serves as init glue; it will be replaced by
# native glue as the override layer grows.

BEETLE   := ../vendor/beetle-psx
CORE_DIR := $(BEETLE)
OBJDIR   := obj

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
           -D__LIBRETRO__ -D_LOW_ACCURACY_ -D_FILE_OFFSET_BITS=64

WARNINGS := -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function \
            -Wno-uninitialized -Wno-strict-aliasing

CFLAGS_COMMON := -O2 -g -fwrapv -fsigned-char $(INCFLAGS) -I$(BEETLE)/include \
                 -include $(BEETLE)/include/debug.h $(DEFINES) $(WARNINGS)
CFLAGS   := $(CFLAGS_COMMON)
CXXFLAGS := $(CFLAGS_COMMON) -std=c++14
LDFLAGS  := -lpthread -ldl -lrt

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

OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(ALL_C)) \
        $(patsubst %.cpp,$(OBJDIR)/%.o,$(ALL_CXX)) \
        $(OBJDIR)/main.o

wide60rt: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(OBJDIR)/main.o: main.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(OBJDIR) wide60rt

.PHONY: clean
