#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libSRTP::srtp2" for configuration "Release"
set_property(TARGET libSRTP::srtp2 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libSRTP::srtp2 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libsrtp2.a"
  )

list(APPEND _cmake_import_check_targets libSRTP::srtp2 )
list(APPEND _cmake_import_check_files_for_libSRTP::srtp2 "${_IMPORT_PREFIX}/lib/libsrtp2.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
