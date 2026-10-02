# Makefile — libetna2d (GC520 2D op builder) + self-test.
#
# Builds the reusable, Qt-independent 2D HAL as both a shared and a static
# library, plus an on-target acceptance test. Cross-compiled for MA35D1.
#
# Standalone build, two ways:
#
#   A) Toolchain already on PATH and exports CC itself (e.g. after
#      `source <sdk>/environment-setup-...`):
#        make
#
#   B) Toolchain installed but not sourced - point at it explicitly:
#        make CROSS_COMPILE=aarch64-linux-gnu- \
#             SYSROOT=/path/to/target/sysroot
#
#      SYSROOT must contain the target's /usr/include, /usr/include/libdrm,
#      and /usr/lib/libdrm_etnaviv.so (i.e. libdrm + libdrm_etnaviv already
#      built/staged for your target). Run `make check-env` first to verify.
#
# Artifacts (all under build/):
#   build/libetna2d.so.1   build/libetna2d.so   build/libetna2d.a
#   build/etna2d_selftest
#
# Deploy build/libetna2d.so.1 + build/etna2d_selftest to the target and run:
#   ETNA2D_VERBOSE=1 ./etna2d_selftest /dev/dri/renderD128

# ---- Cross-toolchain / sysroot -------------------------------------------
# Both empty by default (native/host build). Override for cross builds:
#   CROSS_COMPILE=aarch64-linux-gnu-  SYSROOT=/path/to/sysroot
CROSS_COMPILE ?=
SYSROOT       ?=

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
AR      ?= $(CROSS_COMPILE)ar

SYSROOT_CFLAGS  := --sysroot=$(SYSROOT)
SYSROOT_LDFLAGS := --sysroot=$(SYSROOT)

# Source of the etnaviv register/UAPI headers (state_2d.xml.h,
# etnaviv_drm.h, etc). Vendored in-tree (third_party/libdrm-etnaviv-private/,
# see its NOTICE.md for provenance/licenses) so this builds standalone with
# no external scaffolding.
LIBDRM  ?= $(CURDIR)/third_party/libdrm-etnaviv-private
ETNA_H  := $(LIBDRM)/tests/etnaviv

# ---- Output directory ----------------------------------------------------
BUILDDIR := build
$(shell mkdir -p $(BUILDDIR))

# ---- Flags ---------------------------------------------------------------
# AM_CFLAGS/AM_LDFLAGS are this package's own required flags (its own
# includes, -fPIC, its own --sysroot). CFLAGS/LDFLAGS are reserved for the
# caller to set optimization/hardening flags.
AM_CFLAGS  := -Wall -Wextra -std=gnu11 -fPIC \
              $(SYSROOT_CFLAGS) \
              -I$(SYSROOT)/usr/include/libdrm \
              -I$(ETNA_H) \
              -I$(LIBDRM)/etnaviv \
              -I.

AM_LDFLAGS := $(SYSROOT_LDFLAGS)

LDLIBS  := -ldrm_etnaviv -ldrm

SONAME  := $(BUILDDIR)/libetna2d.so.1
SHARED  := $(BUILDDIR)/libetna2d.so
STATIC  := $(BUILDDIR)/libetna2d.a
OBJ     := $(BUILDDIR)/libetna2d.o

.PHONY: all clean check-env
all: $(SONAME) $(SHARED) $(STATIC) $(BUILDDIR)/etna2d_selftest

$(OBJ): libetna2d.c etna2d.h
	$(CC) $(AM_CFLAGS) $(CFLAGS) -c $< -o $@

# Shared library with a proper SONAME.
$(SONAME): $(OBJ)
	$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) \
		-shared -Wl,-soname,libetna2d.so.1 \
		$< $(LDLIBS) -o $@

$(SHARED): $(SONAME)
	ln -sf libetna2d.so.1 $(SHARED)

$(STATIC): $(OBJ)
	$(AR) rcs $@ $<

# Self-test links the static lib for a self-contained on-target binary.
$(BUILDDIR)/etna2d_selftest: etna2d_selftest.c $(STATIC)
	$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) \
		$< $(STATIC) $(LDLIBS) -o $@

check-env:
	@echo "CC      = $(CC)"
	@echo "SYSROOT = $(SYSROOT)$(if $(SYSROOT),, (none - native/host build))"
	@echo "LIBDRM  = $(LIBDRM)"
	@command -v "$(CC)" >/dev/null 2>&1 || { echo "ERROR: CC ($(CC)) not found - source your SDK env, or set CROSS_COMPILE=<prefix->"; exit 1; }
	@test -f "$(ETNA_H)/state_2d.xml.h" || { echo "ERROR: etnaviv headers not under $(ETNA_H)"; exit 1; }
	@if [ -n "$(SYSROOT)" ]; then \
		test -f "$(SYSROOT)/usr/lib/libdrm_etnaviv.so" || { echo "ERROR: libdrm_etnaviv not found under $(SYSROOT)/usr/lib - build/stage libdrm_etnaviv for your target first"; exit 1; }; \
	else \
		ldconfig -p 2>/dev/null | grep -q libdrm_etnaviv || \
		echo "WARNING: libdrm_etnaviv not found on this host - install it, or set SYSROOT to a target sysroot that has it"; \
	fi
	@echo "environment OK"

clean:
	rm -rf $(BUILDDIR)
