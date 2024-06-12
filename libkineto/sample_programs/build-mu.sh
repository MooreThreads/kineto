#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

mcc -x musa -fPIE -lmusart -lmusa -c kineto_playground.mu -o kplay_mu.o