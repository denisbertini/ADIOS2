# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/performance/query
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/performance/query
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Performance.BPQueryTest.BP5.Serial "/home/denis/work/io/ADIOS2/build-test/bin/Test.Performance.Query.Serial" "--gtest_filter=BPQueryTest.BP5")
set_tests_properties(Performance.BPQueryTest.BP5.Serial PROPERTIES  _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;83;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;0;")
add_test(Performance.BPQueryTest.BP4.Serial "/home/denis/work/io/ADIOS2/build-test/bin/Test.Performance.Query.Serial" "--gtest_filter=BPQueryTest.BP4")
set_tests_properties(Performance.BPQueryTest.BP4.Serial PROPERTIES  _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;83;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;0;")
add_test(Performance.BPQueryTest.BP5.MPI "/opt/intel/oneapi/mpi/2021.18/bin/mpiexec" "-n" "12" "/home/denis/work/io/ADIOS2/build-test/bin/Test.Performance.Query.MPI" "--gtest_filter=BPQueryTest.BP5")
set_tests_properties(Performance.BPQueryTest.BP5.MPI PROPERTIES  PROCESSORS "12" _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;101;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;0;")
add_test(Performance.BPQueryTest.BP4.MPI "/opt/intel/oneapi/mpi/2021.18/bin/mpiexec" "-n" "12" "/home/denis/work/io/ADIOS2/build-test/bin/Test.Performance.Query.MPI" "--gtest_filter=BPQueryTest.BP4")
set_tests_properties(Performance.BPQueryTest.BP4.MPI PROPERTIES  PROCESSORS "12" _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/cmake/upstream/GoogleTest.cmake;227;add_test;/home/denis/work/io/ADIOS2/testing/CMakeLists.txt;101;gtest_add_tests;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;10;gtest_add_tests_helper;/home/denis/work/io/ADIOS2/testing/adios2/performance/query/CMakeLists.txt;0;")
