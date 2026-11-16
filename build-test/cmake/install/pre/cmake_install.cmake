# Install script for directory: /home/denis/work/io/ADIOS2/cmake/install/pre

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "adios2_core-development" OR NOT CMAKE_INSTALL_COMPONENT)
  
  message("Pre-installation cleanup of CMake files")
  file(REMOVE_RECURSE "/usr/local/lib/cmake/adios2")

endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "adios2_core-development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/adios2" TYPE FILE FILES
    "/home/denis/work/io/ADIOS2/cmake/FindCaliper.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindCrayDRC.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindDAOS.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindDataSpaces.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindHDF5.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindIME.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindLIBFABRIC.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindMARGO.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindMERCURY.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindMPI.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindPkgConfig.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindPython.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindPythonModule.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindSZ.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindSodium.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindUCX.cmake"
    "/home/denis/work/io/ADIOS2/cmake/FindZeroMQ.cmake"
    "/home/denis/work/io/ADIOS2/cmake/Findpugixml.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "adios2_core-development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/adios2/upstream" TYPE DIRECTORY FILES "/home/denis/work/io/ADIOS2/cmake/upstream/")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/denis/work/io/ADIOS2/build-test/cmake/install/pre/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
