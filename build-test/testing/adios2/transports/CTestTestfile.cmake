# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/transports
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/transports
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Transports.*/BufferTest.WriteRead/*.Serial "/home/denis/work/io/ADIOS2/build-test/bin/Test.Transports.File.Serial" "--gtest_filter=*/BufferTest.WriteRead/*")
set_tests_properties(Transports.*/BufferTest.WriteRead/*.Serial PROPERTIES  _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;83;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/transports/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/transports/CMakeLists.txt;0;")
