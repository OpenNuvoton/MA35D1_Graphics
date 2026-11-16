# Makefile — etnaviv2d QPA plugin tests
#
# The plugin itself is built directly with qetnaviv2d.pro (qmake qetnaviv2d.pro
# && make) - no wrapper needed. This Makefile only builds the standalone
# benchmark test (etnaviv2d_benchmark), which only uses public QtWidgets API,
# so any Qt5 qmake works (in-tree build dir or staged/installed host qmake).
#
# Build:
#   make QT_BUILD=/path/to/qt5base/build/tree     (uses $(QT_BUILD)/bin/qmake)
#   make QMAKE=/path/to/qmake
#
# Run (--clip or --ops, see source header):
#   QT_QPA_PLATFORM=linuxfb ./etnaviv2d_benchmark --ops
#   QT_QPA_PLATFORM=etnaviv2d:plane=primary ./etnaviv2d_benchmark --clip
#
# Targets:
#   make all/tests   — build etnaviv2d_benchmark
#   make clean       — remove build output (does NOT clean Qt build tree)

QT_BUILD  ?=
ifeq ($(origin QMAKE),undefined)
ifeq ($(strip $(QT_BUILD)),)
$(error Set QT_BUILD=/path/to/qt5 (uses bin/qmake) or QMAKE=/path/to/qmake)
endif
QMAKE := $(QT_BUILD)/bin/qmake
endif

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
