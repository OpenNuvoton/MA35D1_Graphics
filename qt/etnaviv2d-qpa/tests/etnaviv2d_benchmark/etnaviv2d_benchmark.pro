# etnaviv2d_benchmark — GPU vs CPU frame-rate benchmark
#
# Build once, run with two different QT_QPA_PLATFORM values to compare.

TARGET = etnaviv2d_benchmark

QT += widgets

SOURCES = etnaviv2d_benchmark.cpp

CONFIG += console
CONFIG -= app_bundle
