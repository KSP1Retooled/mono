# Cross-builds for arm64 Linux with the host's clang, and runs the result under
# qemu-user. cmake/cross/README.md has the command line.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(MONO_CROSS_SYSROOT "$ENV{MONO_CROSS_SYSROOT}" CACHE PATH
    "arm64 sysroot to compile, link and run against")

# try_compile runs this file again without this build's cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MONO_CROSS_SYSROOT)

if(NOT EXISTS "${MONO_CROSS_SYSROOT}/lib/ld-linux-aarch64.so.1")
  message(FATAL_ERROR
    "MONO_CROSS_SYSROOT is \"${MONO_CROSS_SYSROOT}\", which holds no arm64 loader; "
    "fill a directory with cmake/cross/make-arm64-sysroot.sh and name it")
endif()

set(CMAKE_SYSROOT "${MONO_CROSS_SYSROOT}")

foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_COMPILER_TARGET aarch64-linux-gnu)
endforeach()
set(CMAKE_C_COMPILER   clang   CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "")

# The host's own ld links amd64 alone.
set(CMAKE_EXE_LINKER_FLAGS_INIT    -fuse-ld=lld)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_MODULE_LINKER_FLAGS_INIT -fuse-ld=lld)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# test-memfuncs/MemFuncs.MemmoveAtomic takes 180s under qemu on a fast machine,
# which leaves the 300s default too little room on a slow one.
set(MONO_CTEST_TIMEOUT 1800 CACHE STRING
    "Seconds a CTest test that names no budget of its own may run")

# QEMU_LD_PREFIX rather than -L, because a child process inherits it.
find_program(MONO_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(MONO_QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR
      /usr/bin/env "QEMU_LD_PREFIX=${MONO_CROSS_SYSROOT}" "${MONO_QEMU_AARCH64}")
endif()
