obj-m += c985.o

c985-y := c985_main.o c985_irq.o c985_dma.o c985_fw.o c985_mbox.o \
         c985_v4l2.o c985_audio.o c985_debugfs.o c985_nuc100.o cpr.o

# ---------------------------------------------------------------------------
# Out-of-tree module build
#
# An out-of-tree module must be built with the same compiler family as the
# kernel it will be loaded into: kbuild bakes the target kernel's flags into
# KBUILD_CFLAGS, and a gcc-only flag like -mpreferred-stack-boundary=3 (or a
# clang-only one like -mstack-alignment=8) is a hard error for the wrong
# compiler.
#
# The trap: the kernel build tree does NOT tell you which compiler to use.
# It injects the right *flags* (via include/config/auto.conf), but its
# Makefile still defaults to
#
#     ifneq ($(LLVM),)   CC = clang    LD = ld.lld  AR = llvm-ar ...
#     else               CC = $(CROSS_COMPILE)gcc     LD = ld  AR = ar
#
# i.e. compiler selection is keyed on $(LLVM) being non-empty, *not* on
# CONFIG_CC_IS_CLANG. So a clang-built kernel's build dir will happily hand you
# clang-only flags and then invoke gcc. Passing no LLVM=1 therefore fails with
#     gcc: error: unrecognized command-line option '-mstack-alignment=8'
# while passing LLVM=1 to a gcc-built kernel fails the mirror-image way.
#
# So we detect the target kernel's compiler family ourselves and pass LLVM=1
# only when it is actually needed. Detection order:
#   1. $(CC_FAMILY) if given on the command line (clang | gcc)
#   2. CONFIG_CC_IS_{CLANG,GCC} in $(KDIR)/include/config/auto.conf
#   3. LINUX_COMPILER in $(KDIR)/include/generated/compile.h  (headers that
#      were installed but never built have no auto.conf)
#   4. give up, assume gcc, and say so loudly
#
#   make                        # build for the running kernel
#   make KVER=7.2.7-1-cachyos   # build for another installed kernel
#   make CC_FAMILY=gcc          # override detection
#   make KBUILD_FLAGS=W=1       # extra kbuild flags
# ---------------------------------------------------------------------------

KVER   ?= $(shell uname -r)
KDIR   ?= /lib/modules/$(KVER)/build
CC_FAMILY ?= auto
KBUILD_FLAGS ?=

# --- compiler family detection ---------------------------------------------
ifeq ($(CC_FAMILY),auto)
  KCONFIG := $(wildcard $(KDIR)/include/config/auto.conf)
  ifneq ($(KCONFIG),)
    DETECTED := $(shell sed -n 's/^CONFIG_CC_IS_\([A-Z]*\)=y$$/\1/p' $(KCONFIG))
  endif
  ifeq ($(strip $(DETECTED)),)
    DETECTED := $(shell sed -n 's/^#define[[:space:]]*LINUX_COMPILER[[:space:]]*"\([a-zA-Z]*\).*/\1/p' \
                             $(KDIR)/include/generated/compile.h 2>/dev/null)
  endif
else
  DETECTED := $(CC_FAMILY)
endif

ifeq ($(DETECTED),CLANG)
  DETECTED := clang
else ifeq ($(DETECTED),GCC)
  DETECTED := gcc
endif

ifeq ($(strip $(DETECTED)),)
  DETECTED  := unknown
  DETECT_W  := warn
endif

ifeq ($(DETECTED),clang)
  LLVM_FLAG := LLVM=1
else
  LLVM_FLAG :=
endif

# --- sanity check on the kernel build dir ---------------------------------
ifeq ($(wildcard $(KDIR)/Makefile),)
  $(error kernel build dir '$(KDIR)' not found. Install the headers for \
          '$(KVER)' (pacman -S linux-cachyos-headers, or mount a matching \
          /usr/lib/modules/$(KVER)/build), or pass KVER=<release> / KDIR=<path>)
endif

# What to report in the build banner, so the toolchain is never a mystery.
ifeq ($(strip $(DETECTED)),unknown)
  CC_SHOWN := gcc (assumed - set CC_FAMILY=)
else
  CC_SHOWN := $(DETECTED)
endif

.PHONY: all modules clean distclean modules_install modules_prepare help

# `all` must reach `modules`: kbuild's own `all` target means vmlinux, which
# an out-of-tree module tree cannot build. No recipe here on purpose, so that
# including this file from kbuild cannot recurse.
all: modules

modules clean distclean modules_install modules_prepare:
	@echo "  KVER     $(KVER)"
	@echo "  KDIR     $(KDIR)"
	@echo "  CC       $(CC_SHOWN)"
ifeq ($(DETECT_W),warn)
	@echo "  WARNING  could not determine the target kernel's compiler family;"
	@echo "           defaulting to gcc. Override with CC_FAMILY=clang or CC_FAMILY=gcc."
endif
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(LLVM_FLAG) $(KBUILD_FLAGS) $@

help:
	@echo 'Targets: all (default), modules, clean, distclean, modules_install,'
	@echo '         modules_prepare, help'
	@echo
	@echo 'Vars:    KVER=<release>       kernel release to build against'
	@echo '                               (default: $$(uname -r) = $(KVER))'
	@echo '         KDIR=<path>          kernel build dir    (default: $(KDIR))'
	@echo '         CC_FAMILY=clang|gcc  override compiler auto-detection'
	@echo '         KBUILD_FLAGS="<f>"   extra kbuild flags   (e.g. W=1, V=1)'
