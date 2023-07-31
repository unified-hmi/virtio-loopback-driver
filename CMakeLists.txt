# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
#


cmake_minimum_required(VERSION 3.2)
project(virtio-lo-driver VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(src)

