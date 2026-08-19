if(NOT EXISTS "C:/Users/babikp/Desktop/lala/lemon-chat/server-source-code/third-party/libmaxminddb-1.12.2/install_manifest.txt")
  message(FATAL_ERROR "Cannot find install manifest: C:/Users/babikp/Desktop/lala/lemon-chat/server-source-code/third-party/libmaxminddb-1.12.2/install_manifest.txt")
endif()

file(READ "C:/Users/babikp/Desktop/lala/lemon-chat/server-source-code/third-party/libmaxminddb-1.12.2/install_manifest.txt" files)
string(REGEX REPLACE "\n" ";" files "${files}")
foreach(file ${files})
  message(STATUS "Uninstalling $ENV{DESTDIR}${file}")
  if(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    execute_process(
      COMMAND "C:/Users/babikp/Downloads/w64devkit/bin/cmake.exe" -E remove "$ENV{DESTDIR}${file}"
      OUTPUT_VARIABLE rm_out
      RESULT_VARIABLE rm_retval
      )
    if(NOT "${rm_retval}" STREQUAL 0)
      message(FATAL_ERROR "Problem when removing $ENV{DESTDIR}${file}")
    endif()
  else(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    message(STATUS "File $ENV{DESTDIR}${file} does not exist.")
  endif()
endforeach()
