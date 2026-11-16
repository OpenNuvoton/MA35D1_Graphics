# etnaviv2d_benchmark — Qt 2D benchmark (sprite clip + 2D ops table)
#
# Build once, run with two different QT_QPA_PLATFORM values to compare
# (linuxfb vs etnaviv2d). Only needs QtWidgets public API.

TARGET = etnaviv2d_benchmark

QT += widgets

SOURCES = etnaviv2d_benchmark.cpp

CONFIG += console
CONFIG -= app_bundle
