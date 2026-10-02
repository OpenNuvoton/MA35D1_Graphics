# Makefile — nu-hmi-controller.so custom ivi-shell HMI controller
#
# Standalone build:
#   make CC=aarch64-linux-gnu-gcc \
#        SYSROOT=/path/to/target/sysroot \
#        WESTON_MAJOR=13
#
#   SYSROOT must contain the target's libweston-<N> and wayland-server
#   headers/libs (i.e. Weston already built for your target). WESTON_MAJOR
#   must match the libweston major version in that sysroot (10 for Weston
#   10.x, 13 for Weston 13.x, etc).
#
# Deploy:
#   deploy build/nu-hmi-controller.so to target's /usr/lib/weston/
#
# The plugin is loaded by Weston via weston.ini:
#   [core]
#   modules=nu-hmi-controller.so

SYSROOT ?=

BUILD_DIR = build

# libweston's major-version-suffixed include dir/soname.
WESTON_MAJOR ?= 10

# AM_CFLAGS/AM_LDFLAGS are this package's own required flags. CFLAGS/LDFLAGS
# are reserved for the caller to set optimization/hardening flags.
AM_CFLAGS := \
    -Wall -Wextra \
    -fPIC \
    -I$(SYSROOT)/usr/include \
    -I$(SYSROOT)/usr/include/pixman-1 \
    -I$(SYSROOT)/usr/include/libweston-$(WESTON_MAJOR) \
    -I$(SYSROOT)/usr/include/weston

AM_LDFLAGS := \
    -shared \
    -L$(SYSROOT)/usr/lib \
    -Wl,-rpath-link,$(SYSROOT)/usr/lib \
    -lweston-$(WESTON_MAJOR) \
    -lwayland-server

TARGET = $(BUILD_DIR)/nu-hmi-controller.so
SRC    = nu-hmi-controller.c

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)
