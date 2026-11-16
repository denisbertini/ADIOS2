# SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
#
# SPDX-License-Identifier: Apache-2.0

set(_ADIOS2_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
list(INSERT CMAKE_MODULE_PATH 0 "/home/denis/work/io/ADIOS2/cmake")

if(NOT ON)
  set(atl_DIR /home/denis/work/io/ADIOS2/build-test/thirdparty/atl/atl)
  set(dill_DIR /home/denis/work/io/ADIOS2/build-test/thirdparty/dill/dill)
  set(ffs_DIR /home/denis/work/io/ADIOS2/build-test/thirdparty/ffs/ffs)
endif()

if(TRUE)
  set(EVPath_DIR /home/denis/work/io/ADIOS2/build-test/thirdparty/EVPath/EVPath)
endif()

set(${CMAKE_FIND_PACKAGE_NAME}_CONFIG "${CMAKE_CURRENT_LIST_FILE}")
include("${CMAKE_CURRENT_LIST_DIR}/adios2-config-common.cmake")

set(CMAKE_MODULE_PATH ${_ADIOS2_CMAKE_MODULE_PATH})
unset(_ADIOS2_CMAKE_MODULE_PATH)
