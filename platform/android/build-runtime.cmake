# Cross-builds the AnyGBA runtime for Android and installs it as <PREFIX>/android/<abi>, the
# layout `anygba build --target android` looks for.
#
#   cmake -DPREFIX=<anygba install prefix> [-DNDK=<ndk dir>] [-DABIS="arm64-v8a;x86_64"]
#         [-DBUILD_ROOT=<scratch dir>] [-DGENERATOR=Ninja] -P platform/android/build-runtime.cmake
#
# NDK defaults to ANDROID_NDK_HOME, ANDROID_NDK_ROOT, then the newest <sdk>/ndk/* of
# ANDROID_HOME / ANDROID_SDK_ROOT.
cmake_minimum_required(VERSION 3.28)

if(NOT PREFIX)
  message(FATAL_ERROR "PREFIX is required")
endif()
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT ABIS)
  set(ABIS arm64-v8a x86_64)
endif()
if(NOT BUILD_ROOT)
  set(BUILD_ROOT "${source}/out/android")
endif()
if(NOT GENERATOR)
  set(GENERATOR Ninja)
endif()

if(NOT NDK)
  foreach(var ANDROID_NDK_HOME ANDROID_NDK_ROOT)
    if(DEFINED ENV{${var}} AND EXISTS "$ENV{${var}}/build/cmake/android.toolchain.cmake")
      set(NDK "$ENV{${var}}")
      break()
    endif()
  endforeach()
endif()
if(NOT NDK)
  foreach(var ANDROID_HOME ANDROID_SDK_ROOT)
    if(DEFINED ENV{${var}})
      file(GLOB ndks LIST_DIRECTORIES true "$ENV{${var}}/ndk/*")
      if(ndks)
        list(SORT ndks COMPARE NATURAL ORDER DESCENDING)
        list(GET ndks 0 NDK)
        break()
      endif()
    endif()
  endforeach()
endif()
if(NOT NDK OR NOT EXISTS "${NDK}/build/cmake/android.toolchain.cmake")
  message(FATAL_ERROR "no Android NDK found; pass -DNDK=<dir> or set ANDROID_NDK_HOME")
endif()
file(TO_CMAKE_PATH "${NDK}" NDK)
file(TO_CMAKE_PATH "${PREFIX}" PREFIX)
message(STATUS "NDK: ${NDK}")

foreach(abi IN LISTS ABIS)
  set(bin "${BUILD_ROOT}/${abi}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -Wno-deprecated -S "${source}" -B "${bin}" -G "${GENERATOR}"
      "-DCMAKE_TOOLCHAIN_FILE=${NDK}/build/cmake/android.toolchain.cmake"
      -DANDROID_ABI=${abi} -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static
      -DCMAKE_BUILD_TYPE=Release -DANYGBA_BUILD_TESTS=OFF
      "-DCMAKE_INSTALL_PREFIX=${PREFIX}/android/${abi}"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "configure for ${abi} failed")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${bin}" --parallel RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "build for ${abi} failed")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --install "${bin}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install for ${abi} failed")
  endif()
endforeach()
