#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "adios2::adios2sys" for configuration "Release"
set_property(TARGET adios2::adios2sys APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(adios2::adios2sys PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libadios2sys.so"
  IMPORTED_SONAME_RELEASE "libadios2sys.so"
  )

list(APPEND _cmake_import_check_targets adios2::adios2sys )
list(APPEND _cmake_import_check_files_for_adios2::adios2sys "${_IMPORT_PREFIX}/lib/libadios2sys.so" )

# Import target "adios2::perfstubs" for configuration "Release"
set_property(TARGET adios2::perfstubs APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(adios2::perfstubs PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libadios2_perfstubs.so.2.12.0"
  IMPORTED_SONAME_RELEASE "libadios2_perfstubs.so.2.12"
  )

list(APPEND _cmake_import_check_targets adios2::perfstubs )
list(APPEND _cmake_import_check_files_for_adios2::perfstubs "${_IMPORT_PREFIX}/lib/libadios2_perfstubs.so.2.12.0" )

# Import target "adios2::core" for configuration "Release"
set_property(TARGET adios2::core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(adios2::core PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "adios2::adios2sys;sodium"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libadios2_core.so.2.12.0"
  IMPORTED_SONAME_RELEASE "libadios2_core.so.2.12"
  )

list(APPEND _cmake_import_check_targets adios2::core )
list(APPEND _cmake_import_check_files_for_adios2::core "${_IMPORT_PREFIX}/lib/libadios2_core.so.2.12.0" )

# Import target "adios2::core_mpi" for configuration "Release"
set_property(TARGET adios2::core_mpi APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(adios2::core_mpi PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libadios2_core_mpi.so.2.12.0"
  IMPORTED_SONAME_RELEASE "libadios2_core_mpi.so.2.12"
  )

list(APPEND _cmake_import_check_targets adios2::core_mpi )
list(APPEND _cmake_import_check_files_for_adios2::core_mpi "${_IMPORT_PREFIX}/lib/libadios2_core_mpi.so.2.12.0" )

# Import target "adios2::EncryptionOperator" for configuration "Release"
set_property(TARGET adios2::EncryptionOperator APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(adios2::EncryptionOperator PROPERTIES
  IMPORTED_COMMON_LANGUAGE_RUNTIME_RELEASE ""
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libEncryptionOperator.so"
  IMPORTED_NO_SONAME_RELEASE "TRUE"
  )

list(APPEND _cmake_import_check_targets adios2::EncryptionOperator )
list(APPEND _cmake_import_check_files_for_adios2::EncryptionOperator "${_IMPORT_PREFIX}/lib/libEncryptionOperator.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
