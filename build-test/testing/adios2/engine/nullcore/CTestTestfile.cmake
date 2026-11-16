# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/engine/nullcore
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/engine/nullcore
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Engine.NullCore.NullCoreWriteTest.ADIOS2NullCoreWrite1D8.Serial "/home/denis/work/io/ADIOS2/build-test/bin/Test.Engine.NullCore.Write.Serial" "--gtest_filter=NullCoreWriteTest.ADIOS2NullCoreWrite1D8")
set_tests_properties(Engine.NullCore.NullCoreWriteTest.ADIOS2NullCoreWrite1D8.Serial PROPERTIES  _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;83;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/engine/nullcore/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/engine/nullcore/CMakeLists.txt;0;")
add_test(Engine.NullCore.NullCoreWriteTest.ADIOS2NullCoreWrite1D8.MPI "/opt/intel/oneapi/mpi/2021.18/bin/mpiexec" "-n" "12" "/home/denis/work/io/ADIOS2/build-test/bin/Test.Engine.NullCore.Write.MPI" "--gtest_filter=NullCoreWriteTest.ADIOS2NullCoreWrite1D8")
set_tests_properties(Engine.NullCore.NullCoreWriteTest.ADIOS2NullCoreWrite1D8.MPI PROPERTIES  PROCESSORS "12" _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;101;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/engine/nullcore/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/engine/nullcore/CMakeLists.txt;0;")
