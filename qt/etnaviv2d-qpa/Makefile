# Makefile — etnaviv2d QPA plugin tests
#
# The plugin itself is built directly with qetnaviv2d.pro (qmake qetnaviv2d.pro
# && make) - no wrapper needed. This Makefile only builds the standalone
# benchmark test, which needs an IN-TREE Qt5 build directory (not a
# staged/installed Qt SDK) for its private headers - a staged qmake doesn't
# expose those.
#
# Build:
#   make QT_BUILD=/path/to/qt5base/build/tree
#
#   QT_BUILD must be a Qt5 *build* tree (i.e. you ran `configure && make`
#   there yourself), not an installed/staged Qt.
#
# Targets:
#   make all/tests   — build etnaviv2d_benchmark
#   make clean       — remove build output (does NOT clean Qt build tree)

QT_BUILD  ?=
ifeq ($(strip $(QT_BUILD)),)
$(error QT_BUILD is not set - point it at a Qt5 build tree, e.g. \
  make QT_BUILD=/path/to/qt5base/build/tree)
endif
QMAKE := $(QT_BUILD)/bin/qmake

BUILD_T_BENCH = build/etnaviv2d_benchmark

.PHONY: all tests clean etnaviv2d_benchmark

all: tests

tests: etnaviv2d_benchmark

etnaviv2d_benchmark: $(BUILD_T_BENCH)/Makefile
	$(MAKE) -C $(BUILD_T_BENCH)

$(BUILD_T_BENCH)/Makefile: tests/etnaviv2d_benchmark/etnaviv2d_benchmark.pro | $(BUILD_T_BENCH)
	cd $(BUILD_T_BENCH) && $(QMAKE) $(CURDIR)/tests/etnaviv2d_benchmark/etnaviv2d_benchmark.pro

$(BUILD_T_BENCH):
	mkdir -p $@

# -----------------------------------------------------------------------
# Clean: only our build dirs; does NOT touch the Qt build tree
# -----------------------------------------------------------------------
clean:
	rm -rf build
