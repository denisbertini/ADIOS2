# SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
#
# SPDX-License-Identifier: Apache-2.0

#------------------------------------------------------------------------------#
# Distributed under the OSI-approved Apache License, Version 2.0.  See
# accompanying file Copyright.txt for details.
#------------------------------------------------------------------------------#

if(NOT WIN32)
  add_executable(Test.Engine.Bbview.WriteRead TestBbviewWriteRead.cpp)

  if(ADIOS2_HAVE_MPI)
    target_link_libraries(Test.Engine.Bbview.WriteRead adios2::thirdparty::gtest
      adios2::cxx_mpi adios2_core_mpi MPI::MPI_CXX)
    set(cmd_executor
      ${MPIEXEC_EXECUTABLE} ${MPIEXEC_EXTRA_FLAGS} ${MPIEXEC_NUMPROC_FLAG} 2)
  else()
    target_link_libraries(Test.Engine.Bbview.WriteRead adios2::thirdparty::gtest
      adios2::cxx adios2_core)
    set(cmd_executor)
  endif()

  add_test(NAME Engine.Bbview.WriteRead
    COMMAND ${cmd_executor} $<TARGET_FILE:Test.Engine.Bbview.WriteRead>)
endif()
