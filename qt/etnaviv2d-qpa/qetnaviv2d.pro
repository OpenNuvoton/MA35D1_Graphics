# qetnaviv2d.pro — etnaviv2d QPA plugin, standalone build file
#
# No wayland-scanner codegen step, no in-tree Qt build dependency. Suitable
# for Buildroot's qmake-package infra or any other out-of-tree staged-Qt
# build.
#
#   qmake qetnaviv2d.pro [ETNA2D_INCDIR=...] [ETNA2D_LIBDIR=...]
#   make -j$(nproc)
#
# The plugin lands in $$[QT_INSTALL_PLUGINS]/platforms/libqetnaviv2d.so.
#
# Tuning (all optional, default to the sibling ../../drm/libetna2d checkout):
#   ETNA2D_INCDIR  — path to etna2d.h                   (default: ../../drm/libetna2d)
#   ETNA2D_LIBDIR  — path to libetna2d.a / libetna2d.so (default: ../../drm/libetna2d/build)

TARGET = qetnaviv2d

DEFINES += QT_NO_FOREACH

# Build the frozen legacy-KMS path (drmModeSetCrtc / drmModePageFlip,
# primary-only) instead of the default atomic path:
#   qmake qetnaviv2d.pro LEGACY_KMS=1
!isEmpty(LEGACY_KMS):!equals(LEGACY_KMS, 0): DEFINES += ETNAVIV2D_LEGACY_KMS

QT += \
    core-private          \
    gui-private           \
    service_support-private       \
    eventdispatcher_support-private \
    fontdatabase_support-private  \
    fb_support-private

# kms_support-private pulls in the libdrm sysroot include path (drm.h, xf86drm.h, ...)
qtHaveModule(kms_support-private): QT += kms_support-private

# Input support — same conditional guard as linuxfb
qtHaveModule(input_support-private): QT += input_support-private

SOURCES = \
    src/main.cpp                         \
    src/qetnaviv2dintegration.cpp        \
    src/qetnaviv2dscreen.cpp             \
    src/qetnaviv2dblitter.cpp            \
    src/qetnaviv2dbackingstore.cpp       \
    src/qetnaviv2dwaylandtransport.cpp

HEADERS = \
    src/qetnaviv2dintegration.h          \
    src/qetnaviv2dscreen.h               \
    src/qetnaviv2dblitter.h              \
    src/qetnaviv2dbackingstore.h         \
    src/qetnaviv2dwaylandtransport.h

INCLUDEPATH += $$PWD/src

# libetna2d (our userspace HAL)
isEmpty(ETNA2D_INCDIR): ETNA2D_INCDIR = $$PWD/../../drm/libetna2d
isEmpty(ETNA2D_LIBDIR): ETNA2D_LIBDIR = $$PWD/../../drm/libetna2d/build
INCLUDEPATH += $$ETNA2D_INCDIR
LIBS        += -L$$ETNA2D_LIBDIR -letna2d

# libdrm (BO alloc, PRIME, etnaviv cmd_stream, KMS ioctls) — present in sysroot
LIBS += -ldrm -ldrm_etnaviv

# -----------------------------------------------------------------------
# Wayland protocol bindings (xdg-shell, linux-dmabuf-unstable-v1,
# ivi-application) — pre-generated and checked in under
# src/wayland-protocols/, instead of invoking wayland-scanner at build time.
#
# Regenerate only if bumping the wayland-protocols or Weston version this
# project targets:
#   wayland-scanner private-code   <xml> src/wayland-protocols/<name>-protocol.c
#   wayland-scanner client-header  <xml> src/wayland-protocols/<name>-client-protocol.h
# -----------------------------------------------------------------------
SOURCES += \
    src/wayland-protocols/xdg-shell-protocol.c \
    src/wayland-protocols/linux-dmabuf-unstable-v1-protocol.c \
    src/wayland-protocols/ivi-application-protocol.c

HEADERS += \
    src/wayland-protocols/xdg-shell-client-protocol.h \
    src/wayland-protocols/linux-dmabuf-unstable-v1-client-protocol.h \
    src/wayland-protocols/ivi-application-client-protocol.h

INCLUDEPATH += $$PWD/src/wayland-protocols

# libwayland-client — headers/lib already resolve through the normal
# cross-compile sysroot (baked into the mkspec), same as libdrm above.
LIBS += -lwayland-client

OTHER_FILES += src/etnaviv2d.json

# Built as a standard out-of-tree qmake "external plugin": a shared lib
# tagged CONFIG+=plugin, installed directly into the QPA plugin directory
# of the target Qt install ($$[QT_INSTALL_PLUGINS]).
TEMPLATE = lib
CONFIG  += plugin

target.path = $$[QT_INSTALL_PLUGINS]/platforms
INSTALLS   += target
