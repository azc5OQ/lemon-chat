#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "maxminddb::maxminddb" for configuration "Debug"
set_property(TARGET maxminddb::maxminddb APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(maxminddb::maxminddb PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/maxminddb.lib"
  )

list(APPEND _cmake_import_check_targets maxminddb::maxminddb )
list(APPEND _cmake_import_check_files_for_maxminddb::maxminddb "${_IMPORT_PREFIX}/lib/maxminddb.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
