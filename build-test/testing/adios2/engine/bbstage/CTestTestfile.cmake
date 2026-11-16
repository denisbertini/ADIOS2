# CMake generated Testfile for 
# Source directory: /home/denis/work/io/ADIOS2/testing/adios2/engine/bbstage
# Build directory: /home/denis/work/io/ADIOS2/build-test/testing/adios2/engine/bbstage
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(Engine.Bbstage.WriteRead "/opt/intel/oneapi/mpi/2021.18/bin/mpiexec" "-n" "2" "/home/denis/work/io/ADIOS2/build-test/bin/Test.Engine.Bbstage.WriteRead")
set_tests_properties(Engine.Bbstage.WriteRead PROPERTIES  _BACKTRACE_TRIPLES "/home/denis/work/io/ADIOS2/testing/adios2/engine/bbstage/CMakeLists.txt;24;add_test;/home/denis/work/io/ADIOS2/testing/adios2/engine/bbstage/CMakeLists.txt;0;")
