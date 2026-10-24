# `anygba build` end to end: ROM -> program with the ROM embedded, which then runs with no
# ROM argument and passes the test ROM's checks.
#   cmake -DDRIVER=<anygba> -DPREFIX=<install prefix> -DROM=<rom> -DWORK=<dir> -DCONFIG=<cfg>
#         -DGENERATOR=<gen> -P driver-build.cmake
foreach(var DRIVER PREFIX ROM WORK CONFIG GENERATOR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
set(gen_args --generator "${GENERATOR}")
execute_process(
  COMMAND "${DRIVER}" build "${ROM}" --prefix "${PREFIX}" -o "${WORK}" --name "Driver Test" --build-type ${CONFIG}
    --cmake "${CMAKE_COMMAND}" ${gen_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "anygba build failed (${rc}):\n${out}\n${err}")
endif()
if(CMAKE_HOST_WIN32)
  set(exe "${WORK}/Driver Test.exe")
elseif(CMAKE_HOST_APPLE)
  set(exe "${WORK}/Driver Test.app/Contents/MacOS/Driver Test")
else()
  set(exe "${WORK}/Driver Test")
endif()
if(NOT EXISTS "${exe}")
  message(FATAL_ERROR "anygba build did not write ${exe}:\n${out}")
endif()
execute_process(
  COMMAND "${exe}" --headless --exit-on-idle --frames 600 --no-miss-log --save-dir "${WORK}/saves"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "r12=00000000")
  message(FATAL_ERROR "the built program failed (${rc}):\n${out}\n${err}")
endif()
message(STATUS "built and ran ${exe}")
