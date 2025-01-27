#=========================== begin_copyright_notice ============================
#
# Copyright (C) 2017-2021 Intel Corporation
#
# SPDX-License-Identifier: MIT
#
#============================ end_copyright_notice =============================

# ROOT
cmake_minimum_required(VERSION 2.8)

if (NOT IGC_BUILD)
enable_testing()
endif()

project(IGA)

include (BuildSetup.cmake)
include (BuildFunctions.cmake)

# set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set (CMAKE_CXX_STANDARD 14)

# do static build for IGA standalone build
set(LINK_AS_STATIC_LIB TRUE)

include_directories(include)
include_directories("../../../inc")

set(GED_BRANCH GED_external)

add_subdirectory(GEDLibrary/${GED_BRANCH})
add_subdirectory(IGALibrary)
add_subdirectory(IGAExe)
