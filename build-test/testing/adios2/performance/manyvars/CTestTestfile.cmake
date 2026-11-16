# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/performance/manyvars
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/performance/manyvars
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Performance.*/TestManyVars.DontRedefineVars/*.MPI "/opt/intel/oneapi/mpi/2021.18/bin/mpiexec" "-n" "12" "/home/denis/work/io/ADIOS2/build-test/bin/Test.Performance.ManyVars.MPI" "--gtest_filter=*/TestManyVars.DontRedefineVars/*")
set_tests_properties(Performance.*/TestManyVars.DontRedefineVars/*.MPI PROPERTIES  PROCESSORS "12" _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;101;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/performance/manyvars/CMakeLists.txt;11;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/performance/manyvars/CMakeLists.txt;0;")
