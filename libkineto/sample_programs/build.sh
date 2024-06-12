#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

g++  \
  -g3 \
  -O0 -MMD -MP -fPIC\
  -DFMT_USE_FLOAT128=0 \
  kineto_mupti_profiler.cpp \
  -o main \
  kplay_mu.o \
  /usr/local/lib/libkineto.a \
  -I/usr/local/musa/include \
  -I../third_party/fmt/include \
  -I/usr/local/include/kineto \
  -L/usr/local/lib \
  -L/usr/local/musa/lib \
  -lpthread \
  -lmusart \
  -lmusa \
  -lmupti