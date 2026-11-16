# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/backward_compatibility
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/backward_compatibility
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Engine.BP.*/BP4ReadOldCompressedP.Read/*.BP4.Serial "/home/denis/work/io/ADIOS2/build-test/bin/Test.Engine.BP.ReadOldCompressed.Serial" "--gtest_filter=*/BP4ReadOldCompressedP.Read/*" "BP4")
set_tests_properties(Engine.BP.*/BP4ReadOldCompressedP.Read/*.BP4.Serial PROPERTIES  WORKING_DIRECTORY "/home/denis/work/io/ADIOS2/testing/adios2/backward_compatibility" _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;83;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/backward_compatibility/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/backward_compatibility/CMakeLists.txt;0;")
