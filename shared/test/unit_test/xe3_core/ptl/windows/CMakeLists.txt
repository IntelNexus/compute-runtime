#
# Copyright (C) 2025 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

if(WIN32)
  target_sources(neo_shared_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/product_helper_tests_ptl_windows.cpp)
endif()

