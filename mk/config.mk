# geist-watch build configuration.
#
# GNU Make 3.81+. No target here touches the network and none needs the engine:
# the model-free core, its tests and the CLI contracts build from this file
# alone. `make deps` and `make setup` (added with W04/W06) are the only steps
# that will fetch anything, and they are always explicit.
#
# Compiler floor is C23 as the project plan specifies: GCC 14+ or Clang 19+.
# Clang 18 compiles everything here except `constexpr`, so the source avoids it
# and a Clang 18 host still builds — but the supported floor is what CI gates.

MODE   ?= release
TARGET ?= $(shell uname -s | tr A-Z a-z | sed 's/darwin/darwin/')-$(shell uname -m | sed 's/x86_64/x86_64/;s/aarch64/aarch64/;s/arm64/arm64/')
PREFIX ?= /usr/local
AR     ?= ar
RANLIB ?= ranlib
CXX    ?= c++

# pi5 is a deliberate TARGET rather than a flavour of linux-aarch64: it turns on
# -mcpu=cortex-a76, and a generic ARM64 board must never inherit that. Keeping
# them separate is why the plan asks for it.
VALID_TARGETS := linux-x86_64 linux-aarch64 pi5 darwin-arm64
VALID_MODES   := release debug asan

ifeq ($(filter $(TARGET),$(VALID_TARGETS)),)
$(error Unsupported TARGET=$(TARGET); use one of: $(VALID_TARGETS))
endif
ifeq ($(filter $(MODE),$(VALID_MODES)),)
$(error Unsupported MODE=$(MODE); use one of: $(VALID_MODES))
endif

# Trust the compiler's own triple over the requested TARGET. A cross build that
# silently produced host objects would pass every test here and fail on the
# board, which is the failure this check exists to prevent.
COMPILER_TARGET := $(shell $(CC) $(CFLAGS) -dumpmachine 2>/dev/null)
ifeq ($(COMPILER_TARGET),)
$(error $(CC) does not answer -dumpmachine; set CC to a working compiler)
endif
ifneq ($(filter darwin-%,$(TARGET)),)
ifeq ($(findstring darwin,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) needs a macOS compiler; $(CC) reports $(COMPILER_TARGET))
endif
else
ifeq ($(findstring linux,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) needs a Linux compiler; $(CC) reports $(COMPILER_TARGET))
endif
endif
ifneq ($(filter %x86_64,$(TARGET)),)
ifeq ($(filter x86_64%,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) needs an x86-64 compiler; $(CC) reports $(COMPILER_TARGET))
endif
else
ifeq ($(filter arm64% aarch64%,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) needs an ARM64 compiler; $(CC) reports $(COMPILER_TARGET))
endif
endif

ifeq ($(MODE),release)
MODE_FLAGS := -O2 -DNDEBUG
else ifeq ($(MODE),debug)
MODE_FLAGS := -O0 -g3
else
MODE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_FLAGS  := -fsanitize=address,undefined
endif

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
        -Wimplicit-fallthrough -Wvla -Werror

# _POSIX_C_SOURCE for clock_gettime(CLOCK_MONOTONIC); _DARWIN_C_SOURCE and
# _DEFAULT_SOURCE because glibc and Apple libc each gate parts of that behind
# their own feature macro.
# The supported floor spells this c23. GCC 13 and Clang 18 implement
# enough of the same standard to build this tree but only answer to the
# older c2x spelling, so the flag is a knob rather than a literal — it
# lets a contributor on a distribution compiler run the model-free tests
# without lowering what CI gates.
CSTD ?= c23

BASE_FLAGS := -std=$(CSTD) -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
              -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $(WARN) $(MODE_FLAGS)

ifeq ($(TARGET),pi5)
BASE_FLAGS += -mcpu=cortex-a76
endif

PROJECT_LIBS := $(LDLIBS)

# Build directories are derived from the configuration, so switching TARGET,
# MODE, compiler or flags cannot leave stale objects behind in a shared tree.
CONFIG := $(shell printf '%s\n' \
    '$(CC)|$(shell $(CC) --version 2>/dev/null | head -1)|$(TARGET)|$(MODE)|$(CPPFLAGS)|$(CFLAGS)|$(LDFLAGS)|$(LDLIBS)|$(AR)|$(RANLIB)|$(CSTD)|$(COMPILER_TARGET)|$(shell cksum Makefile mk/config.mk 2>/dev/null)' \
    | cksum | cut -d' ' -f1)
BUILD := build/$(TARGET)/$(MODE)/$(CONFIG)
LIB   := $(BUILD)/libgeist_watch.a
